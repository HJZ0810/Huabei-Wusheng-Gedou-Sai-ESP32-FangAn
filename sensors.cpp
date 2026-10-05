/**
 ******************************************************************************
 * @file    sensors.cpp
 * @brief   外围传感器采集实现（CD74HC4067 + MCP23017）
 *
 * @details 采集要点：
 *            - 4067 切换通道后必须等待导通稳定（120µs），并丢弃第一次采样；
 *            - 红外测距采用「电压 → 距离」查表 + 线性插值，
 *              因为 GP2Y0A02YK0F 的输出与距离成强非线性反比；
 *            - 电池电压用一阶滤波平滑，避免电机启停时的尖刺误触发低压保护。
 *
 * @author  HJZ
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
 * @note    红外曲线为数据手册典型值，不同批次有 ±10% 偏差，
 *          可用网页里的「红外测距比例修正」整体校准。
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include "sensors.h"
#include "cfg.h"
#include <Wire.h>
#include <Adafruit_MCP23X17.h>

/* ==========================================================================
 *                          私有常量与静态数据
 * ========================================================================== */
/**
 * @brief 电池分压比：100kΩ + 10kΩ → 衰减 11 倍（24V → 2.18V，落在 ADC 量程内）
 */
#define BAT_DIVIDER  11.0f

/* ---- 有效性判据（★ 融合版新增） -------------------------------------- */
/**
 * @brief 红外通道电压下限 V：低于此值判为「断路 / 未接」
 * @details GP2Y0A02YK0F 在 150cm 处仍有约 0.42V 输出，
 *          真正断线时 ADC 读到的是 0V 附近（下拉到地）。
 */
#define IR_V_MIN      0.20f

/**
 * @brief 红外通道电压上限 V：高于此值判为「对 VCC 短路 / 异常」
 * @details 该传感器最近距离（约 18cm）输出也只有 2.6V；
 *          接近 3.3V 只可能是线路故障，若不排除会一直误判"前方极近"。
 */
#define IR_V_MAX      3.20f

/** @brief 电池电压有效区间 V（低于下限视为分压未接） */
#define BAT_V_MIN     3.0f
/** @brief 电池电压有效上限 V（高于此值只可能是分压/ADC 故障） */
#define BAT_V_MAX     60.0f

/**
 * @brief GP2Y0A02YK0F（20~150cm）电压—距离曲线（数据手册典型值）
 * @note  表格按电压降序排列，查询时线性插值；数值仅供换算参考。
 */
static const float IR_CURVE[][2] = {
    { 2.60f,  18.0f }, { 2.20f,  22.0f }, { 1.90f,  26.0f }, { 1.60f,  30.0f },
    { 1.30f,  38.0f }, { 1.10f,  45.0f }, { 0.95f,  55.0f }, { 0.85f,  65.0f },
    { 0.75f,  75.0f }, { 0.65f,  90.0f }, { 0.55f, 110.0f }, { 0.48f, 130.0f },
    { 0.42f, 150.0f }, { 0.35f, 180.0f }
};
#define IR_CURVE_N  (sizeof(IR_CURVE) / sizeof(IR_CURVE[0]))    /**< 曲线点数 */

static Adafruit_MCP23X17 s_mcp;         /**< MCP23017 驱动对象 */
static bool              s_mcpOK = false;   /**< MCP23017 在线标志 */
SensorData               gSen;          /**< 全局传感器数据     */

/** @brief 六路红外的车体坐标安装角（与 web_ui.h 的 IR_ANG 必须保持一致） */
const int16_t kIrAngle[IR_COUNT] = { 0, 180, 45, -45, 135, -135 };

/* ==========================================================================
 *                          私有函数（文件内静态）
 * ========================================================================== */

/**
 * @brief   切换 4067 通道
 * @param[in] ch  通道号 0~15
 * @return   无
 * @note     切换后等待 120µs 再采样：模拟开关导通电阻 + ADC 采样电容需要建立时间，
 *           不等待会导致相邻通道串扰（表现为红外读数乱跳）。
 */
static void muxSelect(uint8_t ch) {
    digitalWrite(MUX_S0, (ch & 0x01) ? HIGH : LOW);
    digitalWrite(MUX_S1, (ch & 0x02) ? HIGH : LOW);
    digitalWrite(MUX_S2, (ch & 0x04) ? HIGH : LOW);
    digitalWrite(MUX_S3, (ch & 0x08) ? HIGH : LOW);
    delayMicroseconds(120);
}

/**
 * @brief   电压 → 距离查表换算
 * @param[in] v  采样电压，单位 V
 * @return  距离，单位 cm；超量程返回 200，过近返回曲线最小值
 */
static float irVoltageToCm(float v) {
    if (v >= IR_CURVE[0][0]) return IR_CURVE[0][1];                 /* 过近   */
    if (v <= IR_CURVE[IR_CURVE_N - 1][0]) return 200.0f;            /* 超量程 */

    for (size_t i = 0; i < IR_CURVE_N - 1; i++) {
        if (v <= IR_CURVE[i][0] && v >= IR_CURVE[i + 1][0]) {
            float t = (IR_CURVE[i][0] - v) / (IR_CURVE[i][0] - IR_CURVE[i + 1][0]);
            return IR_CURVE[i][1] + t * (IR_CURVE[i + 1][1] - IR_CURVE[i][1]);
        }
    }
    return 200.0f;
}

