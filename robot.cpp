/**
 ******************************************************************************
 * @file    robot.cpp
 * @brief   机器人主控制器实现（模式状态机 + 三级闭环 + 遥测 + 标定）
 *
 * @details 控制任务每个周期（5ms / 200Hz）的执行顺序是固定的：
 *          @verbatim
 *            ① 取指令（网络核投递）        —— 队列，非阻塞
 *            ② 传感器扫描（20Hz 降频）     —— 4067 + MCP23017
 *            ③ 里程与测速结算              —— FG 增量 → 转速 / 里程计偏航
 *            ④ IMU 更新（互补融合）        —— 得到可靠偏航角
 *            ⑤ 安全仲裁                    —— 触发即清零 PWM
 *            ⑥ 模式机 + 三环运算           —— 位置环 → 速度环 → 航向环
 *            ⑦ 遥测快照（50Hz 降频）       —— 供网络核读取
 *          @endverbatim
 *
 *          三级闭环结构：
 *          @verbatim
 *            位置环（脉冲误差 → 目标速度） → 速度环（速度误差 → PWM）
 *                                          ↘ 航向环（角度误差 → 左右轮速差）
 *          @endverbatim
 *
 * @author  CombatBot 电控组
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>统一企业级注释规范
 *          </table>
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include "robot.h"
#include "motor.h"
#include "pid.h"
#include "kinematics.h"
#include "imu.h"
#include "sensors.h"
#include "safety.h"

/* ==========================================================================
 *                          私有常量
 * ========================================================================== */
#define CTRL_PERIOD_MS   5          /**< 控制周期 5ms → 200Hz                  */
#define CTRL_DT          0.005f     /**< 控制周期（秒）                        */
#define SENSOR_PERIOD_MS 50         /**< 传感器扫描周期 → 20Hz                 */
#define TM_PERIOD_MS     20         /**< 遥测快照刷新周期 → 50Hz               */
#define ACTION_TIMEOUT_MS 30000     /**< 单次动作超时保护                      */
#define TRIM_TIMEOUT_MS  4000       /**< 转向 IMU 精修超时保护                 */
#define CAL_IMU_MS       3000       /**< IMU 零偏采样时长                      */

/* ==========================================================================
 *                          私有状态（模块内静态）
 * ========================================================================== */
static RMode   s_mode  = R_IDLE;    /**< 当前控制模式                          */
static RState  s_state = RS_IDLE;   /**< 当前动作状态                          */
static bool    s_estop = false;     /**< 急停锁定标志                          */
static uint32_t s_lastHb = 0;       /**< 最近一次心跳时间戳                    */

static QueueHandle_t     s_cmdQ  = NULL;    /**< 指令队列（网络核 → 控制核）   */
static QueueHandle_t     s_doneQ = NULL;    /**< 回传队列（控制核 → 网络核）   */
static SemaphoreHandle_t s_tmMtx = NULL;    /**< 遥测互斥量                    */
static Telemetry         s_tm;              /**< 遥测数据本体                  */

/* ---- 连续模式输入 ---- */
static float   s_mx = 0, s_my = 0;  /**< 摇杆归一化输入（x 右为正，y 前为正） */
static uint8_t s_mspd = 0;          /**< 速度上限百分比                       */

/* ---- 精准动作公共状态 ---- */
static int32_t  s_startP[4];        /**< 动作起始时四轮脉冲快照               */
static float    s_startAvg = 0;     /**< 直线：四轮平均起始脉冲               */
static float    s_startL = 0;       /**< 转向：左侧两轮平均起始脉冲           */
static float    s_startR = 0;       /**< 转向：右侧两轮平均起始脉冲           */
static float    s_targetPulses = 0; /**< 目标脉冲数（带符号）                 */
static float    s_targetDeg = 0;    /**< 转向目标角度 °（带符号，正=逆时针）  */
static float    s_yawRef = 0;       /**< 动作起始时的融合偏航角               */
static float    s_vmax = 40;        /**< 本次动作允许的最大线速度 cm/s        */
static uint32_t s_actT0 = 0;        /**< 动作起始时间戳                       */
static uint8_t  s_turnPhase = 0;    /**< 转向阶段：0=脉冲主转 1=IMU 精修 2=收尾 */
static uint32_t s_phaseT0 = 0;      /**< 当前阶段起始时间戳                   */

