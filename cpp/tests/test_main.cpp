#include "tests/test_framework.hpp"

int main() {
  int passed = 0;
  for (auto& [name, fn] : test_registry()) {
    std::printf("[ RUN ] %s\n", name);
    const int before = test_failures();
    fn();
    if (test_failures() == before) {
      ++passed;
      std::printf("[ OK  ] %s\n", name);
    } else {
      std::printf("[FAIL ] %s\n", name);
    }
  }
  std::printf("\n%d/%zu tests passed\n", passed, test_registry().size());
  return test_failures() == 0 ? 0 : 1;
}
