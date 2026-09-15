// Tiny assert-based test harness — zero dependencies, prints failures with
// file:line. Each TU adds cases via RUN_TEST and main() in test_main.cpp.
#pragma once

#include <cstdio>
#include <functional>
#include <vector>

inline std::vector<std::pair<const char*, std::function<void()>>>& test_registry() {
  static std::vector<std::pair<const char*, std::function<void()>>> reg;
  return reg;
}

inline int& test_failures() {
  static int failures = 0;
  return failures;
}

struct TestRegistrar {
  TestRegistrar(const char* name, std::function<void()> fn) {
    test_registry().emplace_back(name, std::move(fn));
  }
};

#define RUN_TEST(fn) static TestRegistrar reg_##fn(#fn, fn)

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ++test_failures();                                                     \
      std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    if (!((a) == (b))) {                                                     \
      ++test_failures();                                                     \
      std::printf("    FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__,    \
                  __LINE__, #a, #b, (long long)(a), (long long)(b));         \
    }                                                                        \
  } while (0)
