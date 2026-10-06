/**
 * @file    types.h
 * @brief   定义配置、控制指令、传感器快照与运行遥测的公共数据模型。
 *
 * ============================================================================
 * 模块职责：集中维护跨模块数据约定，使采集、控制与网页使用相同的字段语义。
 * 运行约定：四轮数组统一按 LF / RF / LR / RR（左前、右前、左后、右后）排序。
 * 设计边界：结构体仅承载数据；有效性校验、并发访问与持久化由所属模块负责。
 * ============================================================================
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "arena_model.h"

namespace bot {
/** @brief 固件发行版本；用于启动日志与握手识别，不作为配置版本或账户凭据。 */
constexpr const char* FirmwareVersion="5.0.0";
constexpr uint32_t CloudOwner=0xFFFFFFFEu, AutonomousOwner=0xFFFFFFFDu;
// ============================================================================
// 控制参数 · PID 系数与车辆配置
// ============================================================================

/**
 * @brief   保存一组比例、积分与微分增益。
 * @details Arduino ESP32 2.0.17 默认使用 C++11；显式构造支持当前的花括号初始化。
 *          三组控制环使用各自的误差与输出单位，调参时应结合对应控制环理解增益。
 */
struct Gains {
  float kp,ki,kd; ///< 比例、积分、微分系数，顺序与配置 JSON 的 kp / ki / kd 一致。
  constexpr Gains(float p=0,float i=0,float d=0):kp(p),ki(i),kd(d){}
};
/** @brief 保存车辆完整配置；默认值用于首次启动及恢复出厂参数。 */
struct Config {
  uint32_t revision=1; ///< 内存发布代次；JSON 中只读，重启后重置，用于拒绝过期标定。
  // ============================================================================
  // 网络寻址 · 字符数组容量包含结尾的空字符
  // ============================================================================
  char apSsid[33]="CombatBot-AP", apPass[65]="12345678";
  char staSsid[33]="", staPass[65]="", hostname[33]="combatbot";
  bool staEnabled=false; ///< 是否尝试连接已配置的路由器；AP 由网络模块同时维护。

  // 云端只建立主动 WSS 出站连接；凭据与 WiFi 密码相互独立。
  bool cloudEnabled=false;
  char cloudHost[128]="combatbot.luo-jin-ai.com", cloudPath[128]="/ws/device";
  char cloudDeviceId[49]="combatbot-01", cloudDeviceKey[129]="";
  char cloudCaPem[2048]=""; ///< 公共 CA PEM；为空时拒绝联网，绝不降级证书校验。
  int cloudPort=443;

  // 自主模式在安装校准前禁止启动；尺寸是设计起点，须以机械外包络实测更新。
  bool arenaEnabled=false, arenaCalibrated=false, arenaClimbEnabled=false;
  float arenaOuterCm=380, arenaPlatformCm=240, arenaBodyLengthCm=26, arenaBodyWidthCm=24;
  float arenaMarginCm=8, arenaNavSpeed=12, arenaSearchSpeed=12, arenaPushSpeed=18, arenaClimbSpeed=20;
  float arenaEntryX=0, arenaEntryY=-150, arenaEntryHeading=90;
  int arenaTaskMs=180000, arenaClimbMs=6000;

  // ============================================================================
  // 机械尺寸与运动模型 · 长度配置以 mm 表示，运动控制以 cm 表示
  // ============================================================================
  // wheelbaseMm 目前用于配置存储与展示，尚未参与控制计算。
  // 三项尺寸单位均为 mm；PPR 按输出轴一圈的 FG 上升沿事件数定义。
  float wheelMm=72, wheelbaseMm=154, trackMm=170, ppr=90;
  // pulsesPerCm 为脉冲/cm，0 表示理论换算；左右滑移系数独立，通用系数保留旧协议语义。
  float pulsesPerCm=0, turnFactor=1, turnFactorLeft=1, turnFactorRight=1;
  // 最大/默认速度为 cm/s，加减速度为 cm/s²，转向角速度上限为 °/s。
  float maxSpeed=100, defaultSpeed=40, acceleration=80, deceleration=100, maxYawRate=120;
  float turnAcceleration=180, turnDeceleration=240; ///< 转向角加、减速度，单位 °/s²；与直线斜坡分别调节。

