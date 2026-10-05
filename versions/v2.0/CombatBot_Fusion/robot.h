/**
 ******************************************************************************
 * @file    robot.h
 * @brief   机器人主控制器接口（模式状态机 + 三级闭环 + 遥测）
 *
 * @details 运行模型：
 *            - 本模块创建一个独立的 FreeRTOS 任务（Core 0，固定 200Hz），
 *              负责传感器采集、里程结算、闭环运算、安全仲裁、遥测快照；
 *            - 网络核（Core 1）通过 @ref robotPushCmd 投递指令，
 *              通过 @ref robotCopyTelemetry / @ref robotPopDone 取回数据；
 *            - 三个通道全部线程安全（队列 / 互斥量），因此网络抖动不会影响控制周期。
 *
 *          支持的模式（@ref RMode）：
 *            - R_IDLE    空闲（PWM=0）
 *            - R_MANUAL  连续实时（按住即走，松开即停）
 *            - R_MOVE    精准直线（梯形规划 + 航向保持）
 *            - R_TURN    精准转向（脉冲主转 + IMU 精修）
 *            - R_CAL_IMU IMU 零偏采样
 *            - R_ESTOP   急停锁定
 *
 * @author  HJZ
 * @version V2.0.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>统一企业级注释规范
 *          <tr><td>2026-10-05 <td>V2.0  <td>电控组 <td>融合版：停止绕过队列 +
 *                                                    代次失效 + 控制权 CAS +
 *                                                    输入校验 + 占用保持
 *          </table>
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

#ifndef __ROBOT_H
#define __ROBOT_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>
#include "cfg.h"

/* ==========================================================================
 *                          公开类型定义
 * ========================================================================== */

/**
 * @brief   控制模式
 */
enum RMode : uint8_t {
    R_IDLE    = 0,      /**< 空闲：PWM=0，不接受驱动指令                     */
    R_MANUAL  = 1,      /**< 连续实时：按住即走，松开即停                    */
    R_MOVE    = 2,      /**< 精准直线：梯形规划 + 航向保持                   */
    R_TURN    = 3,      /**< 精准转向：脉冲主转 + IMU 精修                   */
    R_CAL_IMU = 4,      /**< IMU 零偏采样中（车辆必须静止）                  */
    R_ESTOP   = 5       /**< 急停锁定：需显式解锁才能恢复                    */
};

/**
 * @brief   动作执行状态
 */
enum RState : uint8_t {
    RS_IDLE = 0,        /**< 无动作                                          */
    RS_RUN  = 1,        /**< 动作执行中                                      */
    RS_DONE = 2,        /**< 动作正常完成                                    */
    RS_ERR  = 3         /**< 动作异常终止（超时 / 安全触发）                 */
};

/**
 * @brief   上行指令类型（网络核 → 控制核）
 */
enum CmdType : uint8_t {
    C_NOP = 0,          /**< 空指令                                          */
    C_STOP,             /**< 停车                                            */
    C_DRV,              /**< 连续驱动（a=x, b=y, u=速度百分比）              */
    C_MOVE,             /**< 精准直线（a=距离 cm, u=速度百分比，0=用默认值） */
    C_TURN,             /**< 精准转向（a=角度 °，正=逆时针）                 */
    C_SERVO,            /**< 舵机（u=0 放铲，1 举铲）                        */
    C_ESTOP,            /**< 急停（锁定）                                    */
    C_UNLOCK,           /**< 解除急停                                        */
    C_CAL,              /**< 开始标定（u=CAL_*）                             */
    C_CAL_FIN,          /**< 回填标定实测值（u=CAL_*, a=实测值）             */
    C_HB                /**< 纯心跳：只刷新看门狗，不改变当前输入            */
};

/**
 * @brief   标定项目
 */
enum CalMode : uint8_t {
    CAL_NONE = 0,       /**< 无标定                                          */
    CAL_IMU  = 1,       /**< IMU 零偏标定                                    */
    CAL_ODO  = 2,       /**< 行程标定（解算 K_odo）                          */
    CAL_TURN = 3        /**< 转向标定（解算 K_turn）                         */
};

