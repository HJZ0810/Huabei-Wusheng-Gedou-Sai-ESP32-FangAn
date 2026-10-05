/**
 ******************************************************************************
 * @file    motor.cpp
 * @brief   4×JGB37-3625 驱动实现（LEDC 调速 + 方向 IO + FG 带符号计数）+ 舵机
 *
 * @details 归一化指令 → 硬件动作的换算链：
 *          @code
 *            norm ──▶ 极性反转 ──▶ 侧向缩放 ──▶ 死区补偿 ──▶ 限幅 ──▶ PWM + DIR
 *          @endcode
 *          FG 脉冲在中断里按 @ref s_dir 的符号累加，因此「后退时脉冲数会减少」，
 *          这是位置环能正常收敛的前提（否则里程永远只增不减，闭环直接失控）。
 *
 * @author  CombatBot 电控组
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期2026-10-05       <th>版本  <th>作者：HJZ   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>LEDC 跨版本兼容宏；统一注释规范
 *          </table>
 *
 * @warning 蓝线调速规格是 0~5V；ESP32 的 PWM 只有 3.3V，必须经电平转换器抬升，
 *          否则电机最高只能跑到约 2/3 转速。
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include "motor.h"
#include "cfg.h"

/* ==========================================================================
 *                          私有宏定义（LEDC 跨版本兼容）
 * ========================================================================== */
/*
 * arduino-esp32 3.x 把 LEDC 接口从「按通道」改成了「按引脚」：
 *   2.x：ledcSetup(ch, freq, res) + ledcAttachPin(pin, ch) + ledcWrite(ch, duty)
 *   3.x：ledcAttach(pin, freq, res) + ledcWrite(pin, duty)
 * 用下面两个宏封装差异，业务代码无需关心 core 版本。
 */
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    #define LEDC_ATTACH(pin, ch, freq, res)  ledcAttach((pin), (freq), (res))
    #define LEDC_WRITE(pin, ch, duty)        ledcWrite((pin), (uint32_t)(duty))
#else
    #define LEDC_ATTACH(pin, ch, freq, res)  do { \
                                                 ledcSetup((ch), (freq), (res)); \
                                                 ledcAttachPin((pin), (ch));     \
                                             } while (0)
    #define LEDC_WRITE(pin, ch, duty)        ledcWrite((ch), (uint32_t)(duty))
#endif

/* ==========================================================================
 *                          私有常量与静态数据
 * ========================================================================== */
/** @brief 方向线引脚表（索引：0=左前 1=右前 2=左后 3=右后） */
static const uint8_t DIR_PIN[MOTOR_COUNT] = { PIN_M1_DIR, PIN_M2_DIR, PIN_M3_DIR, PIN_M4_DIR };
/** @brief 调速 PWM 引脚表 */
static const uint8_t PWM_PIN[MOTOR_COUNT] = { PIN_M1_PWM, PIN_M2_PWM, PIN_M3_PWM, PIN_M4_PWM };
/** @brief FG 测速脉冲引脚表 */
static const uint8_t FG_PIN[MOTOR_COUNT]  = { PIN_M1_FG,  PIN_M2_FG,  PIN_M3_FG,  PIN_M4_FG  };
/** @brief 左轮标记（用于施加左/右侧出力缩放） */
static const bool    IS_LEFT[MOTOR_COUNT] = { true, false, true, false };

/** @brief FG 累计带符号脉冲数（中断内修改，任务内读取） */
static volatile int32_t s_pulse[MOTOR_COUNT] = { 0, 0, 0, 0 };
/** @brief FG 计数方向符号：+1 前进，-1 后退（★ FG 本身没有方向信息） */
static volatile int8_t  s_dir[MOTOR_COUNT]   = { 1, 1, 1, 1 };

/** @brief 上一结算周期的脉冲数（用于求增量） */
static int32_t  s_lastPulse[MOTOR_COUNT] = { 0, 0, 0, 0 };
/** @brief 当前窗口内的脉冲增量累加器 */
static int32_t  s_acc[MOTOR_COUNT]       = { 0, 0, 0, 0 };
/** @brief 当前窗口累计时长，单位 ms */
static uint32_t s_accMs[MOTOR_COUNT]     = { 0, 0, 0, 0 };
/** @brief 结算得到的转速 rpm */
static float    s_rpm[MOTOR_COUNT]       = { 0, 0, 0, 0 };
/** @brief 结算得到的线速度 cm/s */
static float    s_speed[MOTOR_COUNT]     = { 0, 0, 0, 0 };
/** @brief 无脉冲持续时间，单位 ms（用于去尾与堵转判定） */
static uint32_t s_idleMs[MOTOR_COUNT]    = { 0, 0, 0, 0 };
/** @brief 当前实际输出占空比 */
static uint16_t s_duty[MOTOR_COUNT]      = { 0, 0, 0, 0 };
/** @brief 疑似堵转持续时间，单位 ms */
static uint32_t s_stallMs[MOTOR_COUNT]   = { 0, 0, 0, 0 };
/** @brief 堵转标志 */
static bool     s_stall[MOTOR_COUNT]     = { false, false, false, false };

