#if defined _state_bait_included_
    #endinput
#endif
#define _state_bait_included_

#include <sourcemod>
#include <sdktools>
#include <sdkhooks>
#include <left4dhooks>

#include "../setup.inc"
#include "../stocks.inc"
#include "./state.inc"

#define STATE_BAIT_NAME         "STATE_BAIT"
#define TARGET_PURSUIT_FACTOR   1.2       // 面对手持近战目标 Charger 后跳时, 此时目标若朝着 Charger 移动, Charger 后跳速度的加速倍数
#define TARGET_RETREAT_FACTOR   0.5       // 面对手持近战目标 Charger 后跳时, 此时目标若远离 Charger 移动, Charger 后跳速度的减速倍数
#define MELEE_PRESS_MIN_SPEED   50.0      // 目标朝 Charger 移动速度超过该值时, 视为主动进入近战危险区
#define MELEE_EMERGENCY_BUFFER  20.0      // 目标距离 Charger 小于 (meleeRange + 这个值) 时, 视为非常危险, 无论目标速度如何都尝试后跳
#define BAIT_LOST_SIGHT_GRACE   3.0       // 连续丢失目标视野超过该时间才退出 Bait

// =========================================================================
// State Bait 接近目标时的博弈状态 (dist < g_cvBhopMinDist.FloatValue 时进入 BAIT_STATE)
// 注册状态函数
// =========================================================================
stock void RegisterState_Bait() {
    ChargerState stateBait;
    strcopy(stateBait.name, sizeof(stateBait.name), "State_Bait");
    stateBait.OnEnter = StateBait_OnEnter;
    stateBait.OnUpdate = StateBait_OnUpdate;
    stateBait.OnExit = StateBait_OnExit;
    g_ChargerStates.PushArray(stateBait);
}

// ============================================================
// OnEnter 函数
// ============================================================
stock void StateBait_OnEnter(int client) {
    g_AiChargers[client].m_flLastVisibleTime = GetGameTime();
    stateLog.debugAll("\n========== Charger (%N) State Bait On Enter ===========", client);
}

