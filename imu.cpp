/**
 ******************************************************************************
 * @file    imu.cpp
 * @brief   姿态与偏航解算实现（MPU6050 + ADXL345）
 *
 * @details 三条数据流：
 *            1. 陀螺积分：ω_z 减去零偏后积分，高频特性好但有漂移；
 *            2. 里程计：由左右轮行程差推算，无漂移但打滑会跳变；
 *            3. 互补融合：按 α 加权合并增量，兼取两家之长。
 *
 *          静止零偏自更新：车辆静止满约 1s 时，以 0.3 的权重平滑修正零偏，
 *          用于抵消 MPU6050 的温漂（这是长时间作业精度的主要杀手）。
 *
 * @version V2.0.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>统一企业级注释规范
 *          <tr><td>2026-10-05 <td>V2.0  <td>电控组 <td>融合版：静止判据改用
 *                                                    方差检验 + 自适应偏置限幅
 *          </table>
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include "imu.h"
#include "cfg.h"
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_ADXL345_U.h>
#include <Preferences.h>

/* ==========================================================================
 *                          私有对象与全局数据
 * ========================================================================== */
static Adafruit_MPU6050        s_mpu;               /**< MPU6050 驱动对象     */
static Adafruit_ADXL345_Unified s_adxl(12345);      /**< ADXL345 驱动对象     */
ImuData gImu;                                       /**< 全局姿态数据         */

/* ==========================================================================
 *                          私有常量
 * ========================================================================== */

/** @brief 静止判定用环形缓冲容量（200Hz × 100 ≈ 0.5s 的观测窗口）       */
#define QUIET_N           100

/**
 * @brief 静止判据的方差上限 (rad/s)²
 * @details 取 1.0e-4 → 标准差 0.01 rad/s ≈ 0.57°/s。
 *          车体真静止时 MPU6050 的噪声远低于此；一旦被推着走、被撞击、
 *          或者车轮在打滑抖，方差会立刻超限，从而避免"把运动当成零偏"。
 */
#define QUIET_VAR_MAX     1.0e-4f

/**
 * @brief 自适应偏置相对「标定值」的最大偏移 (rad/s)
 * @details ±0.001745 rad/s = ±0.1°/s。
 *          在线自更新只允许在这个窄带内微调以跟踪温漂；
 *          超出说明是别的因素（例如车体其实在缓慢移动），绝不写进零偏。
 */
#define ADAPTIVE_MAX      0.001745f

/* ==========================================================================
 *                          私有变量（文件内静态）
 * ========================================================================== */

/** @brief 静止观测环形缓冲（rad/s）                                      */
static float    s_quiet[QUIET_N];
static uint16_t s_qIdx = 0;         /**< 环形缓冲写指针                   */
static uint16_t s_qCnt = 0;         /**< 已填充的样本数                   */

/**
 * @brief 自适应偏置增量（rad/s），只在内存里，不写 NVS
 * @details 与 NVS 中的标定值分离，好处是：一次成功的显式标定可以立刻把
 *          长期漂移的自适应项清零，而不用担心旧漂移被"记住"。
 */
static float    s_adaptive = 0.0f;

/* ==========================================================================
 *                              对外函数实现
 * ========================================================================== */

/**
 * @brief   初始化 I²C 与两块 IMU
 * @param   无
 * @return  true 至少一块 IMU 在线
 */
bool imuInit() {
    Wire.begin(9, 10);                  /* I²C：SDA=GPIO9，SCL=GPIO10        */
    Wire.setClock(400000);              /* 400kHz 快速模式                   */

    /* ---- MPU6050：偏航角来源 ---- */
    gImu.mpuOK = s_mpu.begin(0x68, &Wire);
    if (gImu.mpuOK) {
        s_mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
        s_mpu.setGyroRange(MPU6050_RANGE_500_DEG);
        s_mpu.setFilterBandwidth(MPU6050_BAND_94_HZ);
    } else {
        Serial.println("[imu] MPU6050 未找到 → 偏航角退化为纯里程计");
    }

    /* ---- ADXL345：倾角 / 翻车 / 冲击 ---- */
    gImu.adxlOK = s_adxl.begin();
    if (gImu.adxlOK) {
        s_adxl.setRange(ADXL345_RANGE_4_G);
    } else {
        Serial.println("[imu] ADXL345 未找到 → 倾角/冲击检测不可用");
    }

    CfgSnap cs;
    gImu.bias = cs.c.gzBias;            /* 载入上次标定得到的零偏             */
    return gImu.mpuOK || gImu.adxlOK;
}

