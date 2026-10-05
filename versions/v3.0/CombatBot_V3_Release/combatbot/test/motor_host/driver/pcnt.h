/** @file pcnt.h @brief 成功初始化、可注入计数值的 PCNT 替身。 */
#pragma once
#include <Arduino.h>
typedef int pcnt_unit_t;
constexpr int ESP_OK=0;
constexpr int PCNT_PIN_NOT_USED=-1, PCNT_CHANNEL_0=0;
constexpr int PCNT_COUNT_INC=1, PCNT_COUNT_DIS=0, PCNT_MODE_KEEP=0;
struct pcnt_config_t {
  int pulse_gpio_num,ctrl_gpio_num,channel,unit;
  int pos_mode,neg_mode,lctrl_mode,hctrl_mode,counter_h_lim,counter_l_lim;
};
inline int pcnt_unit_config(const pcnt_config_t*) { return ESP_OK; }
inline int pcnt_set_filter_value(pcnt_unit_t,int) { return ESP_OK; }
inline int pcnt_filter_enable(pcnt_unit_t) { return ESP_OK; }
inline int pcnt_counter_pause(pcnt_unit_t) { return ESP_OK; }
inline int pcnt_counter_clear(pcnt_unit_t unit) { motor_host::counts[unit]=0; return ESP_OK; }
inline int pcnt_counter_resume(pcnt_unit_t) { return ESP_OK; }
inline int pcnt_get_counter_value(pcnt_unit_t unit,int16_t* value) {
  *value=motor_host::counts[unit]; return ESP_OK;
}