/**
 * @brief   指令包（通过队列跨核传递，定长、无指针，天然线程安全）
 * @note    @c client 是发起指令的 WebSocket 连接编号，用于**控制权协调**，
 *          不承担任何身份认证职责（本项目为局域网设备，无鉴权设计）。
 */
struct Cmd {
    uint8_t  t;          /**< 指令类型 @ref CmdType                           */
    float    a;          /**< 参数 A（距离 / 角度 / x）                       */
    float    b;          /**< 参数 B（y）                                     */
    uint8_t  u;          /**< 参数 U（速度百分比 / 标定模式 / 舵机位置）      */
    uint32_t client;     /**< 发起方连接编号，0 表示未知 / REST 通道          */
};

/**
 * @brief   动作完成或事件回传包（控制核 → 网络核）
 */
struct DoneMsg {
    uint8_t type;       /**< 事件类型 @ref DoneType                          */
    uint8_t sub;        /**< 子类型 / 告警码                                 */
    float   target;     /**< 目标值（cm / ° / 命令值）                       */
    float   actual;     /**< 实际完成值（或解算出的系数）                    */
    float   err;        /**< 误差 = actual − target                          */
};

/**
 * @brief   回传事件类型
 */
enum DoneType : uint8_t {
    D_MOVE          = 1,    /**< 精准直线完成                                */
    D_TURN          = 2,    /**< 精准转向完成                                */
    D_CAL_IMU       = 3,    /**< IMU 零偏标定完成                            */
    D_CAL_ODO_WAIT  = 4,    /**< 行程标定动作完成，等待用户输入实测距离      */
    D_CAL_TURN_WAIT = 5,    /**< 转向标定动作完成，等待用户输入实测角度      */
    D_CAL_RESULT    = 6,    /**< 标定系数解算完成                            */
    D_ALERT         = 7     /**< 告警（急停 / 堵转 / 低压 / 边缘 / 心跳）    */
};

/**
 * @brief   遥测数据快照（供网络核按 15Hz 读取并推送到网页）
 */
struct Telemetry {
    float   rpm[4];         /**< 四轮转速 rpm（带符号）                      */
    float   spd;            /**< 车体线速度 cm/s                             */
    float   yaw;            /**< 融合偏航角 °                                */
    float   yawRate;        /**< 偏航角速度 °/s                              */
    int32_t pulses[4];      /**< 四轮累计脉冲                                */
    float   odoCm;          /**< 累计里程 cm                                 */
    float   bat;            /**< 电池电压 V                                  */
    float   ir[6];          /**< 六路红外测距 cm                             */
    bool    irValid[6];     /**< 各路红外是否有效（无效时网页显示 "--"）     */
    bool    batValid;       /**< 电池电压检测是否有效                        */
    bool    gray[4];        /**< 四路灰度                                    */
    bool    e18[3];         /**< 三路光电开关                                */
    bool    ioOk;           /**< MCP23017 在线 → gray/e18 有意义             */
    bool    senStale;       /**< 传感器数据已超时（>500ms 未刷新）           */
    float   acc[3];         /**< 三轴加速度 g                                */
    float   pitch, roll;    /**< 俯仰 / 横滚 °                               */
    bool    mpuOK, adxlOK;  /**< IMU 在线标志                                */
    uint8_t mode;           /**< 当前模式 @ref RMode                         */
    uint8_t state;          /**< 动作状态 @ref RState                        */
    float   progress;       /**< 当前动作进度 0.0 ~ 1.0                      */
    bool    estop, lowBat;  /**< 急停 / 低压标志                             */
    uint32_t owner;         /**< 当前控制者连接编号，0 表示无人占用          */
    bool    stall[4];       /**< 四轮堵转标志                                */
    bool    edge, irLimit;  /**< 边缘触发 / 红外限速                         */
    uint8_t calPending;     /**< 等待用户输入实测值的标定项                  */
    int     servoUS;        /**< 舵机当前脉宽 µs                             */
    uint32_t uptime;        /**< 运行时间 ms                                 */
};