/**
 * @brief   周期更新姿态
 * @param[in] dt          控制周期 s
 * @param[in] dYawOdo     里程计偏航增量 °
 * @param[in] stationary  是否静止
 * @return   无
 */
void imuUpdate(float dt, float dYawOdo, bool stationary) {
    CfgSnap cs;
    const Cfg& c = cs.c;

    /* ★ 融合版：有效零偏 = 标定值（NVS） + 自适应增量（内存，限幅） */
    gImu.bias = c.gzBias + s_adaptive;

    /* ---- 1. IMU 分支：ω_z 去零偏后积分 ---- */
    float dYawImu = 0;
    if (gImu.mpuOK) {
        sensors_event_t a, g, t;
        s_mpu.getEvent(&a, &g, &t);

        float gz = g.gyro.z;                        /* rad/s                  */
        if (c.gyroInvert) gz = -gz;                 /* 装反了可在网页勾选取反 */
        gImu.gz = gz;

        float w = gz - gImu.bias;                   /* 去零偏                 */
        if (fabsf(w) < 0.0035f) w = 0;              /* 死区 ≈0.2°/s，抑制噪声积分 */
        dYawImu   = w * dt * 180.0f / 3.14159265f;
        gImu.yawRate = w * 180.0f / 3.14159265f;

        /* ---- 静止时的零偏自更新（★ 融合版：方差判据 + 自适应限幅） ----
           旧实现只要"看起来静止"就累加，问题是：被对手顶住、轮子原地打滑、
           整车以极低速度平移时，FG 与 PWM 都判不出运动，avg 会缓慢偏离真值，
           于是把「运动」当成了「零偏」，标定被悄悄污染。
           这里改为：先攒满一个观测窗口，用**方差**确认真的没动，
           再只允许在 ±0.1°/s 的窄带内微调，且绝不回写 NVS。 */
        if (stationary) {
            s_quiet[s_qIdx] = gz;
            s_qIdx = (s_qIdx + 1) % QUIET_N;
            if (s_qCnt < QUIET_N) s_qCnt++;

            if (s_qCnt >= QUIET_N) {
                double sum = 0, sumSq = 0;
                for (uint16_t i = 0; i < QUIET_N; i++) {
                    sum   += s_quiet[i];
                    sumSq += (double)s_quiet[i] * (double)s_quiet[i];
                }
                const double mean = sum / QUIET_N;
                const double var  = sumSq / QUIET_N - mean * mean;

                if (var >= 0.0 && var < QUIET_VAR_MAX) {
                    /* 观测可信：以标定值为基准求增量，限幅后再一阶平滑 */
                    float delta = (float)mean - c.gzBias;
                    delta = constrain(delta, -ADAPTIVE_MAX, ADAPTIVE_MAX);
                    s_adaptive = s_adaptive * 0.7f + delta * 0.3f;
                }
                s_qCnt = 0;                 /* 清空窗口，重新攒下一轮观测  */
                s_qIdx = 0;
            }
        } else {
            s_qCnt = 0;                     /* 一动就丢掉半截窗口，避免混入 */
            s_qIdx = 0;
        }
    }

    /* ---- 2. 互补融合（增量式，等价于 α 加权但不会两路发散） ---- */
    if (gImu.mpuOK) {
        gImu.yaw += c.alpha * dYawImu + (1.0f - c.alpha) * dYawOdo;
    } else {
        gImu.yaw     += dYawOdo;                    /* 无 IMU：纯里程计       */
        gImu.yawRate  = dYawOdo / fmaxf(dt, 1e-4f);
    }

    /* 归一化到 (−180°, +180°]，避免长时间累积后数值溢出 */
    if (gImu.yaw >  180.0f) gImu.yaw -= 360.0f;
    if (gImu.yaw < -180.0f) gImu.yaw += 360.0f;

    /* ---- 3. 加速度计：倾角 / 翻车 / 冲击 ---- */
    if (gImu.adxlOK) {
        sensors_event_t e;
        s_adxl.getEvent(&e);

        gImu.ax = e.acceleration.x / 9.80665f;      /* m/s² → g              */
        gImu.ay = e.acceleration.y / 9.80665f;
        gImu.az = e.acceleration.z / 9.80665f;

        gImu.pitch = atan2f(gImu.ax, sqrtf(gImu.ay * gImu.ay + gImu.az * gImu.az))
                     * 180.0f / 3.14159265f;
        gImu.roll  = atan2f(gImu.ay, sqrtf(gImu.ax * gImu.ax + gImu.az * gImu.az))
                     * 180.0f / 3.14159265f;

        float mag = sqrtf(gImu.ax * gImu.ax + gImu.ay * gImu.ay + gImu.az * gImu.az);
        gImu.impact = (mag > c.impactThresh);       /* 被撞击判定             */
    }
}