// ============================================================
// OnUpdate 函数
// ============================================================
stock Action StateBait_OnUpdate(int client, int& buttons, float vel[3], float ang[3]) {
    if (!isAiCharger(client))
        return Plugin_Continue;
    if (IsPinningSurvivor(client))
        return Plugin_Continue;
    
    static int target;
    target = GetClientOfUserId(g_AiChargers[client].m_iTarget);
    if (!IsValidSurvivor(target) || !IsPlayerAlive(target)) {
        stateLog.debugAll("(%s) Charger (%N)'s target %d is invalid, transition to APPROACH STATE", STATE_BAIT_NAME, client, target);
        g_ChargerStateContext[client].transitionTo(CH_STATE_APPROACH);
        return Plugin_Continue;
    }

    // 计算 charger 当前到目标的距离
    static float pos[3], targetPos[3], targetEyePos[3], dist;
    GetClientAbsOrigin(client, pos);
    GetClientAbsOrigin(target, targetPos);
    GetClientEyePosition(target, targetEyePos);
    dist = getVectorDistance2D(pos, targetPos);
    // 是否可视目标
    static bool visible;
    visible = L4D2_IsVisibleToPlayer(client, TEAM_INFECTED, TEAM_SURVIVOR, 0, targetEyePos);
    if (visible) {
        g_AiChargers[client].m_flLastVisibleTime = GetGameTime();
    }
    // 目标是否正在看着 charger
    // 除了目标之外还有多少生还者正在看着 charger
    static bool isWatching, isOnGround;
    static int watchers;
    isOnGround = IsClientOnGround(client);
    isWatching = isTargetWatchingCharger(client, target, g_cvTargetWatchMaxDeg.FloatValue);
    watchers = isAnySurvivorWatchingCharger(client, target, g_cvTargetWatchMaxDeg.FloatValue);
    // 目标威胁类型
    static TargetThreatType threatType;
    threatType = getTargetThreatType(client, target);

    // 只在 Charger 落地后检查高度, 避免后跳在空中时因高度差而错误的退出 Bait 状态
    if (isOnGround && !isChargeHeightReachable(pos, targetPos)) {
        stateLog.debugAll("Charger (%N) bait target (%N) height diff %.2f > %.2f, transition to APPROACH STATE",
            client, target,FloatAbs(pos[2] - targetPos[2]), CHARGE_Z_THRESHOLD);
        
        g_ChargerStateContext[client].transitionTo(CH_STATE_APPROACH);
        return Plugin_Continue;
    }

    /*
    * Bait 可能在 Charger 仍处于上一次 Approach 连跳的空中阶段时触发 (在空中距离目标小于 BaitRange 进入 Bait 状态)
    * 之前的 Bait State 并没有修正空中速度, 如果此时不持续修正空中速度, Charger 会沿着旧的 Path/Strafe 速度从目标侧面飞走
    * 导致后续 Bait -> Locked -> Charging 时 Charger 已经飞出去很远, 需要从 Charging -> Approach 继续接近目标, 错过一次冲锋
    */
    static bool airLineCorrected;
    airLineCorrected = false;

    /*
    * 目标的威胁类型不是近战, 且 Charger 从 Approach 状态转为 Bait 状态但是最后一跳还没结束还在空中时
    * 进行空中速度修正, 避免 Charger 飞过目标
    */
    if (threatType != Threat_Melee && !isOnGround && visible) {
        airLineCorrected = preserveAirChargeLine(client, target, visible, AIR_CHARGE_LINE_CORR_LERP);
    }

    // ============================================================
    // 进入博弈状态, 说明距离正常威胁目标距离小于 g_cvBhopMinDist.FloatValue
    // 距离手持近战目标距离小于 melee_range + g_cvMeleeBaitZoneMaxRange.FloatValue
    // 这时候先阻止冲锋, 并尝试右键挥拳, 若后续满足冲锋条件, 转换到冲锋状态
    // ============================================================
    buttons |= IN_ATTACK2;
    setChargerAbilityCooldown(client, 1.0);

    // ============================================================
    // 优先行动检查, 检测是否有更好的可以直接冲锋的目标, 如果有, 直接转换为冲锋状态
    // 这避免了如果在与手持近战的目标博弈时, 另一个生还者在 charger 身后疯狂输出的行为
    // 若 charger 身后突然出现另一个生还者, 且 charger 此时距离到他更近且他无法躲避冲锋, 直接向他冲锋, 否则, 继续朝着原目标冲锋
    // ============================================================
    if (isOnGround) {
        static int betterTarget;
        betterTarget = getChargeUnavoidableTarget(client, target, pos, dist, g_cvBhopMinDist.FloatValue);
        if (IsValidSurvivor(betterTarget) && IsPlayerAlive(betterTarget) &&
            canCommitBaitCharge(client, betterTarget, visible)
        ) {
            g_AiChargers[client].m_iBetterTarget = GetClientUserId(betterTarget);
            transitionBaitToLocked(client, betterTarget, "Found closer unavoidable charge target, forcing charge!");
            return Plugin_Changed;
        }
    }

    // ============================================================
    // 检查是否需要退出博弈状态, 转移到接近状态
    // 1. 不可见目标时, 转换为接近状态
    // 2. 目标非手持近战, 且到目标距离大于 bhopMinDist + 容差距离, 转换为接近状态
    // 3. 目标手持近战, 且到目标距离大于 meleeRange + baitZoneMax, 出了博弈区, 转换为接近状态
    // ============================================================
    if (shouldExitBaitState(client, target, dist, visible, threatType)) {
        /*
        * Bait 状态下空中速度修正成功, 但是已经达到了退出 Bait 状态的条件, 是否直接退出 Bait 转给 Approach 状态空中修正?
        * 理论上不应该, 这里返回 Plugin_Changed 的语义是当前 Charger 在 Bait 状态的空中, 然后已经对空中速度进行修正
        * Charger 在空中的时候先让其继续修正, 暂时不要在同一帧之内切换状态, 否则在 Bait 中做了速度修正, 然后又退出 Bait, 速度修正就被打断
        * 所以等到 Charger 落地, 再切换到 Approach 状态
        */
        if (airLineCorrected) {
            return Plugin_Changed;
        }

        g_ChargerStateContext[client].transitionTo(CH_STATE_APPROACH);
        return Plugin_Continue;
    }

    // ============================================================
    // 如果当前目标已经被其他强控特感控制, hunter, charger
    // 中止转换到 CHARGING STATE, 停留在 BAIT 状态, 持续对目标挥拳
    // ============================================================
    if (isSurvivorPinned(target)) {
        // stateLog.debugAll("Charger (%N)'s target (%N) is pinned! Suspending charge, melee to it", client, target);
        return Plugin_Continue;
    }

    // ============================================================
    // 根据威胁类型分别处理
    // ============================================================
    static Action resAction;
    if (threatType == Threat_Melee) {
        /*
        * handleMeleeThreat 的返回值为 Plugin_Changed
        * 1. 表示当前 Charger 状态已经转换到 CHARGING STATE, 准备冲锋
        * 2. 表示 Charger 不满足冲锋条件但是允许后跳, performBackwardBhop 后跳输入之后, 结束本帧
        */
        resAction = handleMeleeThreat(client, target, dist, visible, isWatching, watchers, buttons);
        if (resAction != Plugin_Continue) {
            return resAction;
        }
        /*
        * 如果 handleMeleeThreat 中因为不满足冲锋条件导致不能冲锋, 也不允许向后跳, 或向后跳失败了, 且 Charger 在地上
        * 那么如果目标正在看着 Charger, Charger 进入 baitZoneMin 那么 Charger 后退, 出了 baitZoneMax 朝着目标前进进行近战博弈
        * 若向后移动方向不安全, 且此时基础冲锋条件满足, 那么直接冲锋
        */
        static Action distanceControlResult;
        distanceControlResult = maintainMeleeBaitDistance(client, target, dist, visible, isWatching, vel, ang, buttons);
        if (distanceControlResult != Plugin_Continue) {
            return distanceControlResult;
        }
    } else if (threatType == Threat_Shotgun) {
        resAction = handleShotgunThreat(client, target, visible);
    } else {
        resAction = handleNonMeleeThreat(client, target, visible);
    }

    if (resAction != Plugin_Continue)
        return resAction;

    return airLineCorrected ? Plugin_Changed : Plugin_Continue;
}

