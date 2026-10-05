/**
 ******************************************************************************
 * @file    cfg.cpp
 * @brief   全局配置实现：出厂默认值、NVS 读写、JSON 序列化
 *
 * @details 设计要点：
 *            - **单条 NVS 存储**：整包 JSON 写入 "cbot" 命名空间的 "cfg" 键，
 *              新增字段无需迁移旧数据（缺失键自动回落默认值）。
 *            - **部分更新语义**：cfgFromJson 只覆盖 JSON 中出现的键，
 *              使网页可以只提交某个分组而不影响其它分组。
 *            - **默认值可追溯**：参数依据见 docs/design/模型核实与实测参数.md。
 *
 * @author  CombatBot 电控组
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期2026-10-05       <th>版本  <th>作者：HJZ   <th>说明
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
#include "cfg.h"
#include <Preferences.h>
#include <ArduinoJson.h>

/* ==========================================================================
 *                              全局对象定义
 * ========================================================================== */
Cfg               gCfg;                    /**< 全局唯一配置实例 */
SemaphoreHandle_t gCfgMutex = NULL;        /**< 配置跨核互斥量   */

/* ==========================================================================
 *                          私有函数（文件内静态）
 * ========================================================================== */

/**
 * @brief   填充出厂默认值
 * @param[out] c  待填充的配置结构体
 * @return   无
 * @note     所有机械参数（D=72.0 / L=154 / T=170）均已由 STEP 模型与实车测量锁定，
 *           详见 docs/design/模型核实与实测参数.md；运动学相关初值由它们推导。
 */
static void setDefaults(Cfg& c) {
    memset(&c, 0, sizeof(Cfg));

    /* ---- 1. 网络：AP 常驻，STA 默认关闭（纯 AP 直连即可完整使用） ---- */
    strcpy(c.apSsid, "CombatBot-AP");
    strcpy(c.apPass, "12345678");
    strcpy(c.staSsid, "");
    strcpy(c.staPass, "");
    c.staEnable = false;
    strcpy(c.mdns, "combatbot");
    c.captive = true;

    /* ---- 2. 机械 ---- */
    c.wheelDia  = 72.0f;    /* STEP 模型：4 处圆环面外径 = 72mm                */
    c.wheelBase = 154.0f;   /* 实车测量                                        */
    c.track     = 170.0f;   /* 实车测量                                        */
    c.ppr       = 90.0f;    /* 参考值：9 脉冲/电机转 × 减速比 10               */
    /* 未标定时的初值：脉冲/厘米 = PPR ÷ 周长(cm)，C = π·D/10 = 22.619cm */
    c.kOdo  = c.ppr / (3.14159265f * c.wheelDia / 10.0f);   /* ≈ 3.978 */
    c.kTurn = 1.0f;         /* 转向标定前按「无打滑」假设                      */

    /* ---- 3. 运动：出厂保守值，防止首次上电就冲出去 ---- */
    c.maxSpeed  = 100.0f;
    c.defSpeed  = 40.0f;
    c.accel     = 80.0f;
    c.decel     = 100.0f;
    c.maxOmega  = 180.0f;
    c.turnAccel = 200.0f;
    c.turnDecel = 300.0f;

    /* ---- 4. 三环 PID（经验初值，装机后需按实际响应微调） ---- */
    c.vKp = 0.030f; c.vKi = 0.35f; c.vKd = 0.000f;   /* 速度环：PI 为主 */
    c.pKp = 0.35f;  c.pKi = 0.02f; c.pKd = 0.000f;   /* 位置环：P 为主 */
    c.hKp = 1.20f;  c.hKi = 0.00f; c.hKd = 0.150f;   /* 航向环：PD     */
    c.alpha = 0.85f;                                  /* IMU 权重 0.85  */

    /* ---- 5. 电机 ---- */
    for (int i = 0; i < 4; i++) c.invert[i] = false;
    c.pwmDead = 70.0f;      /* 约 6.8%，驼峰轮静摩擦大，装机后实测上调        */
    c.pwmMax  = 1023.0f;
    c.balLR   = 0.0f;
    c.scaleL  = 1.0f;
    c.scaleR  = 1.0f;

    /* ---- 6. 传感器 ---- */
    c.grayInvert   = false;
    c.irAlarm      = 30.0f;
    c.e18ActiveLow = true;
    c.safetyEdge   = true;
    c.safetyIR     = true;
    c.irScale      = 1.0f;

    /* ---- 7. 舵机：脉宽需按实车机械限位微调 ---- */
    c.servoCenter = 1500;
    c.servoMin    = 600;
    c.servoMax    = 2400;
    c.servoUp     = 2200;
    c.servoDown   = 900;

    /* ---- 8. 安全：6S 锂电（24V 标称，满电 25.2 / 亏电 21） ---- */
    c.hbTimeout = 1000;
    c.batLow    = 21.5f;
    c.batCrit   = 20.5f;
    c.stallTime = 1500;
    c.lostBrake = true;

    /* ---- 9. 标定辅助 ---- */
    c.calDist    = 100.0f;
    c.calTurnDeg = 360.0f;

    /* ---- 10. IMU ---- */
    c.gzBias       = 0.0f;
    c.gyroInvert   = false;
    c.impactThresh = 2.0f;

    /* ---- 11. 系统 ---- */
    c.telemetryHz = 15;
}

