/**
 ******************************************************************************
 * @file    motor.cpp
 * @brief   4×JGB37-3625 驱动实现（LEDC 调速 + 方向 IO + FG 带符号计数）+ 舵机
 *
 * @details 归一化指令 → 硬件动作的换算链：
 *          @code
 *            norm ──▶ 有限性校验 ──▶ 极性反转 ──▶ 侧向缩放 ──▶ 换向保护
 *                 ──▶ 死区补偿 ──▶ 限幅 ──▶ PWM + DIR
 *          @endcode
 *
 *          三项关键设计（本版相对初版的强化点）：
 *
 *          ① **FG 用 PCNT 硬件计数，不再靠 GPIO 中断软件累加**
 *             PCNT 是 ESP32 的专用脉冲计数器外设：边沿计数在硬件里完成，
 *             不产生中断风暴、不占用 CPU，而且自带毛刺滤波（滤掉 <1.25µs 的抖动）。
 *             初版用 attachInterrupt 在 ISR 里累加，高转速下既吃 CPU 又可能丢边沿。
 *             PCNT 初始化失败时自动降级为中断计数（灯不会瞎，只是精度差一点）。
 *
 *          ② **换向保护（★ 机械保护，初版完全没有）**
 *             满速前进时直接给反向 PWM，等于让电机瞬间反接——会打齿轮、
 *             引发大电流冲击，还会被堵转判据误判。本模块在换向时先撤驱动，
 *             等到「距末次 FG 边沿 ≥120ms」且「滤波线速度 ≤0.8cm/s」才真正翻转方向线。
 *             等待是跨调用进行的，不使用任何 delay，不阻塞控制周期。
 *
 *          ③ **非有限数（NaN / Inf）一律撤驱动**
 *             上层 PID 一旦被非法输入污染，输出会变成 NaN 并一路传播到 PWM。
 *             这里在出口处统一拦截，保证「坏输入 = 停车」而不是「坏输入 = 满速」。
 *
 * @author  HJZ
 * @version V2.0.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>LEDC 跨版本兼容宏；统一注释规范
 *          <tr><td>2026-10-05 <td>V2.0  <td>电控组 <td>融合版：PCNT 计数 + 换向保护 + 非有限数拦截
 *          </table>
 *
 * @warning 蓝线调速规格是 0~5V；ESP32 的 PWM 只有 3.3V，必须经电平转换器抬升，
 *          否则电机最高只能跑到约 2/3 转速。
 * @warning PWM=0 只是撤去驱动，不是主动制动；车体仍会惯性滑行。
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include "motor.h"
#include "cfg.h"
#include <math.h>

/* ---- PCNT 可用性探测：IDF 5.x 仍保留 legacy 接口，若被移除则自动降级 ----
 * 说明：IDF 5.x 把 PCNT 拆成了「legacy driver/pcnt.h」与新版
 * driver/pulse_cnt.h，前者在包含时会主动抛一条 #warning 提示迁移。
 * 本项目要同时兼容 arduino-esp32 2.x（IDF 4.4，只有 legacy 接口），
 * 因此保留 legacy 调用，仅在 3.x 下把这条迁移提示压掉，
 * 避免它淹没真正需要注意的工程告警。 */
#if defined(__has_include)
    #if __has_include(<driver/pcnt.h>)
        #define CB_HAS_PCNT 1
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wcpp"
        #include <driver/pcnt.h>
        #pragma GCC diagnostic pop
    #endif
#endif

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
 *                          私有常量定义
 * ========================================================================== */
/** @brief PCNT 计数上限；单次采样间隔内边沿数必须远小于它才能正确展开回卷 */
#define PCNT_LIMIT          30000
/** @brief PCNT 毛刺滤波阈值：APB 80MHz 下 100 个周期 ≈ 1.25µs */
#define PCNT_FILTER_CYCLES  100
/** @brief 换向等待：距末次 FG 边沿至少 120ms */
#define REVERSE_WAIT_MS     120
/** @brief 换向等待：滤波线速度绝对值不高于 0.8 cm/s */
#define REVERSE_MAX_CMPS    0.8f
/** @brief 转速/线速度的结算窗口，单位 ms */
#define SPEED_WINDOW_MS     50