/**
* 当 Charger 距离目标距离小于 baitZoneMin 时, 若目标看着 Charger 那么 Charger 后退, 如果距离目标距离大于 baitZoneMax 那么前进继续接近目标
* 后跳时保留远离目标的水平动量, 只在空中存在异常指向目标的速度 (如 Approach 最后一跳进入 BaitZone, 那么急停, 清除输入和当前速度)
* @param client Charger 客户端索引
* @param target 目标客户端索引
* @param dist 到目标的距离
* @param visible 是否可见目标
* @param cmdVel 当前速度
* @param buttons 输入按键状态
* @return Action
* @error 
**/

void clearBaitGroundMoveInput(int& buttons, float cmdVel[3]) {
    buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);

    cmdVel[0] = 0.0;
    cmdVel[1] = 0.0;
}

/**
* 执行按照 wordDir 方向的具体移动
* @param worldDir 世界坐标方向向量, 想要 Charger 移动到的方向向量, desiredDir
* @param moveSpeed 想要 Charger 进行移动的速度大小
* @param viewAngles Charger 的视角向量
* @param buttons 输入按键
* @param cmdVel Charger 输入速度向量
* @return void
* @error 
**/
void applyBaitGroundMoveInput(const float worldDir[3], float moveSpeed, const float viewAngles[3], int& buttons, float cmdVel[3]) {
    static float fwd[3], right[3];

    GetAngleVectors(viewAngles, fwd, NULL_VECTOR, NULL_VECTOR);
    GetAngleVectors(viewAngles, NULL_VECTOR, right, NULL_VECTOR);

    fwd[2] = right[2] = 0.0;

    if (NormalizeVector(fwd, fwd) <= 0.0 || NormalizeVector(right, right) <= 0.0) {
        clearBaitGroundMoveInput(buttons, cmdVel);
        return;
    }

    static float fwdMove, sideMove;
    fwdMove = GetVectorDotProduct(worldDir, fwd) * moveSpeed;
    sideMove = GetVectorDotProduct(worldDir, right) * moveSpeed;

    clearBaitGroundMoveInput(buttons, cmdVel);

    /*
    * 进行真正的移动逻辑, 使用 desiredDir * moveSpeed 计算出实际向 desiredDir 方向的移动速度分量
    * 如果 fwd 方向和 desierdDir 方向相反, 那么 fwdMove < 0, 相同则 fwdMove > 0, 即控制 Charger 前进或后退
    * 如果 right 方向和 desiredDir 方向相反, 那么 sideMove < 0, 相同则 sideMove > 0, 即控制 Charger 向右移动或向左移动
    */
    cmdVel[0] = fwdMove;
    cmdVel[1] = sideMove;

    if (fwdMove > 1.0) {
        buttons |= IN_FORWARD;
    } else if (fwdMove < -1.0) {
        buttons |= IN_BACK;
    }

    if (sideMove > 1.0) {
        buttons |= IN_MOVERIGHT;
    } else if (sideMove < -1.0) {
        buttons |= IN_MOVELEFT;
    }
}