  // ============================================================================
  // 闭环控制与电机补偿 · 先核对极性，再调整增益和补偿
  // ============================================================================
  Gains speedPid{3,8,0}, positionPid{2,0,0}, headingPid{1.5f,0,0.04f};
  float fusionAlpha=0.85f; ///< IMU 在航向互补融合中的权重，范围 0～1。
  bool invert[4]={false,true,false,true}; ///< 四轮输出极性反转标志，轮序 LF / RF / LR / RR。
  // leftTrim/rightTrim 乘在左右侧 PWM 输出上；死区补偿使用 10 位占空比计数。
  float leftTrim=1, rightTrim=1, pwmDeadzone=110;

  // ============================================================================
  // 传感器映射与保护阈值 · 有效电平在采集层归一化为触发状态
  // ============================================================================
  bool grayActiveHigh=true, e18ActiveHigh=false, safetyEnabled=false;
  // 开发模式仅放宽手动摇杆的缺失传感器门槛；开环 PWM ≤180、单次 ≤15 s，默认关闭。
  bool developmentMode=false;
  // 总开关保留旧版语义；分项只在总开关启用时参与运动仲裁。
  bool edgeProtection=true, irProtection=true, tiltProtection=true;
  bool irFailSafeStop=true; ///< 启用红外保护时，相关方向测距无效则拒绝该方向运动。
  int digitalDebounceMs=30; ///< 数字灰度与 E18 输入去抖时间，单位 ms。
  float irThresholdCm=25, batteryLowV=21.5f, batteryScale=11; ///< 红外阈值（cm）、低压阈值（V）、分压还原倍率。
  float irSlowdownCm=60; ///< 红外方向限速的起始距离（cm），必须大于停车距离 irThresholdCm。
  float irScale=1; ///< 距离比例修正；不能补救非法电压或传感器近场盲区。
  float impactThresholdG=2; ///< 加速度模长告警阈值（g）；冲击告警不触发自动停车。
  int irCount=6, batteryChannel=6; ///< 红外数量 1～12；电池在 CD74HC4067 上的通道号 0～15。
  int gyroAxis=2, gyroSign=1; ///< 偏航测量轴 0/1/2 对应 X/Y/Z；方向取 +1 或 -1。
  float gyroBias=0; ///< 保存的陀螺静止零偏，单位 °/s；控制器积分前负责扣除。

  // ============================================================================
  // 舵机与失联策略 · 参数保存不代表硬件具备主动制动能力
  // ============================================================================
  int servoMinUs=500, servoMaxUs=2500, servoCenterUs=1500; ///< 最小、最大与开机中位脉宽，单位 µs。
  float servoDownDeg=0, servoUpDeg=90; ///< 放铲、举铲目标角度，单位 °。
  uint32_t heartbeatMs=1000; ///< 控制客户端心跳超时，单位 ms；由固件判断并停车。
  uint32_t stallTimeoutMs=1500; ///< 较高 PWM 且无新 FG 脉冲的持续判定时间，单位 ms。
  int telemetryHz=15; ///< WebSocket 遥测频率，单位 Hz；不改变运动控制任务周期。
  bool coastOnLoss=true; ///< 当前仅保存停车策略偏好；两种取值均不产生主动制动输出。
};

// ============================================================================
// 传感器快照 · 数值与有效性标志必须配合使用
// ============================================================================

/** @brief 汇总最近一轮传感器采样；默认值不代表已经获得有效测量。 */
struct Sensors {
  bool imuOk=false, accelOk=false, ioOk=false, batteryValid=false; ///< 偏航 IMU、加速度、数字 IO、电池测量有效标志。
  uint8_t accelSource=0; ///< 加速度来源：0 无有效数据，1 MPU6050（±2 g），2 ADXL345（±16 g）。
  bool accelSaturated=false, impact=false; ///< 任一加速度轴接近量程边界；本次模长超过冲击告警阈值。
  float impactG=0; ///< 当前有效加速度模长（g）；饱和时仅能视为冲击幅度的下界估计。
  float gyroDps=0, acc[3]={0,0,0}, ir[12]={0}; ///< °/s、X/Y/Z 三轴 g、红外距离 cm；gyroDps 尚未扣除零偏。
  bool irValid[12]={false}, gray[4]={false}, e18[3]={false}; ///< 红外逐路有效性；灰度及 E18 为已归一的触发状态。
  float batteryV=0; ///< 还原后的电池端电压，单位 V；仅在 batteryValid 为真时有效。
  // 本次采集调用的 millis() 时间；各模拟/数字通道的缓存不是同时采样。
  uint32_t sampledMs=0;
  uint32_t digitalMs=0, imuMs=0, irMs[12]={0}; ///< 实际成功采样时刻，不能用汇总时刻替代。
  bool groundRaw[3]={false}; ///< E18 原始归一状态：true 表示见地，危险置位不等待慢速去抖。
};