static Trapezoid s_trap;            /**< 梯形速度规划器                       */
static PID       s_vPid[4];         /**< 四轮速度环 PID                       */
static PID       s_hPid;            /**< 航向环 PID（直线跑偏修正）           */
static PID       s_symPid;          /**< 对称环 PID（转向打滑补偿）           */

/* ---- 标定状态机 ---- */
static uint8_t  s_calMode    = CAL_NONE;    /**< 正在执行的标定项             */
static uint8_t  s_calPending = CAL_NONE;    /**< 等待用户输入实测值的标定项   */
static float    s_calPulses    = 0;         /**< 行程标定实测到的总脉冲数     */
static float    s_calMeasDeg   = 0;         /**< 转向标定实测到的角度         */
static float    s_calTargetDeg = 0;         /**< 转向标定的命令角度           */
static uint32_t s_calT0 = 0;                /**< 零偏采样起始时间戳           */
static double   s_calSum = 0;               /**< 零偏采样累加值               */
static uint32_t s_calN = 0;                 /**< 零偏采样计数                 */

/* ---- 里程计 ---- */
static int32_t s_prevP[4] = { 0, 0, 0, 0 }; /**< 上一周期的脉冲数（求增量用） */
static float   s_odoCm = 0;                 /**< 累计里程 cm                  */

/* ==========================================================================
 *                          私有函数（文件内静态）
 * ========================================================================== */

/**
 * @brief   角度归一化到 (−180°, +180°]
 * @param[in] a  任意角度 °
 * @return  归一化后的角度 °
 */