/**
* 维护 Charger 和目标之间的近战博弈距离, 保持在 baitZoneMin 到 baitZoneMax 之间
* @param client Charger 客户端索引
* @param target 目标客户端索引
* @param dist 到目标的距离
* @param visible 是否可见目标
* @param isWatching 目标是否正在看着 Charger
* @param cmdVel Charger 当前输入速度向量
* @param viewAngles Charger 的视角向量
* @param buttons 输入按键
* @return Action
* @error 
**/
Action maintainMeleeBaitDistance(int client, int target, float dist, bool visible, bool isWatching, float cmdVel[3], const float viewAngles[3], int& buttons) {
    static float pos[3], targetPos[3], currentVel[3], targetVel[3], currentVel2D[3], targetVel2D[3];
    static float awayDir[3], towardDir[3];

    GetClientAbsOrigin(client, pos);
    GetClientAbsOrigin(target, targetPos);
    GetEntPropVector(client, Prop_Data, "m_vecAbsVelocity", currentVel);
    GetEntPropVector(target, Prop_Data, "m_vecAbsVelocity", targetVel);

    // awayDir = target -> Charger
    MakeVectorFromPoints(targetPos, pos, awayDir);
    awayDir[2] = 0.0;
    if (NormalizeVector(awayDir, awayDir) <= 0.0) {
        return Plugin_Continue;
    }

    // towardDir = Charger -> target
    towardDir[0] = -awayDir[0];
    towardDir[1] = -awayDir[1];
    towardDir[2] = 0.0;

    currentVel2D = currentVel;
    targetVel2D = targetVel;

    currentVel2D[2] = 0.0;
    targetVel2D[2] = 0.0;

    // targetApproachSpeed 是正数表示目标正在朝着 Charger 接近, 负数表示目标正在远离 Charger
    static float targetApproachSpeed, currentGroundSpeed;
    targetApproachSpeed = GetVectorDotProduct(targetVel2D, awayDir);
    currentGroundSpeed = getVectorLength2D(currentVel2D);

    /*
    * 后跳起飞后保留远离目标的动量, 不再被下一帧的距离控制清零
    * 若是从 APPROACH STATE 的最后一次连跳在空中进入 BAIT STATE, 那么保留 vel[2] 让 Charger 自然落地
    */
    if (!IsClientOnGround(client)) {
        clearBaitGroundMoveInput(buttons, cmdVel);

        // 如果 BhopType 是 BhopType_BaitRetreat, 且当前的 Charger 后跳速度分量有效, 那么说明在空中的这次连跳确实由后跳发起, 保持后跳
        static float currentAwaySpeed;
        currentAwaySpeed = GetVectorDotProduct(currentVel, awayDir);

        if (g_AiChargers[client].m_BhopType == BhopType_BaitRetreat &&
            currentAwaySpeed > 1.0)
        {
            return Plugin_Changed;
        }

        // 否则说明当前的速度不是由 BhopType_BaitRetreat 发起的, 可能是 Approach 的最后一跳在空中进入 BaitZone, 此时要急停, 清除输入和速度
        currentVel[0] = 0.0;
        currentVel[1] = 0.0;
        TeleportEntity(client, NULL_VECTOR, NULL_VECTOR, currentVel);

        g_AiChargers[client].m_BhopType = BhopType_None;
        g_AiChargers[client].m_flLastHopSpeed = 0.0;

        return Plugin_Changed;
    }

    if (g_AiChargers[client].m_BhopType == BhopType_BaitRetreat) {
        g_AiChargers[client].m_BhopType = BhopType_None;
        g_AiChargers[client].m_flLastHopSpeed = 0.0;
        ZeroVector(g_AiChargers[client].m_vecAirCorrGoal);
        g_AiChargers[client].m_AirStrafe.init();
    }

    /*
    * 只有目标正在看着 Charger 时才进行距离博弈, 否则目标不看着 Charger, 安全, 不需要进行近战博弈操作
    * 保留给 handleMeleeThreat 处理, 进行目标是否易受攻击, 概率冲锋等决策
    */
    if (!isWatching)
        return Plugin_Continue;

    static float meleeRange, baitZoneMin, baitZoneMax;
    meleeRange = !cvMeleeRange ? DEFAULT_MELEE_RANGE : cvMeleeRange.FloatValue;
    baitZoneMin = meleeRange + g_cvMeleeBaitZoneMinRange.FloatValue;
    baitZoneMax = meleeRange + g_cvMeleeBaitZoneMaxRange.FloatValue;

    static float baitZoneCenter, baitZoneQuarter;
    baitZoneCenter = (baitZoneMin + baitZoneMax) * 0.5;
    baitZoneQuarter = (baitZoneMax - baitZoneMin) * 0.25;

    // Charger 处于 baitZoneCenter +- baitZoneQuarter 区域内, 若目标不动, 此时原地跳跃打拳威慑
    static float baitHopMin, baitHopMax;
    baitHopMin = baitZoneCenter - baitZoneQuarter;
    baitHopMax = baitZoneCenter + baitZoneQuarter;

    static bool moveTowardTarget, moveAwayFromTarget;
    moveTowardTarget = moveAwayFromTarget = false;

    /*
    * 优先修正距离越界, 目标开始接近 Charger, 那么必须后退, 如果目标开始远离 Charger, 那么必须前进
    */
    if (dist < baitZoneMin) {
        moveAwayFromTarget = true;
    } else if (dist > baitZoneMax) {
        moveTowardTarget = true;
    } else if (targetApproachSpeed <= -50.0) {
        // 目标持续后退, 使用原生移动跟随
        moveTowardTarget = true;
    } else if (targetApproachSpeed >= 50.0) {
        // 目标慢速压近, 快速压近已经在 handleMeleeThreat 中优先后跳处理, 这里处理不能后跳的情况
        moveAwayFromTarget = true;
    } else if (dist < baitHopMin) {
        // Charger 偏近, 自然后退回 baitZoneCenter 区域
        moveAwayFromTarget = true;
    } else if (dist > baitHopMax) {
        // Charger 偏远, 自然前进去到 baitZoneCenter 区域
        moveTowardTarget = true;
    } else {
        // 刚好位于 baitZoneCenter +- baitZoneQuarter 区域, 开始原地跳跃并打拳
        clearBaitGroundMoveInput(buttons, cmdVel);
        
        if (currentGroundSpeed <= 50.0) {
            buttons |= IN_ATTACK2;
            buttons |= IN_DUCK;
            buttons |= IN_JUMP;
        }
        return Plugin_Changed;
    }

    static float runTopSpeed;
    runTopSpeed = SDKCall(g_hSdkGetRunTopSpeed, client);

    static float desiredDir[3], desiredVel[3];

    if (moveAwayFromTarget) {
        // 需要远离目标的时候
        desiredDir = awayDir;
    } else {
        desiredDir = towardDir;
    }

    desiredVel[0] = desiredDir[0] * runTopSpeed;
    desiredVel[1] = desiredDir[1] * runTopSpeed;
    desiredVel[2] = 0.0;

    if (!nextTickPosCheck(client, desiredVel, visible)) {
        // 目标慢速靠近 Charger 时, 如果原生后退也不安全, 那么继续尝试冲锋
        if (moveAwayFromTarget && canCommitBaitCharge(client, target, visible)) {
            transitionBaitToLocked(client, target, "Native bait retreat direction unsafe, forcing charge!");
            return Plugin_Changed;
        }
        return Plugin_Continue;
    }

    applyBaitGroundMoveInput(desiredDir, runTopSpeed, viewAngles, buttons, cmdVel);

    g_AiChargers[client].m_BhopType = BhopType_None;
    g_AiChargers[client].m_flLastHopSpeed = 0.0;
    ZeroVector(g_AiChargers[client].m_vecAirCorrGoal);
    g_AiChargers[client].m_AirStrafe.init();

    return Plugin_Changed;
}