/* ==========================================================================
 *                              对外接口
 * ========================================================================== */

/**
 * @brief   初始化机器人子系统并启动控制任务
 * @param   无
 * @return  无
 * @note    内部依次初始化 IMU → 传感器 → 电机，并在 Core 0 创建 200Hz 控制任务。
 */
void robotInit();

/**
 * @brief   控制任务主体（由 robotInit 创建，禁止外部调用）
 * @param[in] arg  FreeRTOS 任务参数（未使用）
 * @return   无
 */
void robotTask(void* arg);

/**
 * @brief   向控制核投递一条指令（含控制权仲裁与输入校验）
 * @param[in] c  指令包；@ref Cmd::client 必须填发起方的连接编号
 * @return  true 指令已受理；false 被拒绝（队列满 / 已被占用 / 急停锁定 / 参数非法）
 *
 * @details 融合版相对初版的三处强化：
 *          ① **Stop / Estop 绕过队列**：
 *             初版把所有指令一视同仁地塞进队列，队列一旦满了，
 *             **急停指令会被直接丢弃**——这是最危险的一种失效模式。
 *             本版改为：停止类指令不排队，而是递增「代次」并置起原子标志位，
 *             控制核下一拍必定处理，同时让队列里尚未执行的旧指令自动作废。
 *          ② **客户端控制权（owner）**：
 *             同一时刻只允许一个连接操控小车，用原子 CAS 抢占；
 *             心跳只对当前占用者有效，避免别的连接"续命"。
 *          ③ **输入校验**：非有限数（NaN/Inf）与越界参数一律拒绝。
 *
 * @warning 返回 true 只表示「已受理」，不代表动作已经执行完成；
 *          完成状态请读取 @ref robotPopDone 的回传事件。
 */
bool robotPushCmd(const Cmd& c);

/**
 * @brief   通知控制核：某个连接已断开
 * @param[in] client  断开的连接编号
 * @return   无
 * @note     若断开的正是当前控制者，则自动登记一次普通停止。
 *          这是「失联即停」安全链的第一道闸门（第二道是心跳超时）。
 */
void robotDisconnect(uint32_t client);

/**
 * @brief   查询控制核是否正忙
 * @param   无
 * @return  true 有动作在执行、或有停止请求待处理
 * @note    供 Web 层在保存配置前判断「能否安全改参数」。
 */
bool robotBusy();

/**
 * @brief   查询当前控制者
 * @param   无
 * @return  当前占用者的连接编号；0 表示无人占用
 */
uint32_t robotOwner();

/**
 * @brief   置位 / 清除「外部占用保持」
 * @param[in] on  true 进入占用保持，false 解除
 * @return   无
 *
 * @details Web 层在**写 NVS 配置**期间调用本接口加保持位：
 *          此时 @ref robotBusy 恒为 true，新动作无法抢占控制权，
 *          避免在保存过程中被一条移动指令改写同一份配置。
 *          保持位置位时会同步释放当前控制者，防止"占着位置不干活"。
 *
 * @note    保持位是**独立于控制周期**的，不会被控制任务每拍的
 *          忙闲结算覆盖掉。
 */
void robotHold(bool on);

/**
 * @brief   取回一条回传事件
 * @param[out] d  事件包
 * @return  true 取到事件；false 队列为空
 */
bool robotPopDone(DoneMsg& d);

/**
 * @brief   拷贝一份遥测快照
 * @param[out] out  遥测结构体
 * @return   无
 */
void robotCopyTelemetry(Telemetry& out);

/**
 * @brief   读取最近一次心跳时间戳
 * @param   无
 * @return  最近心跳时间戳（ms），0 表示从未收到
 */
uint32_t robotLastHbMs();

#endif /* __ROBOT_H */