/* ==========================================================================
 *                              对外函数实现
 * ========================================================================== */

/**
 * @brief   初始化配置子系统：创建互斥量 → 填默认值 → 尝试从 NVS 覆盖
 * @param   无
 * @return  无
 * @note    若 NVS 中无数据（首次上电），会立刻把默认值写回 NVS，
 *          保证「读到的」与「存着的」始终一致。
 */
void cfgInit() {
    gCfgMutex = xSemaphoreCreateMutex();
    setDefaults(gCfg);

    Preferences p;
    if (p.begin("cbot", true)) {                    /* 只读打开命名空间 */
        String s = p.getString("cfg", "");
        p.end();
        if (s.length() > 8) cfgFromJson(s);         /* 有数据则合并覆盖 */
    } else {
        cfgSave();                                  /* 首次上电：写默认值 */
    }
}

/**
 * @brief   将当前配置写入 NVS
 * @param   无
 * @return  无
 */
void cfgSave() {
    String s = cfgToJson();

    if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);

    Preferences p;
    bool ok = p.begin("cbot", false);               /* 读写打开 */
    if (ok) { p.putString("cfg", s); p.end(); }

    if (gCfgMutex) xSemaphoreGive(gCfgMutex);

    Serial.printf("[cfg] save %s (%u bytes)\n", ok ? "OK" : "FAIL", (unsigned)s.length());
}

/**
 * @brief   恢复出厂默认值
 * @param   无
 * @return  无
 */
void cfgFactoryReset() {
    if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);
    setDefaults(gCfg);
    if (gCfgMutex) xSemaphoreGive(gCfgMutex);

    cfgSave();
}

/**
 * @brief   序列化当前配置为 JSON
 * @param   无
 * @return  JSON 字符串
 */