/**
* 是否应该退出博弈状态, 转换为 APPROACH State
* @param client 客户端索引
* @param target 目标索引
* @param dist 距离
* @param visible 是否可见
* @param threatType 目标威胁类型
* @return bool
**/
bool shouldExitBaitState(int client, int target, float dist, bool visible, TargetThreatType threatType) {
    if (!isAiCharger(client) || !IsValidSurvivor(target) || !IsPlayerAlive(target))
        return true;

    // 向后跳在空中时, 禁止因视野或距离变化中途退出 Bait
    if (!IsClientOnGround(client) &&
        g_AiChargers[client].m_BhopType == BhopType_BaitRetreat
    ) {
        return false;
    }

    static float minDist;
    minDist = g_cvBhopMinDist.FloatValue + STATE_TOLERANCE_DIST;
    
    // 连续失去目标视野超过宽限期才转换为 APPROACH STATE
    if (!visible &&
        GetGameTime() - g_AiChargers[client].m_flLastVisibleTime >= BAIT_LOST_SIGHT_GRACE
    ) {
        stateLog.debugAll("Charger (%N) lost the sight of target (%N) for %.2f seconds, transition to APPROACH STATE",
            client, target, BAIT_LOST_SIGHT_GRACE);
        return true;
    }
    // 目标非手持近战, 且到目标距离大于 g_cvBhopMinDist.FloatValue + 容差距离, 转换为 APPROACH STATE
    if (threatType != Threat_Melee && dist > minDist) {
        stateLog.debugAll("Charger (%N), target (%N), distance %.2f > min dist %.2f, transition to APPROACH STATE", client, target, dist, minDist);
        return true;
    }
    // 目标手持近战, 且到目标距离大于 meleeRange + g_cvMeleeBaitZoneMaxRange.FloatValue, 转换为 APPROACH STATE
    static float meleeRange;
    meleeRange = !cvMeleeRange ? DEFAULT_MELEE_RANGE : cvMeleeRange.FloatValue;
    minDist = meleeRange + g_cvMeleeBaitZoneMaxRange.FloatValue + STATE_TOLERANCE_DIST;
    if (threatType == Threat_Melee && dist > minDist) {
        stateLog.debugAll("Charger (%N), target (%N) holding melee, dist %.2f > baitZoneMax %.2f, transition to APPROACH STATE", client, target, dist, minDist);
        return true;
    }
    return false;
}