/* ==========================================================================
 *                          私有函数（文件内静态）
 * ========================================================================== */

/**
 * @brief   FG 脉冲中断服务函数（IRAM 常驻）
 * @param[in] arg  电机索引（由 attachInterruptArg 传入）
 * @return   无
 * @note     只统计上升沿：一个上升沿 = 一个脉冲。
 *           若硬件改为双边沿触发，等效 PPR 会翻倍，需同步修正配置中的 ppr。
 * @warning  函数体内禁止调用任何非 IRAM 安全的 API（如 Serial、malloc）。
 */
static void IRAM_ATTR fgIsr(void* arg) {
    int i = (int)(uintptr_t)arg;
    s_pulse[i] += s_dir[i];         /* ★ 带符号累加：方向由上一条指令决定 */
}

/* ==========================================================================
 *                              对外函数实现 · 电机
 * ========================================================================== */

/**
 * @brief   初始化电机与舵机
 * @param   无
 * @return  无
 */
void motorInit() {
    for (int i = 0; i < MOTOR_COUNT; i++) {
        /* 方向线：默认低电平（CW），且 PWM=0 时电机不会转 */
        pinMode(DIR_PIN[i], OUTPUT);
        digitalWrite(DIR_PIN[i], LOW);

        /* 调速 PWM：上电占空比强制为 0 —— 安全设计基线 */
        pinMode(PWM_PIN[i], OUTPUT);
        LEDC_ATTACH(PWM_PIN[i], i, PWM_FREQ_MOTOR, PWM_RES_MOTOR);
        LEDC_WRITE(PWM_PIN[i], i, 0);

        /* FG：硬件侧已完成 5V→3.3V 分压，此处直接作为输入捕获 */
        pinMode(FG_PIN[i], INPUT);
        s_pulse[i] = 0;
        s_dir[i]   = 1;
        attachInterruptArg(FG_PIN[i], fgIsr, (void*)(uintptr_t)i, RISING);
    }

    servoInit();                    /* 舵机上电回中 */
}

/**
 * @brief   设置单个电机的归一化输出
 * @param[in] i     电机索引：0=左前，1=右前，2=左后，3=右后
 * @param[in] norm  归一化速度指令 [-1.0, +1.0]
 * @return   无
 */
void motorSetNorm(uint8_t i, float norm) {
    if (i >= MOTOR_COUNT) return;

    CfgSnap cs;
    const Cfg& c = cs.c;

    /* ① 极性反转：翻转指令符号，方向线与计数符号会随之翻转，保证里程符号一致 */
    float n = norm;
    if (c.invert[i]) n = -n;

    /* ② 侧向缩放：补偿左右侧机械差异（电机个体差 / 轮胎磨损） */
    n *= (IS_LEFT[i] ? c.scaleL : c.scaleR);
    if (!IS_LEFT[i]) n *= (1.0f + c.balLR);
    n = constrain(n, -1.0f, 1.0f);

    /* ③ 方向线 + FG 计数符号（仅在有输出时更新，滑行阶段保持上次符号） */
    if (fabsf(n) > 0.001f) {
        digitalWrite(DIR_PIN[i], n >= 0 ? HIGH : LOW);   /* 高电平 = CCW = 前进 */
        s_dir[i] = (n >= 0) ? 1 : -1;
    }

    /* ④ 死区补偿：静摩擦会让小占空比完全转不动，一旦要转就直接跨过死区 */
    float a = fabsf(n);
    uint16_t duty = 0;
    if (a > 0.001f) duty = (uint16_t)(c.pwmDead + a * (c.pwmMax - c.pwmDead));
    duty = (uint16_t)constrain((int)duty, 0, (int)c.pwmMax);

    s_duty[i] = duty;
    LEDC_WRITE(PWM_PIN[i], i, duty);
}

/**
 * @brief   停止单个电机
 * @param[in] i  电机索引
 * @return   无
 */
void motorStop(uint8_t i) {
    if (i >= MOTOR_COUNT) return;
    LEDC_WRITE(PWM_PIN[i], i, 0);
    s_duty[i] = 0;
}

/**
 * @brief   停止全部电机
 * @param   无
 * @return  无
 */
void motorStopAll() {
    for (int i = 0; i < MOTOR_COUNT; i++) motorStop(i);
}

/**
 * @brief   周期结算转速 / 线速度 / 堵转
 * @param[in] dt  调用间隔，单位 s
 * @return   无
 */