/* ==========================================================================
 *                          私有类型与静态数据
 * ========================================================================== */

/** @brief 单轮运行时状态 */
struct Wheel {
    bool     pcnt;          /**< true=使用 PCNT 硬件计数；false=降级为中断计数   */
    int      sign;          /**< 当前逻辑驱动方向 +1/-1（不是 FG 实测方向）      */
    bool     appliedInvert; /**< 上次生效的极性反转配置（配置热改后需重新对齐）  */
    int      lastCount;     /**< 上次读到的原始计数值（用于求增量）              */
    int64_t  total;         /**< 带符号累计脉冲（内部 64 位，防长时间溢出）      */
    uint32_t lastEdgeMs;    /**< 最近一次出现 FG 边沿的时间戳 ms                 */
    uint16_t duty;          /**< 当前实际输出占空比 0~1023                       */
    volatile uint32_t irq;  /**< PCNT 不可用时的中断计数（IRAM 内递增）          */
};

/** @brief 方向线引脚表（索引：0=左前 1=右前 2=左后 3=右后） */
static const uint8_t DIR_PIN[MOTOR_COUNT] = { PIN_M1_DIR, PIN_M2_DIR, PIN_M3_DIR, PIN_M4_DIR };
/** @brief 调速 PWM 引脚表 */
static const uint8_t PWM_PIN[MOTOR_COUNT] = { PIN_M1_PWM, PIN_M2_PWM, PIN_M3_PWM, PIN_M4_PWM };
/** @brief FG 测速脉冲引脚表 */
static const uint8_t FG_PIN[MOTOR_COUNT]  = { PIN_M1_FG,  PIN_M2_FG,  PIN_M3_FG,  PIN_M4_FG  };
/** @brief 左轮标记（用于施加左/右侧出力缩放） */
static const bool    IS_LEFT[MOTOR_COUNT] = { true, false, true, false };

static Wheel s_w[MOTOR_COUNT];              /**< 四轮运行时状态                */

/* ---- 结算结果（供上层读取） ---- */
static int32_t  s_pulse[MOTOR_COUNT] = { 0, 0, 0, 0 };  /**< 对外暴露的带符号脉冲 */
static int32_t  s_acc[MOTOR_COUNT]   = { 0, 0, 0, 0 };  /**< 窗口内脉冲增量累加   */
static uint32_t s_accMs[MOTOR_COUNT] = { 0, 0, 0, 0 };  /**< 窗口累计时长 ms      */
static float    s_rpm[MOTOR_COUNT]   = { 0, 0, 0, 0 };  /**< 结算转速 rpm         */
static float    s_speed[MOTOR_COUNT] = { 0, 0, 0, 0 };  /**< 结算线速度 cm/s      */
static uint32_t s_idleMs[MOTOR_COUNT]= { 0, 0, 0, 0 };  /**< 无脉冲持续时长 ms    */
static uint32_t s_stallMs[MOTOR_COUNT] = { 0, 0, 0, 0 };/**< 疑似堵转持续时长 ms  */
static bool     s_stall[MOTOR_COUNT] = { false, false, false, false }; /**< 堵转标志 */

/* ==========================================================================
 *                          私有函数（文件内静态）
 * ========================================================================== */

/** @brief PCNT 不可用时的四路中断入口（IRAM 常驻，禁止调用非 IRAM 安全 API）
 *  @note  这里刻意写 `x = x + 1` 而不是 `x++`：
 *         GCC 13 起对 volatile 限定类型做自增/复合赋值会触发 -Wvolatile
 *         （该语义在 C++20 已被废弃）。写法等价且不会告警。 */