String cfgToJson() {
    JsonDocument doc;

    /* ---- 网络 ---- */
    doc["apSsid"] = gCfg.apSsid;   doc["apPass"] = gCfg.apPass;
    doc["staSsid"] = gCfg.staSsid; doc["staPass"] = gCfg.staPass;
    doc["staEnable"] = gCfg.staEnable;
    doc["mdns"] = gCfg.mdns;       doc["captive"] = gCfg.captive;

    /* ---- 机械 ---- */
    doc["wheelDia"]  = gCfg.wheelDia;
    doc["wheelBase"] = gCfg.wheelBase;
    doc["track"]     = gCfg.track;
    doc["ppr"]       = gCfg.ppr;
    doc["kOdo"]      = gCfg.kOdo;
    doc["kTurn"]     = gCfg.kTurn;

    /* ---- 运动 ---- */
    doc["maxSpeed"] = gCfg.maxSpeed; doc["defSpeed"] = gCfg.defSpeed;
    doc["accel"] = gCfg.accel;       doc["decel"] = gCfg.decel;
    doc["maxOmega"] = gCfg.maxOmega; doc["turnAccel"] = gCfg.turnAccel;
    doc["turnDecel"] = gCfg.turnDecel;

    /* ---- PID ---- */
    doc["vKp"] = gCfg.vKp; doc["vKi"] = gCfg.vKi; doc["vKd"] = gCfg.vKd;
    doc["pKp"] = gCfg.pKp; doc["pKi"] = gCfg.pKi; doc["pKd"] = gCfg.pKd;
    doc["hKp"] = gCfg.hKp; doc["hKi"] = gCfg.hKi; doc["hKd"] = gCfg.hKd;
    doc["alpha"] = gCfg.alpha;

    /* ---- 电机 ---- */
    JsonArray inv = doc["invert"].to<JsonArray>();
    for (int i = 0; i < 4; i++) inv.add(gCfg.invert[i]);
    doc["pwmDead"] = gCfg.pwmDead; doc["pwmMax"] = gCfg.pwmMax;
    doc["balLR"] = gCfg.balLR;     doc["scaleL"] = gCfg.scaleL;
    doc["scaleR"] = gCfg.scaleR;

    /* ---- 传感器 ---- */
    doc["grayInvert"] = gCfg.grayInvert; doc["irAlarm"] = gCfg.irAlarm;
    doc["e18ActiveLow"] = gCfg.e18ActiveLow;
    doc["safetyEdge"] = gCfg.safetyEdge; doc["safetyIR"] = gCfg.safetyIR;
    doc["irScale"] = gCfg.irScale;

    /* ---- 舵机 ---- */
    doc["servoCenter"] = gCfg.servoCenter;
    doc["servoMin"] = gCfg.servoMin;  doc["servoMax"] = gCfg.servoMax;
    doc["servoUp"] = gCfg.servoUp;    doc["servoDown"] = gCfg.servoDown;

    /* ---- 安全 ---- */
    doc["hbTimeout"] = gCfg.hbTimeout;
    doc["batLow"] = gCfg.batLow;     doc["batCrit"] = gCfg.batCrit;
    doc["stallTime"] = gCfg.stallTime; doc["lostBrake"] = gCfg.lostBrake;

    /* ---- 标定 ---- */
    doc["calDist"] = gCfg.calDist;   doc["calTurnDeg"] = gCfg.calTurnDeg;

    /* ---- IMU ---- */
    doc["gzBias"] = gCfg.gzBias;     doc["gyroInvert"] = gCfg.gyroInvert;
    doc["impactThresh"] = gCfg.impactThresh;

    /* ---- 系统 ---- */
    doc["telemetryHz"] = gCfg.telemetryHz;

    String out;
    serializeJson(doc, out);
    return out;
}

/**
 * @brief   从 JSON 字符串合并配置
 * @param[in] json  JSON 字符串
 * @return  true 解析成功；false JSON 非法
 * @note    采用 ArduinoJson 的 `|` 默认值运算符：键缺失时保持原值，
 *          因此网页可以只提交单个分组。
 */
