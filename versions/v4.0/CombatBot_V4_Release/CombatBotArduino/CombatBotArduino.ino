/**
 * @file    CombatBotArduino.ino
 * @brief   应用入口：按安全顺序建立硬件、控制任务与本地网络服务。
 *
 * ============================================================================
 * 启动顺序
 *   配置载入 → 硬件初始化与零输出 → 控制任务 → WiFi → Web 服务。
 *   先建立安全输出，再开放远程控制入口，避免网络请求先于控制器就绪。
 *
 * 任务分工
 *   Arduino loop 服务 DNS、网络重试和遥测；Core 1 控制任务负责运动与采集。
 *   配置快照通过互斥锁取得，取得快照时可能等待正在进行的配置写入。
 *
 * 硬件边界
 *   软件只能在 GPIO 初始化后撤去驱动；上电与复位窗口由 PWM 硬件下拉保护。
 * ============================================================================
 */
#include <Arduino.h>
#include "config.h"
#include "hardware.h"
#include "controller.h"
#include "wifi_mgr.h"
#include "web.h"
#include "cloud_client.h"

// ============================================================================
// 启动阶段 · 只建立初始状态，不提交任何运动动作
// ============================================================================

/**
 * @brief   初始化整车运行环境，由 Arduino 框架在启动时调用一次。
 * @details
 *   1. 载入 NVS 配置，取得本次启动使用的参数快照。
 *   2. 初始化电机、舵机和传感器，并再次将四轮 PWM 清零。
 *   3. 建立控制任务后启动 AP/STA 和 Web，允许浏览器提交控制请求。
 * @note motorStop() 撤去调速输出，不代表车体已机械停止。
 */
void setup() {
  Serial.begin(115200);
  bot::configBegin();
  const auto config = bot::configSnapshot();
  // 控制入口尚未开放：完成硬件初始化后，再显式确认电机调速输出为零。
  bot::hardwareBegin(config);
  bot::motorStop();
  bot::controllerBegin();
  bot::wifiBegin(config);
  bot::webBegin();
  bot::cloudBegin();
  Serial.printf("CombatBot V%s ready; AP fallback: http://192.168.4.1\n",bot::FirmwareVersion);
}

// ============================================================================
// 服务阶段 · 运动闭环由独立任务运行，此处不等待 STA 连接完成
// ============================================================================

/**
 * @brief   周期性服务网络状态与遥测，并给调度器留出运行空档。
 * @note vTaskDelay(1) 的单位是 FreeRTOS tick，不能直接解释为固定 1 ms。
 */
void loop() {
  bot::wifiTick(bot::configSnapshot());
  bot::webTick();
  // 仅 taskYIELD() 不一定让低优先级 idle 任务运行，因此主动休眠一个 tick。
  vTaskDelay(1);
}