/* ==========================================================================
 *                              对外函数实现
 * ========================================================================== */

/**
 * @brief   初始化传感器子系统
 * @param   无
 * @return  true MCP23017 在线
 */
bool sensorsInit() {
    /* ---- 4067 地址脚 ---- */
    pinMode(MUX_S0, OUTPUT); pinMode(MUX_S1, OUTPUT);
    pinMode(MUX_S2, OUTPUT); pinMode(MUX_S3, OUTPUT);
    pinMode(MUX_SIG, INPUT);

    /* ---- ADC：12bit + 11dB 衰减（满量程 ≈3.3V），仅用 ADC1 ---- */
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    analogSetPinAttenuation(MUX_SIG, ADC_11db);

    /* ---- MCP23017：GPA0~6 全部上拉输入 ---- */
    if (!s_mcp.begin_I2C(0x20, &Wire)) {
        Serial.println("[sen] MCP23017 未找到 → 灰度/E18 不可用");
        s_mcpOK = false;
    } else {
        s_mcpOK = true;
        for (uint8_t p = 0; p < 7; p++) s_mcp.pinMode(p, INPUT_PULLUP);
    }

    for (int i = 0; i < IR_COUNT; i++) { gSen.ir[i] = 200.0f; gSen.irValid[i] = false; }
    gSen.batValid = false;
    gSen.ioOk     = s_mcpOK;
    gSen.stamp    = 0;
    return s_mcpOK;
}

/**
 * @brief   读取 4067 某通道电压
 * @param[in] ch  通道号 0~15
 * @return  电压 V
 */
float irRawVoltage(uint8_t ch) {
    muxSelect(ch);
    (void)analogRead(MUX_SIG);          /* 丢弃第一次：消除通道切换残留电荷 */
    int raw = analogRead(MUX_SIG);
    return (float)raw * 3.3f / 4095.0f;
}

/**
 * @brief   扫描一轮传感器
 * @param   无
 * @return  无
 */
void sensorsUpdate() {
    CfgSnap cs;
    const Cfg& c = cs.c;

    /* ---- 1. 红外测距 ×6 ---- */
    float minD = 200.0f;
    bool  near = false;
    for (int i = 0; i < IR_COUNT; i++) {
        float v = irRawVoltage(i);

        /* ★ 逐通道有效性：断路 / 对 VCC 短路的通道一律判为无效，
           既不参与限速仲裁，也不在网页上伪装成真实读数。 */
        const bool valid = (v >= IR_V_MIN) && (v <= IR_V_MAX);
        gSen.irValid[i] = valid;

        if (!valid) {
            gSen.ir[i] = 200.0f;                    /* 无效 → 视为"无目标"  */
            continue;                               /* 关键：不参与 near 判定 */
        }

        float d = irVoltageToCm(v) * c.irScale;     /* 比例修正（可网页校准） */
        if (d < 0) d = 0;
        gSen.ir[i] = d;
        if (d < minD) minD = d;
        if (d < c.irAlarm) near = true;             /* 触发限速条件           */
    }
    gSen.irMin  = minD;
    gSen.irNear = near;

    /* ---- 2. 电池电压（CH6，经 11:1 分压） ---- */
    float vbat = irRawVoltage(6) * BAT_DIVIDER;
    /* 一阶滤波：平滑电机启停造成的电压尖刺，避免误触发低压保护 */
    gSen.bat = (gSen.bat == 0) ? vbat : (gSen.bat * 0.8f + vbat * 0.2f);
    gSen.batValid = (gSen.bat >= BAT_V_MIN) && (gSen.bat <= BAT_V_MAX);

    /* ---- 3. MCP23017：灰度 ×4 + E18 ×3 ---- */
    if (s_mcpOK) {
        uint8_t v = s_mcp.readGPIOA();              /* GPA0~6 一次性读入     */

        for (int i = 0; i < 4; i++) {
            bool raw = (v >> i) & 0x01;
            gSen.gray[i] = c.grayInvert ? !raw : raw;
        }

        bool drop = false;
        for (int i = 0; i < 3; i++) {
            bool raw  = (v >> (4 + i)) & 0x01;
            bool trig = c.e18ActiveLow ? !raw : raw;    /* NPN 常开：触发输出低 */
            gSen.e18[i] = trig;
            if (trig) drop = true;                      /* 判定为悬空/障碍     */
        }
        gSen.edgeDrop = drop;
    }

    /* ---- 4. 打时间戳：上层据此判断数据是否新鲜 ---- */
    gSen.stamp = millis();
}

/**
 * @brief   判断传感器数据是否新鲜
 * @param[in] now      当前时间戳 ms
 * @param[in] maxAgeMs 允许的最大数据年龄 ms
 * @return  true 数据在有效期内
 */
bool sensorsFresh(uint32_t now, uint32_t maxAgeMs) {
    if (gSen.stamp == 0) return false;              /* 从未成功扫描过        */
    return (now - gSen.stamp) <= maxAgeMs;          /* 无符号差天然抗回绕    */
}