/**
 * @brief   偏航角归零
 * @param   无
 * @return  无
 */
void imuResetYaw() { gImu.yaw = 0; }

/**
 * @brief   直接设定偏航角
 * @param[in] v  偏航角 °
 * @return   无
 */
void imuSetYaw(float v) { gImu.yaw = v; }

/**
 * @brief   复位自适应偏置
 * @param   无
 * @return   无
 */
void imuResetAdaptive() {
    s_adaptive = 0.0f;
    s_qCnt     = 0;
    s_qIdx     = 0;
}

/**
 * @brief   零偏标定（阻塞式采样 + 方差检验）
 * @param[in] sampleMs  采样时长 ms
 * @return  标定得到的零偏 rad/s
 *
 * @details ★ 融合版新增方差检验：
 *          只求均值是不够的 —— 如果采样期间被撞了一下、或者有人在推车，
 *          均值会被污染，但单看均值完全看不出来。
 *          这里同时累加平方和，算出方差；方差超限说明「采样期间并不静止」，
 *          此时**拒绝写入**并返回当前值，避免把一次坏标定固化进 NVS。
 *
 * @warning 本函数含 delay 循环，仅允许在「标定」这种非实时场景下调用。
 */
float imuCalibrateBias(uint32_t sampleMs) {
    if (!gImu.mpuOK) return 0;

    uint32_t t0 = millis();
    double   sum = 0, sumSq = 0;
    uint32_t n   = 0;

    while (millis() - t0 < sampleMs) {
        sensors_event_t a, g, t;
        s_mpu.getEvent(&a, &g, &t);
        const double gz = g.gyro.z;
        sum   += gz;
        sumSq += gz * gz;
        n++;
        delay(2);
    }

    if (n < 10) return gImu.bias;                   /* 样本太少，不信任     */

    const double mean = sum / n;
    const double var  = sumSq / n - mean * mean;

    /* ---- 方差检验：采样期间不静止则拒绝本次标定 ---- */
    if (!(var >= 0.0) || var >= QUIET_VAR_MAX) {
        Serial.printf("[imu] 零偏标定失败：方差 %.3e 超限（车辆可能未静止）\n", var);
        return gImu.bias;                            /* 保持原值不变        */
    }

    const float b = (float)mean;

    if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);
    gCfg.gzBias = b;
    if (gCfgMutex) xSemaphoreGive(gCfgMutex);

    imuResetAdaptive();                              /* 新基准，清掉旧漂移  */
    gImu.bias = b;
    return b;
}
