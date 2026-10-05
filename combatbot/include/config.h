/**
 * @file    config.h
 * @brief   运行配置的快照、校验、JSON 转换与 NVS 持久化接口。
 *
 * ============================================================================
 * 接口约定
 *   配置以完整快照发布；部分更新先合并、校验并写入 NVS，再替换内存快照。
 *   返回 false 时，调用方应展示 error；不得把失败写入当成保存成功。
 *
 * 调用时机
 *   保存接口本身不判断车辆是否运动。Web 接入层与控制器负责选择安全时机，
 *   Flash 写入和取得互斥锁均可能等待，不能作为实时控制环内的轻量操作。
 * ============================================================================
 */
#pragma once
#include "types.h"
#include <Arduino.h>
#include <ArduinoJson.h>
namespace bot {
/** @brief 建立配置互斥锁并从 NVS 载入参数；存储缺失或非法时采用默认配置。 */
void configBegin();

/** @brief 取得按值复制的配置快照；返回后调用方不持有配置锁。 */
Config configSnapshot();

/**
 * @brief   将完整配置写入目标 JSON 对象。
 * @param[in] secrets true 时输出真实 WiFi 密码，false 时输出空字符串及存在标志。
 * @note 默认 secrets=true；导出的 JSON 应按含密码的本地配置文件管理。
 */
void configJson(const Config&, JsonObject, bool secrets=true);

/**
 * @brief   将 JSON 的部分字段合并至当前配置，校验后保存并发布。
 * @param[out] error 失败原因；成功时清空。
 * @return 校验和 NVS 写入均成功时返回 true。
 * @note 未提交的字段保持原值；密码的显式空字符串表示清空密码。
 */
bool configApply(JsonVariantConst, String& error);

/**
 * @brief   校验并保存一份完整配置；成功后替换运行快照。
 * @param[out] error 校验或持久化失败的原因。
 * @pre 调用方已选择合适的保存时机，避免在车辆动作期间写 Flash。
 */
bool configSave(const Config&, String& error);

/** @brief 保存全部出厂默认参数；网络参数在下次启动时应用。 */
bool configDefaults(String& error);

/** @brief 恢复默认 AP 与主机名、清除 STA 参数；保留机械及控制配置。 */
bool configClearWifi(String& error);
}
