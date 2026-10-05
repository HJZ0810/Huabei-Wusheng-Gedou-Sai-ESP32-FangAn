/**
 * @file test_motion.cpp
 * @brief 验证运动数学工具的边界、单位换算与安全判据。
 *
 * ============================================================================
 * 测试范围
 *   直接执行 PID、运动学和安全判定头文件中的真实算法，无需 ESP32 或连接硬件。
 *   断言覆盖指定数值场景，不验证电机响应、传感器精度或任务并发。
 * ============================================================================
 */
#include "pid.h"
#include "kinematics.h"
#include "safety.h"
#include <cassert>
#include <cstdio>
using namespace bot;
static bool near(float a,float b,float eps=0.001f) { return std::fabs(a-b)<eps; }
int main() {
  // ============================================================================
  // 场景一：脉冲比例、轮行程与转角的单位换算
  // ============================================================================
  Config c;
  assert(near(pulsesPerCm(c),90/(7.2f*pi)));
  c.pulsesPerCm=4; assert(near(pulsesPerCm(c),4));
  assert(near(wheelTurnDistance(360,c),17*pi));
  assert(near(wheelTurnDistance(-90,c),-17*pi/4));
  c.turnFactor=1.5f;
  const float travel=wheelTurnDistance(90,c);
  assert(near(odoYawDegrees(-travel,travel,c),90));
  int64_t start[4]={100,200,300,400}, forward[4]={140,240,340,440}, backward[4]={60,160,260,360}, turn[4]={60,240,260,440};
  assert(near(pulseTravel(forward,start,c),10));
  assert(near(pulseTravel(backward,start,c),-10));
  assert(near(absolutePulseTravel(turn,start),40));
  assert(near(pulseTravel(turn,start,c),0));
  // ============================================================================
  // 场景二：停车包络、加减速和先停后换向
  // ============================================================================
  assert(near(stoppingVelocity(2,100,40),20));
  assert(near(stoppingVelocity(-2,100,40),-20));
  assert(near(stoppingVelocity(100,100,40),40));
  assert(near(rampVelocity(0,40,80,100,0.01f),0.8f));
  assert(near(rampVelocity(40,0,80,100,0.01f),39));
  assert(near(rampVelocity(1,-40,80,100,0.01f),0));
  // ============================================================================
  // 场景三：融合增量持续累加，支持 360° 多圈目标
  // ============================================================================
  c.gyroBias=2; c.fusionAlpha=0.85f;
  float yaw=0; for(int i=0;i<400;++i) yaw+=fusedYawIncrement(92,0.01f,0.9f,c);
  assert(near(yaw,360,0.003f));
  // ============================================================================
  // 场景四：长时间输出饱和后，积分不留下同向残余
  // ============================================================================
  // 10000 次 × 0.01 s = 100 s；输出限幅期间持续给出正误差。
  Pid pid; Gains gains{1,10,0};
  for(int i=0;i<10000;++i) assert(near(pid.step(100,0.01f,gains,-10,10),10));
  assert(near(pid.step(0,0.01f,gains,-10,10),0));
  assert(pid.step(-1,0.01f,gains,-10,10)<0);
  // 前馈、侧向补偿与限幅组合必须共同参与抗积分饱和，而非只限制裸 PID。
  Config pwmConfig; pwmConfig.speedPid=Gains{30,10,0}; pwmConfig.pwmDeadzone=0;
  Pid fullOutput, trimmedOutput;
  for(int i=0;i<10000;++i) {
    assert(near(wheelSpeedPwm(fullOutput,80,70,0.01f,pwmConfig,1),1023));
    assert(near(wheelSpeedPwm(trimmedOutput,80,70,0.01f,pwmConfig,1.5f),1023));
  }
  assert(near(wheelSpeedPwm(fullOutput,80,80,0.01f,pwmConfig,1),818.4f));
  // trim 1.5 已使前馈本身达到上限，仍应允许负误差降低输出。
  assert(wheelSpeedPwm(trimmedOutput,80,90,0.01f,pwmConfig,1.5f)<1023);
  assert(near(wheelSpeedPwm(fullOutput,-80,-80,0.01f,pwmConfig,1),-818.4f));
  assert(wheelSpeedPwm(fullOutput,0,0,0.01f,pwmConfig,1)==0);
  // ============================================================================
  // 场景五：运动方向、数字危险输入、倾倒与时间回绕
  // ============================================================================
  Sensors s; c.irCount=6; c.irThresholdCm=25; c.irSlowdownCm=60; c.irFailSafeStop=false;
  s.irValid[0]=true; s.ir[0]=20;
  assert(obstacleInDirection(s,c,1,0)); assert(!obstacleInDirection(s,c,-1,0));
  s.irValid[0]=false; s.irValid[3]=true; s.ir[3]=20;
  assert(obstacleInDirection(s,c,-1,0)); assert(!obstacleInDirection(s,c,1,0));
  assert(obstacleInDirection(s,c,0,1));
  s.irValid[3]=false; s.irValid[0]=true; s.ir[0]=42.5f;
  assert(near(directionSafety(s,c,1,0).speedScale,0.5f));
  assert(directionSafety(s,c,1,0).unavailable); // 缺失的两条前半平面通道仍应告警。
  c.irFailSafeStop=true;
  assert(directionSafety(s,c,1,0).speedScale==0);
  for(int i=0;i<6;++i) { s.irValid[i]=true; s.ir[i]=100; }
  s.ir[0]=60; assert(directionSafety(s,c,1,0).speedScale==1);
  s.ir[0]=25; assert(directionSafety(s,c,1,0).speedScale==0);
  assert(directionSafety(s,c,-1,0).speedScale==1);
  s.irValid[0]=false; assert(directionSafety(s,c,-1,0).speedScale==1);
  assert(directionSafety(s,c,0,1).speedScale==0);
  assert(directionSafety(s,c,0,0.001f).speedScale==0); // 低速转向同样检查全周。
  assert(directionSafety(s,c,0,0).speedScale==1);
  s.gray[2]=true; assert(sensorEdge(s)); s.gray[2]=false; s.e18[1]=true; assert(sensorEdge(s));
  s.accelOk=true; s.acc[2]=1; assert(!sensorTilt(s)); s.acc[2]=-1; assert(sensorTilt(s));
  assert(timedOut(15,0xfffffff0u,20)); assert(!timedOut(15,0xfffffff0u,40));
  assert(!timedOut(100,105,250)); // 同一周期中刚更新的异步时间戳，不能误判为失联。
  std::puts("motion tests passed");
}