static float wrap180(float a) {
    while (a >  180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

/**
 * @brief   投递一条回传事件到网络核
 * @param[in] type    事件类型 @ref DoneType
 * @param[in] sub     子类型 / 告警码
 * @param[in] target  目标值
 * @param[in] actual  实际值
 * @return   无
 */
static void pushDone(uint8_t type, uint8_t sub, float target, float actual) {
    DoneMsg d;
    d.type   = type;
    d.sub    = sub;
    d.target = target;
    d.actual = actual;
    d.err    = actual - target;
    if (s_doneQ) xQueueSend(s_doneQ, &d, 0);
}

/**
 * @brief   停止全部电机并复位闭环历史
 * @param   无
 * @return  无
 * @note    复位 PID 积分项非常重要：切换模式时若不复位，
 *          上一次动作残留的积分会造成明显的超调。
 */
static void allStop() {
    motorStopAll();
    for (int i = 0; i < 4; i++) s_vPid[i].reset();
    s_trap.reset();
}

/**
 * @brief   启动一次精准直线动作
 * @param[in] distCm  目标距离 cm（负值后退）
 * @param[in] spdPct  速度上限百分比（0 表示使用配置中的默认速度）
 * @param[in] c       当前配置
 * @return   无
 */
static void startMove(float distCm, uint8_t spdPct, const Cfg& c) {
    s_targetDeg = distCm;

    s_vmax = (spdPct > 0) ? (c.maxSpeed * spdPct / 100.0f) : c.defSpeed;
    s_vmax = fminf(s_vmax, c.maxSpeed);

    /* 记录起始脉冲：四轮取平均，抵消单轮打滑对位置环的影响 */
    s_startAvg = 0;
    for (int i = 0; i < 4; i++) {
        s_startP[i] = motorPulses(i);
        s_startAvg += s_startP[i];
    }
    s_startAvg /= 4.0f;

    s_targetPulses = kinCmToPulses(distCm);
    s_yawRef       = gImu.yaw;             /* 航向基准：保持出发时的朝向      */

    s_trap.reset();
    s_hPid.reset();
    s_mode  = R_MOVE;
    s_state = RS_RUN;
    s_actT0 = millis();
}

/**
 * @brief   启动一次精准转向动作（原地坦克转向）
 * @param[in] deg  目标角度 °，正值 = 逆时针（左转）
 * @param[in] c    当前配置
 * @return   无
 * @note     转向默认使用 40% 最大线速度：驼峰轮转向阻力极大，
 *          满速起转必然打滑，限速反而更快更准。
 */
static void startTurn(float deg, const Cfg& c) {
    s_targetDeg = deg;
    s_vmax      = c.maxSpeed * 0.4f;

    s_startL = (motorPulses(0) + motorPulses(2)) / 2.0f;
    s_startR = (motorPulses(1) + motorPulses(3)) / 2.0f;
    s_targetPulses = kinDegToPulses(deg);
    s_yawRef       = gImu.yaw;

    s_trap.reset();
    s_symPid.reset();
    s_turnPhase = 0;
    s_phaseT0   = millis();
    s_mode      = R_TURN;
    s_state     = RS_RUN;
    s_actT0     = millis();
}

/**
 * @brief   指令分发
 * @param[in] cmd  指令包
 * @param[in] c    当前配置
 * @return   无
 * @note     急停锁定期间，除「解锁」外的所有驱动类指令都会被丢弃。
 */
static void handleCmd(const Cmd& cmd, const Cfg& c) {
    switch (cmd.t) {
        case C_STOP:
            allStop();
            s_mode  = R_IDLE;
            s_state = RS_IDLE;
            break;

        case C_HB:
            /* 纯心跳：只刷新看门狗，不改变当前输入 */
            s_lastHb = millis();
            break;

        case C_DRV:
            if (s_estop) break;
            s_mx   = constrain(cmd.a, -1.0f, 1.0f);
            s_my   = constrain(cmd.b, -1.0f, 1.0f);
            s_mspd = cmd.u;
            s_lastHb = millis();
            if (s_mode != R_MANUAL) { s_trap.reset(); s_mode = R_MANUAL; }
            s_state = RS_RUN;
            break;

        case C_MOVE:
            if (s_estop) break;
            if (s_mode == R_MANUAL) allStop();
            startMove(cmd.a, cmd.u, c);
            break;

        case C_TURN:
            if (s_estop) break;
            if (s_mode == R_MANUAL) allStop();
            startTurn(cmd.a, c);
            break;

        case C_SERVO:
            if (cmd.u) servoUp(); else servoDown();
            break;

        case C_ESTOP:
            s_estop = true;
            allStop();
            s_mode  = R_ESTOP;
            s_state = RS_IDLE;
            pushDone(D_ALERT, 1, 0, 0);         /* 告警码 1：急停已触发 */
            break;

        case C_UNLOCK:
            s_estop = false;
            allStop();
            s_mode  = R_IDLE;
            s_state = RS_IDLE;
            break;

        case C_CAL:
            if (cmd.u == CAL_IMU) {
                /* 零偏标定：必须静止，先切断输出再开始采样 */
                allStop();
                s_calT0  = millis();
                s_calSum = 0;
                s_calN   = 0;
                s_mode   = R_CAL_IMU;
                s_state  = RS_RUN;
            } else if (cmd.u == CAL_ODO) {
                s_calMode = CAL_ODO;
                startMove(c.calDist, 0, c);
            } else if (cmd.u == CAL_TURN) {
                s_calMode     = CAL_TURN;
                s_calTargetDeg = c.calTurnDeg;
                startTurn(c.calTurnDeg, c);
            }
            break;

        case C_CAL_FIN: {
            /* 用户在网页填入实测值 → 解算标定系数并写入 NVS */
            float actual = cmd.a;

            if (cmd.u == CAL_ODO && actual > 1.0f && s_calPulses != 0) {
                float k = s_calPulses / actual;             /* 脉冲 / 厘米    */
                if (k > 0.05f && k < 100.0f) {
                    if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);
                    gCfg.kOdo = k;
                    if (gCfgMutex) xSemaphoreGive(gCfgMutex);
                    cfgSave();
                    pushDone(D_CAL_RESULT, CAL_ODO, actual, k);
                    s_calPending = CAL_NONE;
                }
            } else if (cmd.u == CAL_TURN && fabsf(actual) > 1.0f) {
                CfgSnap cs;
                /* 打滑越多，需要走的脉冲越多：K_new = K_old × 命令角 / 实测角 */
                float k = cs.c.kTurn * (s_calTargetDeg / actual);
                k = constrain(k, 0.4f, 2.5f);
                if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);
                gCfg.kTurn = k;
                if (gCfgMutex) xSemaphoreGive(gCfgMutex);
                cfgSave();
                pushDone(D_CAL_RESULT, CAL_TURN, actual, k);
                s_calPending = CAL_NONE;
            }
            break;
        }

        default:
            break;
    }
}

