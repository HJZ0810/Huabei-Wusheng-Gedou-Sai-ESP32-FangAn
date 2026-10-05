#include "session_registry.h"
#include <cassert>
#include <cstdio>

int main() {
  bot::SessionRegistry registry;
  const char* a = "0123456789abcdef0123456789abcdef";
  const char* b = "1123456789abcdef0123456789abcdef";
  const char* c = "2123456789abcdef0123456789abcdef";
  const char* d = "3123456789abcdef0123456789abcdef";
  const char* e = "4123456789abcdef0123456789abcdef";
  assert(!registry.add(0, a));
  assert(!registry.add(1, "short"));
  assert(!registry.add(1, "g123456789abcdef0123456789abcdef"));
  assert(registry.add(7, a));
  assert(registry.find(a) == 7 && registry.contains(7));
  assert(!registry.add(8, a));
  assert(!registry.add(7, b));
  assert(registry.add(8, b) && registry.add(9, c) && registry.add(10, d));
  assert(!registry.add(11, e));
  assert(registry.find(e) == 0 && registry.find(nullptr) == 0);
  registry.remove(7);
  assert(!registry.contains(7) && registry.find(a) == 0);
  registry.remove(7);
  assert(registry.find(b) == 8);
  assert(registry.add(7, e));
  assert(registry.find(a) == 0 && registry.find(e) == 7);
  std::puts("PASS: session binding / malformed credentials / collisions / capacity / revoke / reconnect");
}