/**
* Charger 面对手持近战目标时的处理逻辑
* 1. 若目标没有看着 Charger, 且没有其他人看着 Charger, 处在博弈区内, 概率冲锋
* 2. 目标处于易受攻击状态, 直接冲锋
* 3. 目标不处于易受攻击状态, Charger 血量大于近战斩杀线时, 概率冲锋
* @param client 客户端索引
* @param target 目标索引
* @param dist 距离
* @param visible 是否可见目标
* @param isWatching 目标是否正在看着 Charger
* @param watchers 正在看着 Charger 的人数
* @return void
**/
Action handleMeleeThreat(int client, int target, float dist, bool visible, bool isWatching, int watchers, int& buttons) {
    // Charger 需要在地上
    if (!IsClientOnGround(client))
        return Plugin_Continue;

    static int health;
    health = GetEntProp(client, Prop_Data, "m_iHealth");

    static float meleeRange, baitZoneMin, baitZoneMax;
    meleeRange = !cvMeleeRange ? DEFAULT_MELEE_RANGE : cvMeleeRange.FloatValue;
    baitZoneMin = meleeRange + g_cvMeleeBaitZoneMinRange.FloatValue;
    baitZoneMax = meleeRange + g_cvMeleeBaitZoneMaxRange.FloatValue;

    /*
    * ShouldExitBaitState 允许 baitZoneMax + stateTolerance, 这里应该也保持这个区间
    * 如果距离超过了 biatZoneMax + StateTolerance, 那么直接退出 BAIT STATE
    */
    if (dist > (baitZoneMax + STATE_TOLERANCE_DIST))
        return Plugin_Continue;

    // 检查当前是否超出博弈最大时长
    static bool baitTimeout;
    baitTimeout = isBaitTimeout(client);

    /*
    * 首先检查是否满足基础冲锋条件, 接着检查目标是否处于易受攻击状态, 是否超出最大博弈时长和是否允许概率冲锋
    */
    if (canCommitBaitCharge(client, target, visible)) {
        // 1. 目标处于易受攻击状态, 允许直接冲锋
        if (isTargetVulnerable(client, target)) {
            transitionBaitToLocked(client, target, "Target is vulnerable");
            return Plugin_Changed;
        }

        // 2. 超出最大博弈时长, 强制冲锋
        if (baitTimeout) {
            transitionBaitToLocked(client, target, "Bait max duration exceeded");
            return Plugin_Changed;
        }

        /*
        * 3. 当前是否允许朝着手持近战目标概率冲锋
        * 需要目标没有看着 Charger 且没有其他生还者看着 Charger 或 Charger 血量大于近战斩杀线
        */
        if (shouldProbolisticMeleeCharge(client, isWatching, watchers, health)) {
            transitionBaitToLocked(client, target, "Probabilistic charge");
            return Plugin_Changed;
        }
    } else if (baitTimeout) {
        /*
        * 此时是基础冲锋条件不满足 (不可见目标/高度不可达/冲锋路径阻挡等), 但是超出了最大博弈时长
        * 应该退出 BaitState, 返回 STATE_APPROACH 重新寻路
        */
        stateLog.debugAll("Charger (%N) bait timeout but cannot safely charge target (%N), transition to APPROACH STATE",
            client, target);
        
        g_ChargerStateContext[client].transitionTo(CH_STATE_APPROACH);
        return Plugin_Changed;
    }

    /*
    * 如果不满足冲锋条件, 那么目标接近时尝试后跳
    * 1. 目标主动进入 baitZoneMin 时, 且在 target -> Charger 方向的速度分量大于 MELEE_PRESS_MIN_SPEED 时, 表示目标正在主动接近 Charger, 应该后跳
    * 2. 目标距离 Charger 小于 (meleeRange + MELEE_EMERGENCY_BUFFER) 时, 情况紧急, 应该后跳, 否则会被砍
    */
    static float chargerPos[3], targetPos[3], targetVel[3], targetToCharger[3];
    GetClientAbsOrigin(client, chargerPos);
    GetClientAbsOrigin(target, targetPos);
    GetEntPropVector(target, Prop_Data, "m_vecAbsVelocity", targetVel);

    MakeVectorFromPoints(targetPos, chargerPos, targetToCharger);
    targetToCharger[2] = 0.0;

    if (NormalizeVector(targetToCharger, targetToCharger) <= 0.0)
        return Plugin_Continue;
    
    targetVel[2] = 0.0;

    // 计算目标速度和 Charger -> target 方向的分量, 如果 targetApproachSpeed > 0 表示目标正在朝着 Charger 主动接近, 此时应该向后跳
    static float targetApproachSpeed;
    targetApproachSpeed = GetVectorDotProduct(targetVel, targetToCharger);

    /*
    * targetPressing: 目标主动进入 baitZoneMin 时, 且速度分量大于 MELEE_PRESS_MIN_SPEED 时, 应该后跳
    * emergencyClose: 目标距离 Charger 小于 (meleeRange + MELEE_EMERGENCY_BUFFER) 时, 情况紧急, 应该后跳, 否则会被砍
    */
    static bool targetPressing, emergencyClose;
    targetPressing = dist <= baitZoneMin && targetApproachSpeed >= MELEE_PRESS_MIN_SPEED;
    emergencyClose = dist <= meleeRange + MELEE_EMERGENCY_BUFFER;

    // 无法冲锋时, 目标主动进入 baitZoneMin 或已经过近才尝试后跳
    if (targetPressing || emergencyClose) {
        return performBackwardBhop(client, target, visible, buttons);
    }

    return Plugin_Continue;
}

