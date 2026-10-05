/**
 ******************************************************************************
 * @file    cfg.h
 * @brief   全局配置结构体定义与 NVS 持久化接口
 *
 * @details 本模块是整个系统的「唯一参数源」：
 *            - 所有可调参数（网络、机械、PID、传感器、安全、标定）集中于 @ref Cfg；
 *            - 参数以「整包 JSON 字符串」写入 NVS，避免逐项 put/get 带来的版本漂移；
 *            - 网页端通过 /api/config 读写同一份 JSON，实现「所见即所存」。
 *
 *          并发模型：配置由网络核（Core 1）写入、控制核（Core 0）读取，
 *          因此所有访问都必须经过 @ref gCfgMutex，或使用局部快照类 @ref CfgSnap。
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
 * @note    缺省值来源：docs/design/模型核实与实测参数.md 第 6 节（D/L/T 均已实测锁定）。
 * @warning 禁止长时间持锁；跨核读取请统一使用 CfgSnap 取快照。
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

#ifndef __CFG_H
#define __CFG_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>

/**
 * @brief  系统全局配置集合
 * @note   字段按「功能分组」排列，与网页设置页的分组卡片一一对应。
 *         新增字段时必须同步修改 cfg.cpp 中的 setDefaults / cfgToJson / cfgFromJson 三处。
 */
struct Cfg {
    /* ---- 1. 网络 -------------------------------------------------------- */
    char   apSsid[33];      /**< AP 热点名称                                  */
    char   apPass[64];      /**< AP 密码（WPA2 要求 ≥8 位，不足则开放热点）   */
    char   staSsid[33];     /**< 待连接的路由器 SSID                          */
    char   staPass[64];     /**< 待连接的路由器密码                           */
    bool   staEnable;       /**< 是否启用 STA 联网（关闭则纯 AP 直连）        */
    char   mdns[33];        /**< mDNS 主机名，访问 http://<mdns>.local        */
    bool   captive;         /**< 强制门户开关（手机连 AP 自动弹控制页）       */

    /* ---- 2. 机械（模型 + 实测锁定） ------------------------------------- */
    float  wheelDia;        /**< D 轮径，单位 mm（已核实 72.0）                */
    float  wheelBase;       /**< L 轴距，单位 mm（实测 154）                   */
    float  track;           /**< T 轮距，单位 mm（实测 170）                   */
    float  ppr;             /**< FG 每轮转脉冲数（参考值 90，实际以标定为准）  */
    float  kOdo;            /**< 标定结果 K_odo：脉冲数 / 厘米                 */
    float  kTurn;           /**< 标定结果 K_turn：转向打滑修正系数             */

    /* ---- 3. 运动 -------------------------------------------------------- */
    float  maxSpeed;        /**< 最大线速度上限，单位 cm/s                     */
    float  defSpeed;        /**< 精准模式默认速度，单位 cm/s                   */
    float  accel;           /**< 直线加速度上限，单位 cm/s²                    */
    float  decel;           /**< 直线减速度上限，单位 cm/s²                    */
    float  maxOmega;        /**< 转向最大角速度，单位 °/s                      */
    float  turnAccel;       /**< 转向角加速度，单位 °/s²                       */
    float  turnDecel;       /**< 转向角减速度，单位 °/s²                       */

    /* ---- 4. 三环 PID ---------------------------------------------------- */
    float  vKp, vKi, vKd;   /**< 速度环参数（输出为归一化占空比 -1.0 ~ +1.0）  */
    float  pKp, pKi, pKd;   /**< 位置环参数（输出为目标线速度 cm/s）           */
    float  hKp, hKi, hKd;   /**< 航向环参数（输出为左右轮速差 cm/s）           */
    float  alpha;           /**< 互补滤波融合系数（IMU 权重，打滑大可上调）    */

    /* ---- 5. 电机 -------------------------------------------------------- */
    bool   invert[4];       /**< 单轮极性反转（接线反了不用改线，勾这里）      */
    float  pwmDead;         /**< PWM 死区（克服静摩擦的最小占空比 0~1023）     */
    float  pwmMax;          /**< PWM 输出上限 0~1023                           */
    float  balLR;           /**< 左右轮速差补偿（正值 = 右侧加力）             */
    float  scaleL, scaleR;  /**< 左 / 右侧整体出力缩放                         */

