/**
 * @file test_cloud_protocol.cpp
 * @brief 验证真实云协议的会话隔离、设备时钟期限与反重放。
 * @note 全部令牌为固定测试夹具，日志只输出场景结果，不输出令牌或凭据。
 */
#include "cloud_protocol.h"
#include <cassert>
#include <cstdio>
#include <string>
static const char* bootA="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char* bootB="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char* linkA="cccccccccccccccccccccccccccccccc";
static const char* linkB="dddddddddddddddddddddddddddddddd";
static const char* leaseA="eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
static const char* leaseB="ffffffffffffffffffffffffffffffff";
static const char* permitA="11111111111111111111111111111111";
static const char* permitB="22222222222222222222222222222222";
static const char* permitC="33333333333333333333333333333333";
int main(){
  bot::CloudProtocol protocol;uint32_t deadline=0;
  assert(!protocol.issue(permitA,100));assert(!protocol.setLease(bootA,linkA,leaseA));
  for(const char* bad:{static_cast<const char*>(nullptr),"","short","not_hex_token_aaaaaaaaaaaaaaaaaaaaa"}){
    assert(!protocol.begin(bad,linkA));assert(!protocol.begin(bootA,bad));
  }
  const std::string longToken(65,'a'),minToken(31,'a');
  assert(!protocol.begin(longToken.c_str(),linkA));assert(!protocol.begin(minToken.c_str(),linkA));
  assert(protocol.begin(bootA,linkA));assert(protocol.epoch(bootA,linkA));
  assert(!protocol.epoch(bootA,linkB));assert(!protocol.epoch(nullptr,linkA));
  assert(!protocol.setLease(bootB,linkA,leaseA));assert(!protocol.setLease(bootA,linkA,longToken.c_str()));
  assert(protocol.setLease(bootA,linkA,leaseA));assert(protocol.issue(permitA,100));
  assert(!protocol.accept(bootA,linkA,leaseA,permitA,0,100,deadline));
  assert(protocol.accept(bootA,linkA,leaseA,permitA,1,100,deadline));assert(deadline==700);
  assert(!protocol.accept(bootA,linkA,leaseA,permitA,1,110,deadline)); // 相同序号重放。
  assert(!protocol.accept(bootB,linkA,leaseA,permitA,2,110,deadline));
  assert(!protocol.accept(bootA,linkB,leaseA,permitA,2,110,deadline));
  assert(!protocol.accept(bootA,linkA,leaseB,permitA,2,110,deadline));
  assert(!protocol.accept(bootA,linkA,leaseA,nullptr,2,110,deadline));
  assert(!protocol.accept(bootA,linkA,leaseA,"",2,110,deadline));
  assert(!protocol.accept(bootA,linkA,leaseA,permitB,2,110,deadline));
  // 拒绝不消费序号，合法重试仍可使用该序号；期限保持原签发时间。
  assert(protocol.accept(bootA,linkA,leaseA,permitA,2,699,deadline));assert(deadline==700);
  assert(!protocol.accept(bootA,linkA,leaseA,permitA,3,700,deadline));
  assert(protocol.issue(permitB,500));assert(!protocol.accept(bootA,linkA,leaseA,permitB,3,499,deadline));
  assert(protocol.accept(bootA,linkA,leaseA,permitB,3,500,deadline));assert(deadline==1100);
  // 重复租约声明不清序号；新租约才建立新的序号空间。
  assert(protocol.setLease(bootA,linkA,leaseA));assert(!protocol.accept(bootA,linkA,leaseA,permitB,3,510,deadline));
  assert(protocol.setLease(bootA,linkA,leaseB));
  assert(!protocol.accept(bootA,linkA,leaseA,permitB,4,510,deadline));
  assert(protocol.accept(bootA,linkA,leaseB,permitB,1,510,deadline));
  assert(!protocol.endLease(bootA,linkB,leaseB));assert(!protocol.endLease(bootA,linkA,leaseA));
  assert(protocol.endLease(bootA,linkA,leaseB));assert(!protocol.accept(bootA,linkA,leaseB,permitB,2,520,deadline));
  assert(protocol.setLease(bootA,linkA,leaseB));assert(protocol.accept(bootA,linkA,leaseB,permitB,1,530,deadline));
  // millis 回绕：uint32 差值仍保留 600 ms 窗口，未来时间不会被误当新鲜。
  assert(protocol.issue(permitC,0xFFFFFFF0u));
  assert(protocol.accept(bootA,linkA,leaseB,permitC,2,20,deadline));assert(deadline==584);
  assert(protocol.accept(bootA,linkA,leaseB,permitC,3,583,deadline));
  assert(!protocol.accept(bootA,linkA,leaseB,permitC,4,584,deadline));
  const char* futurePermit="66666666666666666666666666666666";
  assert(protocol.issue(futurePermit,600));assert(!protocol.accept(bootA,linkA,leaseB,futurePermit,4,599,deadline));
  assert(protocol.accept(bootA,linkA,leaseB,futurePermit,0xFFFFFFFFu,600,deadline));
  assert(!protocol.accept(bootA,linkA,leaseB,futurePermit,1,610,deadline)); // 序号也不能回绕重放。
  // 新启动/连接代次撤销旧许可，即使令牌尚在自己的时间窗口内。
  assert(protocol.begin(bootB,linkB));assert(protocol.setLease(bootB,linkB,leaseA));
  assert(!protocol.accept(bootB,linkB,leaseA,futurePermit,1,610,deadline));
  assert(!protocol.accept(bootA,linkA,leaseB,futurePermit,1,610,deadline));
  const std::string valid64(64,'a');assert(protocol.issue(valid64.c_str(),610));
  assert(protocol.accept(bootB,linkB,leaseA,valid64.c_str(),1,610,deadline));
  assert(!protocol.issue(longToken.c_str(),610));assert(!protocol.issue(minToken.c_str(),610));
  // 许可历史有界；四槽之外的旧值不保留，当前和最近槽仍可独立校验。
  const char* ring[]={permitA,permitB,permitC,"44444444444444444444444444444444","55555555555555555555555555555555"};
  for(unsigned i=0;i<5;++i)assert(protocol.issue(ring[i],700+i*10));
  assert(!protocol.accept(bootB,linkB,leaseA,permitA,2,750,deadline));
  assert(protocol.accept(bootB,linkB,leaseA,permitB,2,750,deadline));
  assert(protocol.accept(bootB,linkB,leaseA,ring[4],3,750,deadline));
  protocol.reset();assert(!protocol.epoch(bootB,linkB));assert(!protocol.accept(bootB,linkB,leaseA,ring[4],4,760,deadline));
  std::puts("PASS: real cloud protocol epoch / lease / replay sequence / strict token bounds / 600 ms boundary / future permit / clock rollover / bounded history / reset");
}
