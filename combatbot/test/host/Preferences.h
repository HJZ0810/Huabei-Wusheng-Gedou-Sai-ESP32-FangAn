/**
 * @file Preferences.h
 * @brief 使用内存映射模拟 Preferences 存取与失败路径。
 *
 * ============================================================================
 * 可注入打开或写入失败；不模拟 Flash、掉电恢复与存储磨损。
 * ============================================================================
 */
#pragma once
#include "Arduino.h"
#include <map>
inline std::map<std::string,std::string> hostNvs;
inline bool hostNvsOpen=true,hostNvsWrite=true;
class Preferences {
  std::string namespaceName;
  bool opened=false,readOnly=true;
public:
  bool begin(const char* name,bool readonly) {
    namespaceName=name; readOnly=readonly; opened=hostNvsOpen; return opened;
  }
  String getString(const char* key,const char* fallback) {
    if(!opened) return String(fallback);
    const auto it=hostNvs.find(namespaceName+"/"+key); return it==hostNvs.end()?String(fallback):it->second;
  }
  size_t putString(const char* key,const String& data) {
    if(!opened || readOnly || !hostNvsWrite)return 0;
    hostNvs[namespaceName+"/"+key]=data;return data.length();
  }
  void end() { opened=false; }
};
