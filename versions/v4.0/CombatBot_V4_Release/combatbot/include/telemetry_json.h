/** @file telemetry_json.h
 *  @brief 将线程安全快照转换为统一遥测协议；不导出网络凭据。
 */
#pragma once
#include <ArduinoJson.h>
namespace bot { void telemetryJson(JsonObject output); }
