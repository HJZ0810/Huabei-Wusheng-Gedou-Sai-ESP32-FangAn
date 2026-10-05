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
    gImu.bias = c.gzBias;

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

        /* ---- 静止时缓慢更新零偏（应对温漂） ---- */
        if (stationary) {
            static float   biasAcc = 0;             /* 累计观测值             */
            static uint16_t n      = 0;             /* 观测计数               */
            biasAcc += gz; n++;
            if (n >= 200) {                         /* 200Hz × 1s             */
                float nb = biasAcc / n;
                if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);
                gCfg.gzBias = gCfg.gzBias * 0.7f + nb * 0.3f;   /* 一阶平滑   */
                if (gCfgMutex) xSemaphoreGive(gCfgMutex);
                biasAcc = 0; n = 0;
            }
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
 * @brief   零偏标定（阻塞式采样）
 * @param[in] sampleMs  采样时长 ms
 * @return  标定得到的零偏 rad/s
 * @warning 本函数含 delay 循环，仅允许在「标定」这种非实时场景下调用。
 */
float imuCalibrateBias(uint32_t sampleMs) {
    if (!gImu.mpuOK) return 0;

    uint32_t t0 = millis();
    double   sum = 0;
    uint32_t n   = 0;

    while (millis() - t0 < sampleMs) {
        sensors_event_t a, g, t;
        s_mpu.getEvent(&a, &g, &t);
        sum += g.gyro.z;
        n++;
        delay(2);
    }

    float b = n ? (float)(sum / n) : 0.0f;

    if (gCfgMutex) xSemaphoreTake(gCfgMutex, portMAX_DELAY);
    gCfg.gzBias = b;
    if (gCfgMutex) xSemaphoreGive(gCfgMutex);

    gImu.bias = b;
    return b;
}