static void IRAM_ATTR fgIsr0() { s_w[0].irq = s_w[0].irq + 1; }
static void IRAM_ATTR fgIsr1() { s_w[1].irq = s_w[1].irq + 1; }
static void IRAM_ATTR fgIsr2() { s_w[2].irq = s_w[2].irq + 1; }
static void IRAM_ATTR fgIsr3() { s_w[3].irq = s_w[3].irq + 1; }
static void (*const FG_ISR[MOTOR_COUNT])() = { fgIsr0, fgIsr1, fgIsr2, fgIsr3 };

/**
 * @brief   尝试为某一路 FG 建立 PCNT 硬件计数
 * @param[in] i  电机索引
 * @return  true 建立成功；false 失败（调用方应降级为中断计数）
 * @note    只统计上升沿：一个上升沿 = 一个脉冲。若硬件改为双边沿，
 *          等效 PPR 会翻倍，需同步修正配置中的 ppr。
 */
static bool pcntSetup(uint8_t i) {
#if defined(CB_HAS_PCNT)
    pcnt_config_t cfg = {};
    cfg.pulse_gpio_num = FG_PIN[i];
    cfg.ctrl_gpio_num  = PCNT_PIN_NOT_USED;
    cfg.channel        = (pcnt_channel_t)(PCNT_CHANNEL_0 + (i % 8));
    cfg.unit           = (pcnt_unit_t)i;
    cfg.pos_mode       = PCNT_COUNT_INC;      /* 上升沿 +1 */
    cfg.neg_mode       = PCNT_COUNT_DIS;      /* 下降沿忽略 */
    cfg.lctrl_mode     = PCNT_MODE_KEEP;
    cfg.hctrl_mode     = PCNT_MODE_KEEP;
    cfg.counter_h_lim  = PCNT_LIMIT;
    cfg.counter_l_lim  = -1;

    if (pcnt_unit_config(&cfg) != ESP_OK) return false;
    pcnt_set_filter_value((pcnt_unit_t)i, PCNT_FILTER_CYCLES);   /* 硬件毛刺滤波 */
    pcnt_filter_enable((pcnt_unit_t)i);
    pcnt_counter_pause((pcnt_unit_t)i);
    pcnt_counter_clear((pcnt_unit_t)i);
    return pcnt_counter_resume((pcnt_unit_t)i) == ESP_OK;
#else
    (void)i;
    return false;
#endif
}

/**
 * @brief   读取某一路 FG 的原始计数增量（自动展开 PCNT 回卷）
 * @param[in] i  电机索引
 * @return  本次调用以来的边沿增量（恒为非负）
 */
static int pcntDelta(uint8_t i) {
    Wheel& w = s_w[i];
    int delta = 0;

    if (w.pcnt) {
#if defined(CB_HAS_PCNT)
        int16_t raw = 0;
        if (pcnt_get_counter_value((pcnt_unit_t)i, &raw) == ESP_OK) {
            delta    = (int)raw - w.lastCount;
            /* PCNT 达到高限后归零，负差值补回一个计数周期。
               保留连续计数（读后不清零）可避开"清零窗口丢边沿"的经典问题。 */
            if (delta < 0) delta += PCNT_LIMIT;
            w.lastCount = raw;
        }
#endif
    } else {
        uint32_t raw = w.irq;
        delta        = (int)(uint32_t)(raw - (uint32_t)w.lastCount);
        w.lastCount  = (int)raw;
    }
    return delta;
}

/* ==========================================================================
 *                              对外函数实现 · 电机
 * ========================================================================== */

/**
 * @brief   初始化电机与舵机
 * @param   无
 * @return  无
 * @note    上电顺序：先把 PWM 引脚拉低并配置 LEDC 输出 0，再建立计数通道。
 *          这样即使后续初始化失败，电机也不会意外转动。
 */
