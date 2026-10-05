/**
 * @file    controller.h
 * @brief   运动控制入口、命令投递与遥测快照接口。
 *
 * ============================================================================
 * 模块职责
 *   将网络侧命令交给唯一的运动控制任务，向调用方提供按值复制的遥测。
 *
 * 运行约定
 *   初始化后可从网络回调投递命令；调用方不应直接操作控制状态或电机输出。
 *   命令受理与动作完成是两个阶段，完成状态通过遥测结果字段读取。
 * ============================================================================
 */
#pragma once
#include "types.h"
namespace bot {
/**
 * @brief   建立命令队列，并在 Core 1 启动运动控制任务。
 * @pre 配置存储与硬件初始化已经完成；仅在启动流程中调用一次。
 * @note 创建失败会进入锁定状态，故障信息由遥测快照提供。
 */
void controllerBegin();
/**
 * @brief   登记停止、心跳请求，或将普通命令投递到队列。
 * @return true 表示请求已受理；false 表示忙、所有权冲突、锁定或队列已满等。
 * @details 普通动作按客户端排他占用。持续驾驶允许同一客户端更新目标；
 *          心跳仅接受当前占用者。Stop 与 Estop 不受普通队列容量限制。
 * @note 受理成功不代表动作已执行。Stop 允许随后提交新动作；Estop 需先解锁。
 */
bool controllerEnqueue(const Command&);
/** @brief 当前运动占用者断开连接时，登记普通停止请求。 */
void controllerDisconnect(uint32_t client);
/**
 * @brief   登记软件急停请求，并锁定后续运动命令。
 * @note 控制任务响应后撤去 PWM；不保证固定响应上界，也不等同于机械刹车。
 */
void controllerEmergencyStop();
/** @brief 在临界区内复制已发布遥测；返回值可在锁外读取。 */
Telemetry controllerSnapshot();
/** @brief 查询是否有动作占用或尚待处理的停止请求。 */
bool controllerBusy();
/**
 * @brief 原子申请空闲配置窗口；与命令消费及运动所有权共同仲裁。
 * @return 成功后普通动作被禁止，旧非运动队列失效；Stop/Estop仍可登记。
 * @note 必须在完成或失败后配对调用 controllerEndConfig()，锁内不等待网络。
 */
bool controllerTryBeginConfig();
/** @brief 释放已取得的配置窗口，不改变急停状态或启动运动。 */
void controllerEndConfig();
}
