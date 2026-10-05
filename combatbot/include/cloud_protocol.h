/**
 * @file    cloud_protocol.h
 * @brief   云端租约、反重放与设备时钟指令期限。
 *
 * 时钟约定：许可由设备签发，600ms 后到期；不依赖浏览器或服务器时间。
 * 并发约定：实例仅由云任务操作，控制队列携带原始截止时间再次检查。
 * 设计边界：本模块只验证消息上下文；身份验证由 TLS 与服务端设备凭据承担。
 */
#pragma once
#include <stdint.h>
#include <cstring>
namespace bot {
class CloudProtocol {
 public:
  static constexpr uint32_t PermitMs=600;
  bool begin(const char* boot,const char* link) {
    reset(); if(!hex(boot) || !hex(link)) return false;
    copy(boot_,boot); copy(link_,link); return true;
  }
  void reset() { boot_[0]=link_[0]=lease_[0]=0; sequence_=0; index_=0; for(auto& p:permits_) p.valid=false; }
  bool epoch(const char* boot,const char* link) const {
    return boot && link && boot_[0] && !std::strcmp(boot_,boot) && !std::strcmp(link_,link);
  }
  bool setLease(const char* boot,const char* link,const char* lease) {
    if(!epoch(boot,link) || !hex(lease)) return false;
    if(std::strcmp(lease_,lease)) { copy(lease_,lease); sequence_=0; }
    return true;
  }
  bool endLease(const char* boot,const char* link,const char* lease) {
    if(!epoch(boot,link) || !lease || !lease_[0] || std::strcmp(lease_,lease)) return false;
    lease_[0]=0; sequence_=0; return true;
  }
  bool issue(const char* token,uint32_t now) {
    if(!hex(token) || !boot_[0]) return false;
    Permit& p=permits_[index_++ % 4]; copy(p.token,token); p.issued=now; p.valid=true; return true;
  }
  bool accept(const char* boot,const char* link,const char* lease,const char* permit,
              uint32_t sequence,uint32_t now,uint32_t& deadline) {
    if(!epoch(boot,link) || !lease || !lease_[0] || std::strcmp(lease_,lease) || !permit || sequence<=sequence_) return false;
    for(const auto& p:permits_) if(p.valid && !std::strcmp(p.token,permit) && uint32_t(now-p.issued)<PermitMs) {
      deadline=p.issued+PermitMs; sequence_=sequence; return true;
    }
    return false;
  }
  const char* boot() const { return boot_; }
  const char* link() const { return link_; }
  const char* lease() const { return lease_; }
 private:
  struct Permit { char token[65]={0}; uint32_t issued=0; bool valid=false; };
  Permit permits_[4]; char boot_[65]={0},link_[65]={0},lease_[65]={0};
  uint32_t sequence_=0,index_=0;
  static void copy(char* dst,const char* src) { std::memcpy(dst,src,std::strlen(src)+1); }
  static bool hex(const char* value) {
    if(!value) return false;
    const size_t n=std::strlen(value); if(n<32 || n>64) return false;
    for(size_t i=0;i<n;++i) if(!((value[i]>='0' && value[i]<='9') || (value[i]>='a' && value[i]<='f') || (value[i]>='A' && value[i]<='F'))) return false;
    return true;
  }
};
}