/**
* Charger 处于 StateBait 时能否冲锋的基础检查
* 1. Charger 需要在地上
* 2. Charger 需要可见目标
* 3. Charger 冲锋能力就绪
* 4. Charger 冲锋目标高度可达
* 5. Charger 冲锋路径不被阻挡
* @param client 客户端索引
* @param target 目标索引
* @param visible 是否可见目标
* @return bool
* @error
**/
bool canCommitBaitCharge(int client, int target, bool visible) {
    return (IsClientOnGround(client) &&
            visible &&
            isChargeAbilityReady(client) &&
            isChargeTargetHeightReachable(client, target) &&
            !isChargePathBlocked(client, target));
}

void transitionBaitToLocked(int client, int target, const char[] reason) {
    stateLog.debugAll("Charger (%N) trainsition to LOCKED STATE, target (%N), reason: %s",
        client, target, reason);

    g_ChargerStateContext[client].transitionTo(CH_STATE_LOCKED);
}

bool isBaitTimeout(client) {
    return g_ChargerStateContext[client].getStateDuration() >= g_cvBaitMaxDuration.FloatValue;
}

bool shouldProbolisticMeleeCharge(int client, bool isWatching, int watchers, int health) {
    static int meleeDamageCharger;
    meleeDamageCharger = !cvMeleeDamageCharger ? DEFAULT_MELEE_DMG_CHARGER : cvMeleeDamageCharger.IntValue;

    /*
        一帧最多决策一次, 概率冲锋条件为
        1. 目标没有看着 Charger, 且没有其他人看着 Charger
        2. Charger 血量大于近战斩杀线时, 概率冲锋
    */
    bool canProbCharge = (
                            (!isWatching && watchers <= 0) ||
                            (health > meleeDamageCharger)
                        );
                    
    return canProbCharge && isChargerProbCharge(client);
}

/**
* Charger 在面对手持近战目标时, 执行向后连跳操作
* @param client 客户端索引
* @param target 目标索引
* @param dist 距离
* @param visible 是否可见目标
* @return Action
**/
Action performBackwardBhop(int client, int target, bool visible, int& buttons) {
    if (!IsClientOnGround(client) || !allowToBhop(client))
        return Plugin_Continue;

    /*
    * 使用目标到 Charger 的方向, 确保后跳严格远离目标, 不受当前 Charger 视角影响, 之前是使用 Charger 视角方向, 不够健壮
    */
    static float pos[3], targetPos[3], vBack[3];
    GetClientAbsOrigin(client, pos);
    GetClientAbsOrigin(target, targetPos);
    MakeVectorFromPoints(targetPos, pos, vBack);
    vBack[2] = 0.0;

    if (NormalizeVector(vBack, vBack) <= 0.0)
        return Plugin_Continue;

    // 获取 runTopSpeed 作为基础速度
    static float baseSpeed;
    baseSpeed = SDKCall(g_hSdkGetRunTopSpeed, client);

    // 计算目标速度在 charger 后退方向上的投影, dot > 0 表示目标在追 charger, dot < 0 表示目标远离 charger
    static float targetAbsVel[3], approachSpeed;
    GetEntPropVector(target, Prop_Data, "m_vecAbsVelocity", targetAbsVel);
    targetAbsVel[2] = 0.0;
    approachSpeed = GetVectorDotProduct(targetAbsVel, vBack);
    
    // 计算 charger 需要的后退速度
    static float backSpeed;
    // backSpeed = baseSpeed + g_cvBhopImpulse.FloatValue;
    backSpeed = baseSpeed;
    if (approachSpeed > 0.0) {
        backSpeed += approachSpeed * TARGET_PURSUIT_FACTOR;
    } else {
        // approachSpeed < 0, 目标远离 charger, 在 backSpeed 速度上叠加目标速度 (approachSpeed 是负数, 因此是减速), 减少距离
        // 控制减速大小, 0.5 表示在 backSpeed 基础上减速目标速度的 50%
        backSpeed += approachSpeed * TARGET_RETREAT_FACTOR;
    }

    // 限制最小后跳速度为 RunToSpeed, 最大后跳速度不超过设置的连跳速度上限
    if (backSpeed < baseSpeed) {
        backSpeed = baseSpeed;
    }
    if (backSpeed > g_cvBhopMaxSpeed.FloatValue) {
        backSpeed = g_cvBhopMaxSpeed.FloatValue;
    }

    // 应用缩放
    ScaleVector(vBack, backSpeed);

    // 连跳后退的落点不安全的时候, 则转为冲锋状态, 使用统一的基础冲锋检查函数来检查冲锋是否安全
    if (!nextTickPosCheck(client, vBack, visible)) {
        if (canCommitBaitCharge(client, target, visible)) {
            transitionBaitToLocked(client, target, "Backward bhop landing pos unsafe, forcing charge!");
            return Plugin_Changed;
        }

        return Plugin_Continue;
    }

    stateLog.debugAll("Charger (%N) bait to holding melee target (%N), performing backward bunnyhop, speed %.2f", client, target, backSpeed);
    g_AiChargers[client].m_BhopType = BhopType_BaitRetreat;
    g_AiChargers[client].m_flLastHopSpeed = backSpeed;
    ZeroVector(g_AiChargers[client].m_vecAirCorrGoal);
    g_AiChargers[client].m_AirStrafe.init();
    buttons |= IN_DUCK;
    buttons |= IN_JUMP;
    TeleportEntity(client, NULL_VECTOR, NULL_VECTOR, vBack);
    return Plugin_Changed;
}