/**
 * @brief   四轮速度闭环（含前馈）
 * @param[in] vL     左轮目标速度 cm/s
 * @param[in] vR     右轮目标速度 cm/s
 * @param[in] c      当前配置
 * @param[in] dt     控制周期 s
 * @param[in] scale  全局限速系数（安全仲裁给出）
 * @return   无
 * @note     控制量 = 前馈 0.9×目标速度占比 + PID 修正。
 *          前馈让电机迅速跟上，PID 负责消除稳态误差与负载扰动。
 */
static void driveWheels(float vL, float vR, const Cfg& c, float dt, float scale) {
    for (int i = 0; i < 4; i++) {
        s_vPid[i].set(c.vKp, c.vKi, c.vKd);
        s_vPid[i].setLimit(-1.0f, 1.0f, -0.6f, 0.6f);

        float target = ((i == 0 || i == 2) ? vL : vR) * scale;
        float err    = target - motorSpeed(i);

        float out = s_vPid[i].update(err, dt);
        float ff  = target / fmaxf(c.maxSpeed, 1.0f);       /* 前馈项 */

        motorSetNorm(i, 0.9f * ff + out);
    }
}

/* ==========================================================================
 *                              对外函数实现
 * ========================================================================== */

/**
 * @brief   初始化机器人子系统并创建控制任务
 * @param   无
 * @return  无
 */
void robotInit() {
    s_cmdQ  = xQueueCreate(16, sizeof(Cmd));
    s_doneQ = xQueueCreate(8,  sizeof(DoneMsg));
    s_tmMtx = xSemaphoreCreateMutex();

    imuInit();                  /* ① 先起 I²C，后续传感器共用同一总线 */
    sensorsInit();              /* ② 4067 + MCP23017                  */
    motorInit();                /* ③ LEDC + FG 中断 + 舵机（PWM=0）   */
    servoCenter();              /* ④ 舵机回中                         */
    motorResetAllPulses();      /* ⑤ 里程从零开始                     */

    /* 控制任务绑定到 Core 0，与网络核（Core 1 的 loopTask）物理隔离 */
    xTaskCreatePinnedToCore(robotTask, "robot", 8192, NULL, 5, NULL, 0);
}

/**
 * @brief   控制任务主体（200Hz）
 * @param[in] arg  未使用
 * @return   无
 */