void motorInit() {
    for (int i = 0; i < MOTOR_COUNT; i++) {
        Wheel& w = s_w[i];
        w.pcnt   = false;
        w.sign   = 1;
        w.appliedInvert = false;
        w.lastCount     = 0;
        w.total         = 0;
        w.lastEdgeMs    = millis();
        w.duty          = 0;
        w.irq           = 0;

        /* 方向线：默认低电平（CW），且 PWM=0 时电机不会转 */
        pinMode(DIR_PIN[i], OUTPUT);
        digitalWrite(DIR_PIN[i], LOW);

        /* 调速 PWM：上电占空比强制为 0 —— 安全设计基线 */
        pinMode(PWM_PIN[i], OUTPUT);
        LEDC_ATTACH(PWM_PIN[i], i, PWM_FREQ_MOTOR, PWM_RES_MOTOR);
        LEDC_WRITE(PWM_PIN[i], i, 0);

        /* FG：硬件侧已完成 5V→3.3V 分压，此处直接作为输入捕获 */
        pinMode(FG_PIN[i], INPUT);
        w.pcnt = pcntSetup(i);
        if (!w.pcnt) {
            attachInterrupt(digitalPinToInterrupt(FG_PIN[i]), FG_ISR[i], RISING);
        }

        s_pulse[i] = 0;
    }

    servoInit();                    /* 舵机上电回中 */
}

/**
 * @brief   设置单个电机的归一化输出
 * @param[in] i     电机索引：0=左前，1=右前，2=左后，3=右后
 * @param[in] norm  归一化速度指令 [-1.0, +1.0]
 * @return   无
 * @note     NaN / Inf 一律按撤驱动处理，绝不写入 PWM。
 */
void motorSetNorm(uint8_t i, float norm) {
    if (i >= MOTOR_COUNT) return;
    Wheel& w = s_w[i];

    /* ① 非有限数拦截：坏输入 = 停车，而不是满速 */
    if (!cfgFinite(norm)) {
        w.duty = 0;
        LEDC_WRITE(PWM_PIN[i], i, 0);
        return;
    }

    CfgSnap cs;
    const Cfg& c = cs.c;

    /* ② 极性反转：翻转指令符号，方向线与计数符号会随之翻转，保证里程符号一致 */
    float n = norm;
    if (c.invert[i]) n = -n;

    /* ③ 侧向缩放：补偿左右侧机械差异（电机个体差 / 轮胎磨损） */
    n *= (IS_LEFT[i] ? c.scaleL : c.scaleR);
    if (!IS_LEFT[i]) n *= (1.0f + c.balLR);
    n = constrain(n, -1.0f, 1.0f);

    /* ④ 死区与占空比换算：静摩擦会让小占空比完全转不动，一旦要转就跨过死区 */
    float a = fabsf(n);
    if (a < 0.002f) {                       /* 视为撤驱动 */
        w.duty = 0;
        LEDC_WRITE(PWM_PIN[i], i, 0);
        return;
    }
    uint16_t duty = (uint16_t)(c.pwmDead + a * (c.pwmMax - c.pwmDead));
    duty = (uint16_t)constrain((int)duty, 0, (int)c.pwmMax);

    /* ⑤ 换向保护（★ 机械保护）
          目标方向与当前方向不同时，先撤驱动并观察余转是否衰减：
            条件 A：距末次 FG 边沿 ≥ 120ms
            条件 B：滤波线速度 ≤ 0.8 cm/s
          两个条件都满足才真正翻转方向线并施加新的占空比；
          否则本次只输出 0，把等待时间留给下一个控制周期（不阻塞）。 */
    int desiredSign = (n >= 0) ? 1 : -1;
    if (desiredSign != w.sign || w.appliedInvert != c.invert[i]) {
        w.duty = 0;
        LEDC_WRITE(PWM_PIN[i], i, 0);
        if ((uint32_t)(millis() - w.lastEdgeMs) < REVERSE_WAIT_MS ||
            fabsf(s_speed[i]) > REVERSE_MAX_CMPS) {
            return;                          /* 余转还没停，继续等 */
        }
        w.sign          = desiredSign;
        w.appliedInvert = c.invert[i];
    }

    /* ⑥ 真正输出：方向线 + 占空比 */
    digitalWrite(DIR_PIN[i], (w.sign > 0) ? HIGH : LOW);   /* 高电平 = CCW = 前进 */
    w.duty = duty;
    LEDC_WRITE(PWM_PIN[i], i, duty);
}