/**
* Charger 面对手持霰弹枪的目标的处理逻辑
* 1. 若目标手持霰弹枪, Charger 到目标距离小于 bhopMinDist, 直接冲锋
* @param client 玩家索引
* @param target 目标索引
* @param dist 距离
* @param visible 是否可见目标
* @return Action
**/
Action handleShotgunThreat(int client, int target, bool visible) {
    // Charger 需要在地面
    if (!IsClientOnGround(client))
        return Plugin_Continue;
    // 如果不满足基础冲锋条件, 检查博弈是否超时, 若博弈超时, 转换到 APPROACH STATE, 不冒险冲锋
    if (!canCommitBaitCharge(client, target, visible)) {
        if (isBaitTimeout(client)) {
            stateLog.debugAll("Charger (%N) bait timeout against shotgun target (%N), cannot safely charge, transition to APPROACH STATE",
                client, target);
            
            g_ChargerStateContext[client].transitionTo(CH_STATE_APPROACH);
            return Plugin_Changed;
        }
        return Plugin_Continue;
    }

    // 遇到手持霰弹枪的目标, 直接冲锋
    transitionBaitToLocked(client, target, "Shotgun target in safe charge range, forcing charge!");
    return Plugin_Changed;
}

/**
* Charger 面对手持非近战, 非霰弹枪的目标的处理逻辑
* 1. 目标没有看着 Charger, 且没有其他人看着 Charger, 继续接近挥拳, 同时概率冲锋
* 2. 目标处于易受攻击状态, 直接冲锋
* 3. 无论目标有没有看着 Charger, 若血量大于近战斩杀线, 概率冲锋, 小于则直接冲锋
* @param client 客户端索引
* @param target 目标索引
* @param dist 距离
* @param health Charger 生命值
* @param visible 是否可见目标
* @param meleeDamage Charger 近战斩杀线
* @param watchers 正在看着 Charger 的人数
* @return Action
**/
Action handleNonMeleeThreat(int client, int target, bool visible) {
    // Charger 需要在地上才进行处理
    if (!IsClientOnGround(client))
        return Plugin_Continue;

    static bool baitTimeout;
    baitTimeout = isBaitTimeout(client);

    // 如果不满足基础冲锋条件, 检查博弈是否超时, 若博弈超时, 转换到 APPROACH STATE, 不冒险冲锋
    if (!canCommitBaitCharge(client, target, visible)) {
        if (baitTimeout) {
            stateLog.debugAll("Charger (%N) bait timeout against shotgun target (%N), cannot safely charge, transition to APPROACH STATE",
                client, target);
            
            g_ChargerStateContext[client].transitionTo(CH_STATE_APPROACH);
            return Plugin_Changed;
        }
        return Plugin_Continue;
    }

    // 优先检查目标是否处于易受攻击状态
    if (isTargetVulnerable(client, target)) {
        transitionBaitToLocked(client, target, "Non-melee target is vulnerable, forcing charge!");
        return Plugin_Changed;
    }

    // 接着检查博弈是否超时
    if (baitTimeout) {
        transitionBaitToLocked(client, target, "Non-melee bait max duration exceeded, forcing charge!");
        return Plugin_Changed;
    }

    /*
    * 最后检查血量条件
    * 1. 血量低于近战斩杀线时立刻冲锋
    * 2. 血量高于近战斩杀线概率冲锋
    */
    static int health, meleeDamageCharger;
    health = GetEntProp(client, Prop_Data, "m_iHealth");
    meleeDamageCharger = !cvMeleeDamageCharger ? DEFAULT_MELEE_DMG_CHARGER : cvMeleeDamageCharger.IntValue;

    if (health <= meleeDamageCharger) {
        transitionBaitToLocked(client, target, "Charger current health is lower than melee threshold, forcing charge!");
        return Plugin_Changed;
    }

    if (isChargerProbCharge(client)) {
        transitionBaitToLocked(client, target, "Probabilistic charge to non-melee target, forcing charge!");
        return Plugin_Changed;
    }
    
    return Plugin_Continue;
}

// ============================================================
// OnExit 函数
// ============================================================
stock void StateBait_OnExit(int client) {
    stateLog.debugAll("\n========== Charger (%N) State Bait On Exit ===========", client);
    if (g_AiChargers[client].m_BhopType == BhopType_BaitRetreat) {
        g_AiChargers[client].m_BhopType = BhopType_None;
        g_AiChargers[client].m_flLastHopSpeed = 0.0;
    }
}