void robotTask(void* arg) {
    (void)arg;

    TickType_t lastWake = xTaskGetTickCount();
    uint32_t   lastSen  = 0;
    uint32_t   lastTm   = 0;

    for (;;) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CTRL_PERIOD_MS));
        float    dt  = CTRL_DT;
        uint32_t now = millis();

        CfgSnap cs;
        const Cfg& c = cs.c;

        /* ---- ① 取指令 ---- */
        Cmd cmd;
        while (s_cmdQ && xQueueReceive(s_cmdQ, &cmd, 0)) handleCmd(cmd, c);

        /* ---- ② 传感器扫描（20Hz） ---- */
        if (now - lastSen >= SENSOR_PERIOD_MS) {
            sensorsUpdate();
            lastSen = now;
        }

        /* ---- ③ 里程与测速结算 ---- */
        int32_t cur[4];
        float   dL = 0, dR = 0;
        for (int i = 0; i < 4; i++) {
            cur[i]     = motorPulses(i);
            int32_t d  = cur[i] - s_prevP[i];
            s_prevP[i] = cur[i];

            if (i == 0 || i == 2) dL += d / 2.0f; else dR += d / 2.0f;
            s_odoCm += fabsf((float)d) / kinPulsesPerCm();
        }
        motorUpdate(dt);

        /* 里程计偏航增量：Δθ = (s_R − s_L) / T ，单位为度 */
        float dYawOdo = 0;
        if (fabsf(dL) + fabsf(dR) > 0.01f) {
            dYawOdo = (kinPulsesToCm(dR) - kinPulsesToCm(dL)) / kinTrackCm()
                      * 180.0f / 3.14159265f;
        }

        bool stationary = true;
        for (int i = 0; i < 4; i++)
            if (motorDuty(i) > 0 || fabsf(motorSpeed(i)) > 1.0f) stationary = false;

        /* ---- ④ IMU 更新（互补融合） ---- */
        imuUpdate(dt, dYawOdo, stationary);

        /* ---- ⑤ 安全仲裁 ---- */
        bool anyStall = false;
        for (int i = 0; i < 4; i++) if (motorIsStalled(i)) anyStall = true;

        SafetyOut sf;
        safetyUpdate(c, now, s_lastHb, s_mode == R_MANUAL, anyStall, sf);

        if (sf.forceStop && s_mode != R_ESTOP) {
            allStop();
            if (s_mode == R_MANUAL) {
                s_mode  = R_IDLE;          /* 失联：直接回到空闲 */
                s_state = RS_IDLE;
            } else if (s_state == RS_RUN) {
                s_state = RS_ERR;          /* 动作被安全机制打断 */
                s_mode  = R_IDLE;
            }
            if      (sf.stall)   pushDone(D_ALERT, 2, 0, 0);   /* 堵转       */
            else if (sf.critBat) pushDone(D_ALERT, 3, 0, 0);   /* 严重低压   */
            else if (sf.edge)    pushDone(D_ALERT, 4, 0, 0);   /* 边缘       */
            else if (sf.hbLost)  pushDone(D_ALERT, 5, 0, 0);   /* 心跳超时   */
        }

        /* ---- ⑥ 模式机 ---- */
        switch (s_mode) {
            case R_IDLE:
            case R_ESTOP:
                allStop();
                break;

            case R_MANUAL: {
                /* 差速混合：x>0 表示右转，因此角速度取负 */
                float vmax  = c.maxSpeed * (s_mspd > 0 ? s_mspd / 100.0f : 1.0f)
                              * sf.speedScale;
                float v     = s_my * vmax;
                float omega = -s_mx * c.maxOmega * sf.speedScale;   /* °/s    */
                float diff  = kinOmegaToDiff(omega);                /* cm/s   */

                float vL = constrain(v - diff, -vmax * 1.5f, vmax * 1.5f);
                float vR = constrain(v + diff, -vmax * 1.5f, vmax * 1.5f);
                driveWheels(vL, vR, c, dt, 1.0f);
                break;
            }

            case R_MOVE: {
                /* 已完成脉冲数：四轮取平均 */
                float avg = 0;
                for (int i = 0; i < 4; i++) avg += (motorPulses(i) - s_startP[i]);
                avg /= 4.0f;

                float remainP  = s_targetPulses - avg;
                float remainCm = kinPulsesToCm(remainP);
                float vmax     = s_vmax * sf.speedScale;

                /* 位置环：梯形规划按剩余距离给出目标速度 */
                float v = s_trap.update(fabsf(remainCm), vmax, c.accel, c.decel, dt);
                if (remainP < 0) v = -v;

                /* 航向环：保持出发时的朝向；err = 目标(0) − 当前相对转角 */
                s_hPid.set(c.hKp, c.hKi, c.hKd);
                s_hPid.setLimit(-vmax, vmax, -vmax, vmax);
                float yawErr = wrap180(s_yawRef - gImu.yaw);
                float corr   = s_hPid.update(yawErr, dt);

                driveWheels(v + corr, v - corr, c, dt, 1.0f);

                s_tm.progress = (fabsf(s_targetPulses) > 1e-3f)
                              ? (1.0f - fabsf(remainP) / fabsf(s_targetPulses)) : 1.0f;

                /* 到位判定：剩余量小于 0.4cm 当量，或已越过目标且偏差很小 */
                bool done = (fabsf(remainP) <= kinCmToPulses(0.4f)) ||
                            (remainP * s_targetPulses < 0 && fabsf(remainP) < kinCmToPulses(1.5f));

                if (done || (now - s_actT0 > ACTION_TIMEOUT_MS)) {
                    allStop();
                    s_state = (now - s_actT0 > ACTION_TIMEOUT_MS) ? RS_ERR : RS_DONE;

                    float actualCm = kinPulsesToCm(avg);
                    if (s_calMode == CAL_ODO) {
                        s_calPulses   = fabsf(avg);
                        s_calPending  = CAL_ODO;
                        pushDone(D_CAL_ODO_WAIT, CAL_ODO, c.calDist, actualCm);
                        s_calMode = CAL_NONE;
                    } else {
                        pushDone(D_MOVE, 0, s_targetDeg, actualCm);
                    }
                    s_mode = R_IDLE;
                }
                break;
            }

            case R_TURN: {
                float pL = (motorPulses(0) + motorPulses(2)) / 2.0f - s_startL;
                float pR = (motorPulses(1) + motorPulses(3)) / 2.0f - s_startR;
                float prog    = (pR - pL) / 2.0f;        /* 已完成的旋转量    */
                float remainP = s_targetPulses - prog;
                float vmax    = s_vmax * sf.speedScale;

                if (s_turnPhase == 0) {
                    /* ---- 阶段 0：按脉冲目标做梯形规划 ---- */
                    float remainCm = kinPulsesToCm(remainP);
                    float v   = s_trap.update(fabsf(remainCm), vmax, c.accel, c.decel, dt);
                    float sgn = (remainP >= 0) ? 1.0f : -1.0f;

                    /* 对称误差：pL + pR 应为 0，非零说明单侧打滑更多，
                     * 用纯平动去补偿（不影响旋转量，只把车拉回中心）。 */
                    float eSym = kinPulsesToCm((pL + pR) / 2.0f);
                    s_symPid.set(c.hKp, 0, c.hKd);
                    s_symPid.setLimit(-vmax * 0.6f, vmax * 0.6f, 0, 0);
                    float vTrans = -s_symPid.update(eSym, dt);

                    driveWheels(-sgn * v + vTrans, sgn * v + vTrans, c, dt, 1.0f);

                    s_tm.progress = (fabsf(s_targetPulses) > 1e-3f)
                                  ? (0.9f * (1.0f - fabsf(remainP) / fabsf(s_targetPulses)))
                                  : 1.0f;

                    bool done = (fabsf(remainP) <= kinCmToPulses(0.3f)) ||
                                (remainP * s_targetPulses < 0);
                    if (done || (now - s_actT0 > ACTION_TIMEOUT_MS)) {
                        allStop();
                        if (gImu.mpuOK) { s_turnPhase = 1; s_phaseT0 = millis(); }
                        else            { s_turnPhase = 2; }
                    }

                } else if (s_turnPhase == 1) {
                    /* ---- 阶段 1：IMU 精修 ----
                     * 驼峰轮打滑让脉冲法必然有残差，这里以融合偏航角为准低速补齐。 */
                    float yawErr = wrap180(s_targetDeg - wrap180(gImu.yaw - s_yawRef));
                    if (fabsf(yawErr) > 1.0f) {
                        float v = copysignf(fminf(vmax * 0.35f, 12.0f), yawErr);
                        driveWheels(-v, v, c, dt, 1.0f);   /* yawErr>0 → 继续左转 */
                    } else {
                        allStop();
                        s_turnPhase = 2;
                    }
                    if (now - s_phaseT0 > TRIM_TIMEOUT_MS) { allStop(); s_turnPhase = 2; }

                } else {
                    /* ---- 阶段 2：收尾结算 ---- */
                    float actualDeg = gImu.mpuOK
                                    ? wrap180(gImu.yaw - s_yawRef)
                                    : kinPulsesToDeg(prog);

                    if (s_calMode == CAL_TURN) {
                        s_calMeasDeg  = actualDeg;
                        s_calPending  = CAL_TURN;
                        pushDone(D_CAL_TURN_WAIT, CAL_TURN, s_calTargetDeg, actualDeg);
                        s_calMode = CAL_NONE;
                    } else {
                        pushDone(D_TURN, 0, s_targetDeg, actualDeg);
                    }
                    s_state = RS_DONE;
                    s_mode  = R_IDLE;
                    s_tm.progress = 1.0f;
                }
                break;
            }

            case R_CAL_IMU: {
                /* 零偏采样期间必须保持静止 */
                allStop();
                s_calSum += gImu.gz;
                s_calN++;

                if (now - s_calT0 >= CAL_IMU_MS && s_calN > 10) {
                    float b = (float)(s_calSum / s_calN);
                    if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);
                    gCfg.gzBias = b;
                    if (gCfgMutex) xSemaphoreGive(gCfgMutex);
                    cfgSave();
                    pushDone(D_CAL_IMU, CAL_IMU, 0, b);
                    s_mode  = R_IDLE;
                    s_state = RS_DONE;
                }
                break;
            }

            default:
                break;
        }

        /* ---- ⑦ 遥测快照（50Hz） ---- */
        if (now - lastTm >= TM_PERIOD_MS) {
            lastTm = now;
            if (s_tmMtx && xSemaphoreTake(s_tmMtx, pdMS_TO_TICKS(2)) == pdTRUE) {
                for (int i = 0; i < 4; i++) {
                    s_tm.rpm[i]    = motorRPM(i);
                    s_tm.pulses[i] = motorPulses(i);
                    s_tm.stall[i]  = motorIsStalled(i);
                }
                s_tm.spd     = (motorSpeed(0) + motorSpeed(1) +
                                motorSpeed(2) + motorSpeed(3)) / 4.0f;
                s_tm.yaw     = gImu.yaw;
                s_tm.yawRate = gImu.yawRate;
                s_tm.odoCm   = s_odoCm;
                s_tm.bat     = gSen.bat;

                for (int i = 0; i < 6; i++) s_tm.ir[i]   = gSen.ir[i];
                for (int i = 0; i < 4; i++) s_tm.gray[i] = gSen.gray[i];
                for (int i = 0; i < 3; i++) s_tm.e18[i]  = gSen.e18[i];

                s_tm.acc[0] = gImu.ax;
                s_tm.acc[1] = gImu.ay;
                s_tm.acc[2] = gImu.az;
                s_tm.pitch  = gImu.pitch;
                s_tm.roll   = gImu.roll;
                s_tm.mpuOK  = gImu.mpuOK;
                s_tm.adxlOK = gImu.adxlOK;

                s_tm.mode = (uint8_t)s_mode;
                s_tm.state = (uint8_t)s_state;
                if (s_mode != R_MOVE && s_mode != R_TURN) s_tm.progress = 0;

                s_tm.estop      = s_estop;
                s_tm.lowBat     = sf.lowBat || sf.critBat;
                s_tm.edge       = sf.edge;
                s_tm.irLimit    = sf.irLimit;
                s_tm.calPending = s_calPending;
                s_tm.servoUS    = servoCurrentUS();
                s_tm.uptime     = now;

                xSemaphoreGive(s_tmMtx);
            }
        }
    }
}

/**
 * @brief   投递指令到控制核
 * @param[in] c  指令包
 * @return  true 投递成功
 */
bool robotPushCmd(const Cmd& c) {
    return s_cmdQ && (xQueueSend(s_cmdQ, &c, 0) == pdTRUE);
}

/**
 * @brief   取回一条回传事件
 * @param[out] d  事件包
 * @return  true 取到事件
 */
bool robotPopDone(DoneMsg& d) {
    return s_doneQ && (xQueueReceive(s_doneQ, &d, 0) == pdTRUE);
}

/**
 * @brief   拷贝遥测快照
 * @param[out] out  遥测结构体
 * @return   无
 */
void robotCopyTelemetry(Telemetry& out) {
    if (!s_tmMtx) return;
    if (xSemaphoreTake(s_tmMtx, pdMS_TO_TICKS(5)) == pdTRUE) {
        out = s_tm;
        xSemaphoreGive(s_tmMtx);
    }
}

/**
 * @brief   读取最近心跳时间戳
 * @param   无
 * @return  最近心跳时间戳 ms
 */
uint32_t robotLastHbMs() { return s_lastHb; }