/**
 * @brief   停止单个电机
 * @param[in] i  电机索引
 * @return   无
 * @note     保留方向线与计数符号：滑行阶段的脉冲仍能正确计入里程。
 */
void motorStop(uint8_t i) {
    if (i >= MOTOR_COUNT) return;
    s_w[i].duty = 0;
    LEDC_WRITE(PWM_PIN[i], i, 0);
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
 * @brief   周期结算脉冲 / 转速 / 线速度 / 堵转
 * @param[in] dt  调用间隔，单位 s
 * @return   无
 * @note     脉冲增量在这里按「当前逻辑方向」赋符号，因此后退时里程会减少。
 *           由于换向保护保证了翻向时余转已衰减，符号与真实转向是一致的。
 */
void motorUpdate(float dt) {
    CfgSnap cs;
    const Cfg& c = cs.c;

    float ppr    = (c.ppr > 1.0f) ? c.ppr : 90.0f;
    float circCm = 3.14159265f * c.wheelDia / 10.0f;     /* 轮周长，单位 cm */
    uint32_t dtMs = (uint32_t)(dt * 1000.0f);
    if (dtMs == 0) dtMs = 1;

    for (int i = 0; i < MOTOR_COUNT; i++) {
        Wheel& w = s_w[i];

        /* ---- 原始边沿增量 → 带符号累计 ---- */
        int delta = pcntDelta(i);
        if (delta > 0) w.lastEdgeMs = millis();
        w.total += (int64_t)delta * w.sign;
        s_pulse[i] = (int32_t)w.total;

        /* ---- 窗口累加（用于结算速度） ---- */
        s_acc[i]   += delta * w.sign;
        s_accMs[i] += dtMs;

        /* ---- 停转计时（用于去尾与堵转判定） ---- */
        if (delta != 0) s_idleMs[i] = 0; else s_idleMs[i] += dtMs;

        /* ---- 50ms 窗口结算 ---- */
        if (s_accMs[i] >= SPEED_WINDOW_MS) {
            /* rpm = 脉冲数 / 时间(min) / 每转脉冲数 */
            s_rpm[i]   = (float)s_acc[i] * 60000.0f / (ppr * (float)s_accMs[i]);
            s_speed[i] = s_rpm[i] / 60.0f * circCm;      /* cm/s */
            s_acc[i]   = 0;
            s_accMs[i] = 0;

            /* 长时间无脉冲 → 主动清零，避免低速残留读数 */
            if (s_idleMs[i] > 200) { s_rpm[i] = 0; s_speed[i] = 0; }
        }

        /* ---- 堵转判定：占空比明显高于死区却持续无脉冲 ---- */
        if (w.duty > (uint16_t)(c.pwmDead * 1.5f) && s_idleMs[i] > 120) {
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
void motorResetPulses(uint8_t i) { if (i < MOTOR_COUNT) { s_w[i].total = 0; s_pulse[i] = 0; } }

/**
 * @brief   清零全部电机脉冲计数
 * @param   无
 * @return   无
 */
void motorResetAllPulses()       { for (int i = 0; i < MOTOR_COUNT; i++) motorResetPulses(i); }

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
uint16_t motorDuty(uint8_t i)    { return (i < MOTOR_COUNT) ? s_w[i].duty : 0; }

/**
 * @brief   查询堵转状态
 * @param[in] i  电机索引
 * @return  true 表示堵转
 */
bool motorIsStalled(uint8_t i)   { return (i < MOTOR_COUNT) ? s_stall[i] : false; }

/**
 * @brief   查询最近一次 FG 边沿距今的时间
 * @param[in] i  电机索引
 * @return  毫秒数
 * @note    供上层做「余转是否已停」的判据（如精准动作的到位确认）。
 */
uint32_t motorIdleMs(uint8_t i)  { return (i < MOTOR_COUNT) ? s_idleMs[i] : 0; }

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
