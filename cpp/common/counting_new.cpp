// Global operator new/delete overrides that count allocations. Linked only
// into benchmark binaries (see common/alloc_count.hpp).
#include <new>
#include <cstdlib>

#include "common/alloc_count.hpp"

void* operator new(std::size_t n) {
  hft::alloc_count().fetch_add(1, std::memory_order_relaxed);
  if (n == 0) ++n;
  void* p = std::malloc(n);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void* operator new[](std::size_t n) { return ::operator new(n); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
