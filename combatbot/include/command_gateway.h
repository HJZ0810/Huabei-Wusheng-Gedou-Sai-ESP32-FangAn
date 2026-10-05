/**
 * @file    command_gateway.h
 * @brief   本地与云端共用的指令校验入口。
 *
 * 前置条件：传输适配器已完成身份、会话和消息新鲜度校验。
 * 职责边界：仅解析、校验并投递；不得访问网络连接或直接操作电机。
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdint.h>
namespace bot {
bool commandDispatch(JsonVariantConst body,uint32_t client,String& error,
                     bool cloud=false,uint32_t expiresAt=0);
}
