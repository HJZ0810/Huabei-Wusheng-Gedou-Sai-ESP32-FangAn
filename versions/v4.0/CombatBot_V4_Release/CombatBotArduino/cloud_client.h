/**
 * @file    cloud_client.h
 * @brief   STA 模式下的主动 WSS 云端连接。
 *
 * 云任务独占套接字；DNS、TLS 与重连不进入运动控制任务。
 * 联网失败保留 AP；自主任务由车端预算和地面保护约束，网络断线不重启动作。
 */
#pragma once
namespace bot {
void cloudBegin();
const char* cloudState();
}