    /* ---- 6. 传感器 ------------------------------------------------------ */
    bool   grayInvert;      /**< 灰度逻辑取反                                  */
    float  irAlarm;         /**< 红外测距报警距离，单位 cm                     */
    bool   e18ActiveLow;    /**< E18 触发电平（NPN 常开 → 触发时输出低）       */
    bool   safetyEdge;      /**< 边缘保护开关（E18 判定悬空即刹车）            */
    bool   safetyIR;        /**< 红外接近限速开关                              */
    float  irScale;         /**< 红外测距比例修正（1.0 = 不修正）              */

    /* ---- 7. 舵机（铲子武器） -------------------------------------------- */
    int    servoCenter;     /**< 中位脉宽，单位 µs                             */
    int    servoMin;        /**< 最小脉宽（机械下限），单位 µs                 */
    int    servoMax;        /**< 最大脉宽（机械上限），单位 µs                 */
    int    servoUp;         /**< 举铲脉宽，单位 µs                             */
    int    servoDown;       /**< 放铲脉宽，单位 µs                             */

    /* ---- 8. 安全 -------------------------------------------------------- */
    uint32_t hbTimeout;     /**< 心跳超时时间，单位 ms                         */
    float  batLow;          /**< 低压告警阈值，单位 V                          */
    float  batCrit;         /**< 严重低压停机阈值，单位 V                      */
    uint32_t stallTime;     /**< 堵转判定持续时间，单位 ms                     */
    bool   lostBrake;       /**< 失联动作：true=刹车，false=惰行               */

    /* ---- 9. 标定辅助 ---------------------------------------------------- */
    float  calDist;         /**< 行程标定命令距离，单位 cm（默认 100）         */
    float  calTurnDeg;      /**< 转向标定命令角度，单位 ° （默认 360）         */

    /* ---- 10. IMU -------------------------------------------------------- */
    float  gzBias;          /**< MPU6050 Z 轴角速度零偏，单位 rad/s            */
    bool   gyroInvert;      /**< 陀螺仪 Z 轴方向取反（装反了勾这里）           */
    float  impactThresh;    /**< 撞击检测阈值，单位 g                          */

    /* ---- 11. 系统 ------------------------------------------------------- */
    uint8_t telemetryHz;    /**< 遥测推送频率，单位 Hz                         */
};

/* ==========================================================================
 *                              全局对象
 * ========================================================================== */
extern Cfg               gCfg;       /**< 全局唯一配置实例                     */
extern SemaphoreHandle_t gCfgMutex;  /**< 配置跨核互斥量                       */

/* ==========================================================================
 *                              对外接口
 * ========================================================================== */

/**
 * @brief   加载配置：从 NVS 读回，失败则写回出厂默认值
 * @param   无
 * @return  无
 * @note    必须在所有业务模块初始化之前调用（setup() 的第一步）。
 */
void cfgInit();

/**
 * @brief   将当前配置写入 NVS（掉电不丢）
 * @param   无
 * @return  无
 * @note    以整体 JSON 字符串存储，约 1.5KB，远小于 NVS 单条目上限。
 */
void cfgSave();

/**
 * @brief   恢复出厂默认值并立即写入 NVS
 * @param   无
 * @return  无
 */
void cfgFactoryReset();

/**
 * @brief   将当前配置序列化为 JSON 字符串
 * @param   无
 * @return  JSON 字符串（可直接作为 /api/config 响应或导出文件）
 */
String cfgToJson();

/**
 * @brief   从 JSON 字符串合并配置（部分更新）
 * @param[in] json  JSON 字符串；仅覆盖其中出现的键，未出现的键保持原值
 * @return  true 解析成功；false JSON 非法（原配置不受影响）
 */
bool cfgFromJson(const String& json);

/**
 * @brief   配置快照（RAII 风格）
 * @details 控制核每周期只需「读」配置，用本类可以避免长时间占用互斥量：
 *          @code
 *              CfgSnap snap;                  // 构造：加锁 → 拷贝 → 解锁
 *              float v = snap.c.maxSpeed;     // 之后只访问副本
 *          @endcode
 */
class CfgSnap {
public:
    /** @brief 构造：加互斥量 → 拷贝配置 → 立即释放互斥量 */
    CfgSnap() {
        if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);
        c = gCfg;
        if (gCfgMutex) xSemaphoreGive(gCfgMutex);
    }

    Cfg c;  /**< 配置副本（构造完成后即为只读快照） */
};

#endif /* __CFG_H */