// ============================================================================
// 控制指令 · 网络入口完成解析后交给控制器排队处理
// ============================================================================

/** @brief 控制器支持的指令类型；标定开始与标定参数应用使用不同入口。 */
enum class CommandType { Heartbeat, Drive, Move, Turn, Stop, Estop, Unlock, Servo, ImuCal, OdoCalStart, TurnCalStart, ClimbObserveStart, ClimbObserveEnd, Pose, Navigate, Battle, Climb, Takeover };
// client 为 WebSocket 连接编号；它用于控制权协调，不构成身份认证凭据。
// x/y 为 [-1,1] 摇杆输入（右 / 前为正），speed 为 cm/s。
// value 随类型表示距离 cm、转角 ° 或舵机归一位置 [0,1]；receivedMs 为接收时间 ms。
struct Command {
  CommandType type=CommandType::Stop; uint32_t client=0;
  float x=0,y=0,speed=40,value=0; uint32_t receivedMs=0;
  bool expires=false; uint32_t expiresAt=0; ///< 云入口签发的设备时钟期限；入队后不得重算。
  bool upper=false; ///< Pose：人工明确设置所在层面，不能仅由坐标推断。
  bool directionalCalibration=false; ///< 区分旧版通用转向标定与左/右独立标定协议。
};

// ============================================================================
// 运行遥测与结果 · 控制器内部快照，网页协议由网络模块转换
// ============================================================================

/** @brief 保存控制器当前反馈与最近一次完成结果，供快照接口及网页读取。 */
struct Telemetry {
  arena::Decision arena;
  // rpm、wheelSpeed、pulses、pwm 均沿用 LF / RF / LR / RR 轮序。
  float rpm[4]={0}, wheelSpeed[4]={0}, speed=0, yaw=0, odo=0, progress=0; ///< rpm、cm/s、cm/s、°、cm、[0,1] 完成比例。
  int64_t pulses[4]={0}; ///< 有符号累计 FG 脉冲；方向来自驱动指令记忆，单相 FG 不测外力方向。
  uint16_t pwm[4]={0}; ///< 当前四轮 PWM 幅值，10 位输出范围 0～1023。
  Sensors sensors;
  bool estop=false, lowBattery=false, imuCalibrating=false;
  bool irLimited=false, irUnavailable=false; ///< 方向红外正在限速；相关方向测距无效。
  float irSpeedScale=1; ///< 当前方向红外施加的速度比例 [0,1]，供页面解释保护状态。
  uint32_t owner=0; ///< 当前控制连接编号；0 表示无控制者，不表示认证状态。
  char state[24]="idle", fault[64]="";
  // 网页输出 progress 时乘以 100；完成结果按 resultId 去重推送。
  bool resultReady=false; ///< 最近一次动作结果是否可读；停车或失败不等同于正常完成。
  char resultType[20]="";
  float target=0, actual=0, error=0; ///< 目标、实际量与误差；单位随结果类型为 cm、° 或 IMU 零偏 °/s。
  uint32_t resultId=0; ///< 本次启动会话内递增的完成编号；不跨重启持久化。
  uint32_t resultConfigRevision=0; ///< 结果生成时使用的配置代次；拒绝用旧结果覆盖新参数。
  uint32_t calibrationSessionId=0; ///< 本次上电随机标识；和 resultId 组合隔离旧页面/旧结果。
  float calibrationPulses=0, calibrationTurnFactor=1; ///< 四轮绝对行程脉冲均值及标定动作使用的转向系数。
  float calibrationMaxTiltDeg=0; ///< 登台观测期间采样到的最大加速度倾角；仅为观测值。
  uint32_t calibrationDurationMs=0, calibrationSupportChanges=0; ///< 观测时长及支撑输入变化数，不代表高度或登台成功。
};
}