void motorUpdate(float dt) {
    CfgSnap cs;
    const Cfg& c = cs.c;

    float ppr    = (c.ppr > 1.0f) ? c.ppr : 90.0f;
    float circCm = 3.14159265f * c.wheelDia / 10.0f;     /* 轮周长，单位 cm   */
    uint32_t dtMs = (uint32_t)(dt * 1000.0f);

    for (int i = 0; i < MOTOR_COUNT; i++) {
        /* ---- 脉冲增量 ---- */
        int32_t cur = s_pulse[i];
        int32_t d   = cur - s_lastPulse[i];
        s_lastPulse[i] = cur;
        s_acc[i]   += d;
        s_accMs[i] += dtMs;

        /* ---- 停转计时（用于去尾与堵转判定） ---- */
        if (d != 0) s_idleMs[i] = 0; else s_idleMs[i] += dtMs;

        /* ---- 50ms 窗口结算 ---- */
        if (s_accMs[i] >= 50) {
            /* rpm = 脉冲数 / 时间(min) / 每转脉冲数 */
            s_rpm[i]   = (float)s_acc[i] * 60000.0f / (ppr * (float)s_accMs[i]);
            s_speed[i] = s_rpm[i] / 60.0f * circCm;      /* cm/s */
            s_acc[i]   = 0;
            s_accMs[i] = 0;

            /* 长时间无脉冲 → 主动清零，避免低速残留读数 */
            if (s_idleMs[i] > 200) { s_rpm[i] = 0; s_speed[i] = 0; }
        }

        /* ---- 堵转判定：占空比明显高于死区却持续无脉冲 ---- */
        if (s_duty[i] > (uint16_t)(c.pwmDead * 1.5f) && s_idleMs[i] > 120) {
            s_stallMs[i] += dtMs;
            if (s_stallMs[i] > c.stallTime) s_stall[i] = true;
        } else {
            s_stallMs[i] = 0;
            s_stall[i]   = false;
        }
    }
}

/**
 * @brief   读取累计带符号脉冲数
 * @param[in] i  电机索引
 * @return  累计脉冲数
 */
int32_t motorPulses(uint8_t i)   { return (i < MOTOR_COUNT) ? s_pulse[i] : 0; }

/**
 * @brief   清零某电机脉冲计数
 * @param[in] i  电机索引
 * @return   无
 */
void motorResetPulses(uint8_t i) { if (i < MOTOR_COUNT) s_pulse[i] = 0; }

/**
 * @brief   清零全部电机脉冲计数
 * @param   无
 * @return  无
 */
void motorResetAllPulses()       { for (int i = 0; i < MOTOR_COUNT; i++) s_pulse[i] = 0; }

/**
 * @brief   读取转速
 * @param[in] i  电机索引
 * @return  带符号转速 rpm
 */
float motorRPM(uint8_t i)        { return (i < MOTOR_COUNT) ? s_rpm[i] : 0; }

/**
 * @brief   读取线速度
 * @param[in] i  电机索引
 * @return  带符号线速度 cm/s
 */
float motorSpeed(uint8_t i)      { return (i < MOTOR_COUNT) ? s_speed[i] : 0; }

/**
 * @brief   读取当前占空比
 * @param[in] i  电机索引
 * @return  占空比 0~1023
 */
uint16_t motorDuty(uint8_t i)    { return (i < MOTOR_COUNT) ? s_duty[i] : 0; }

/**
 * @brief   查询堵转状态
 * @param[in] i  电机索引
 * @return  true 表示堵转
 */
bool motorIsStalled(uint8_t i)   { return (i < MOTOR_COUNT) ? s_stall[i] : false; }

/* ==========================================================================
 *                              对外函数实现 · 舵机
 * ========================================================================== */
static int s_servoUS = 1500;    /**< 舵机当前脉宽，单位 µs */

/**
 * @brief   初始化舵机
 * @param   无
 * @return  无
 */
void servoInit() {
    pinMode(PIN_SERVO, OUTPUT);
    LEDC_ATTACH(PIN_SERVO, 4, PWM_FREQ_SERVO, PWM_RES_SERVO);
    servoSetUS(1500);               /* 上电回中 */
}

/**
 * @brief   按脉宽设置舵机位置（带机械限位截断）
 * @param[in] us  目标脉宽，单位 µs
 * @return   无
 */
void servoSetUS(int us) {
    CfgSnap cs;
    const Cfg& c = cs.c;

    us = constrain(us, c.servoMin, c.servoMax);      /* 禁止超出机械极限 */
    s_servoUS = us;

    /* 50Hz 周期为 20000µs，16bit 分辨率 → 占空比 = us / 20000 × 65536 */
    uint32_t duty = (uint32_t)((float)us * 65536.0f / 20000.0f);
    LEDC_WRITE(PIN_SERVO, 4, duty);
}

/**
 * @brief   舵机回中
 * @param   无
 * @return  无
 */
void servoCenter() { CfgSnap cs; servoSetUS(cs.c.servoCenter); }

/**
 * @brief   举铲
 * @param   无
 * @return  无
 */
void servoUp()     { CfgSnap cs; servoSetUS(cs.c.servoUp); }

/**
 * @brief   放铲
 * @param   无
 * @return  无
 */
void servoDown()   { CfgSnap cs; servoSetUS(cs.c.servoDown); }

/**
 * @brief   读取舵机当前脉宽
 * @param   无
 * @return  当前脉宽 µs
 */
int servoCurrentUS() { return s_servoUS; }