bool cfgFromJson(const String& json) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, json);
    if (e) {
        Serial.printf("[cfg] json err: %s\n", e.c_str());
        return false;
    }

    if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);

    strlcpy(gCfg.apSsid,  doc["apSsid"]  | gCfg.apSsid,  sizeof(gCfg.apSsid));
    strlcpy(gCfg.apPass,  doc["apPass"]  | gCfg.apPass,  sizeof(gCfg.apPass));
    strlcpy(gCfg.staSsid, doc["staSsid"] | gCfg.staSsid, sizeof(gCfg.staSsid));
    strlcpy(gCfg.staPass, doc["staPass"] | gCfg.staPass, sizeof(gCfg.staPass));
    gCfg.staEnable = doc["staEnable"] | gCfg.staEnable;
    strlcpy(gCfg.mdns,    doc["mdns"]    | gCfg.mdns,    sizeof(gCfg.mdns));
    gCfg.captive = doc["captive"] | gCfg.captive;

    gCfg.wheelDia  = doc["wheelDia"]  | gCfg.wheelDia;
    gCfg.wheelBase = doc["wheelBase"] | gCfg.wheelBase;
    gCfg.track     = doc["track"]     | gCfg.track;
    gCfg.ppr       = doc["ppr"]       | gCfg.ppr;
    gCfg.kOdo      = doc["kOdo"]      | gCfg.kOdo;
    gCfg.kTurn     = doc["kTurn"]     | gCfg.kTurn;

    gCfg.maxSpeed  = doc["maxSpeed"]  | gCfg.maxSpeed;
    gCfg.defSpeed  = doc["defSpeed"]  | gCfg.defSpeed;
    gCfg.accel     = doc["accel"]     | gCfg.accel;
    gCfg.decel     = doc["decel"]     | gCfg.decel;
    gCfg.maxOmega  = doc["maxOmega"]  | gCfg.maxOmega;
    gCfg.turnAccel = doc["turnAccel"] | gCfg.turnAccel;
    gCfg.turnDecel = doc["turnDecel"] | gCfg.turnDecel;

    gCfg.vKp = doc["vKp"] | gCfg.vKp; gCfg.vKi = doc["vKi"] | gCfg.vKi; gCfg.vKd = doc["vKd"] | gCfg.vKd;
    gCfg.pKp = doc["pKp"] | gCfg.pKp; gCfg.pKi = doc["pKi"] | gCfg.pKi; gCfg.pKd = doc["pKd"] | gCfg.pKd;
    gCfg.hKp = doc["hKp"] | gCfg.hKp; gCfg.hKi = doc["hKi"] | gCfg.hKi; gCfg.hKd = doc["hKd"] | gCfg.hKd;
    gCfg.alpha = doc["alpha"] | gCfg.alpha;

    if (doc["invert"].is<JsonArray>())
        for (int i = 0; i < 4; i++) gCfg.invert[i] = doc["invert"][i] | gCfg.invert[i];
    gCfg.pwmDead = doc["pwmDead"] | gCfg.pwmDead;
    gCfg.pwmMax  = doc["pwmMax"]  | gCfg.pwmMax;
    gCfg.balLR   = doc["balLR"]   | gCfg.balLR;
    gCfg.scaleL  = doc["scaleL"]  | gCfg.scaleL;
    gCfg.scaleR  = doc["scaleR"]  | gCfg.scaleR;

    gCfg.grayInvert   = doc["grayInvert"]   | gCfg.grayInvert;
    gCfg.irAlarm      = doc["irAlarm"]      | gCfg.irAlarm;
    gCfg.e18ActiveLow = doc["e18ActiveLow"] | gCfg.e18ActiveLow;
    gCfg.safetyEdge   = doc["safetyEdge"]   | gCfg.safetyEdge;
    gCfg.safetyIR     = doc["safetyIR"]     | gCfg.safetyIR;
    gCfg.irScale      = doc["irScale"]      | gCfg.irScale;

    gCfg.servoCenter = doc["servoCenter"] | gCfg.servoCenter;
    gCfg.servoMin    = doc["servoMin"]    | gCfg.servoMin;
    gCfg.servoMax    = doc["servoMax"]    | gCfg.servoMax;
    gCfg.servoUp     = doc["servoUp"]     | gCfg.servoUp;
    gCfg.servoDown   = doc["servoDown"]   | gCfg.servoDown;

    gCfg.hbTimeout = doc["hbTimeout"] | gCfg.hbTimeout;
    gCfg.batLow    = doc["batLow"]    | gCfg.batLow;
    gCfg.batCrit   = doc["batCrit"]   | gCfg.batCrit;
    gCfg.stallTime = doc["stallTime"] | gCfg.stallTime;
    gCfg.lostBrake = doc["lostBrake"] | gCfg.lostBrake;

    gCfg.calDist    = doc["calDist"]    | gCfg.calDist;
    gCfg.calTurnDeg = doc["calTurnDeg"] | gCfg.calTurnDeg;

    gCfg.gzBias       = doc["gzBias"]       | gCfg.gzBias;
    gCfg.gyroInvert   = doc["gyroInvert"]   | gCfg.gyroInvert;
    gCfg.impactThresh = doc["impactThresh"] | gCfg.impactThresh;

    gCfg.telemetryHz = doc["telemetryHz"] | gCfg.telemetryHz;

    if (gCfgMutex) xSemaphoreGive(gCfgMutex);
    return true;
}
