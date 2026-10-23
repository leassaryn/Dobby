#include "dobby.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <sys/mman.h>
#include <cstring>
#include <unistd.h>

namespace {
using Function = int (*)(int);
using Clock = std::chrono::steady_clock;
__attribute__((noinline)) int Target(int x) {
  asm volatile("" ::: "memory");
  return (x ^ 0x1234) + 7;
}
Function original = nullptr;
__attribute__((noinline)) int Replacement(int x) {
  return original(x);
}
volatile uint64_t sink = 0;
uint64_t callbacks = 0;
void Instrument(RegisterContext *, const HookEntryInfo *) {
  ++callbacks;
}
void Check(int result) {
  if (result != RS_SUCCESS) {
    std::fprintf(stderr, "%s\n", DobbyGetErrorMessage(DobbyGetLastError()));
    std::exit(1);
  }
}
double Calls(size_t count) {
  std::array<double, 5> samples{};
  Function volatile call = Target;
  for (auto &sample : samples) {
    uint64_t sum = 0;
    const auto start = Clock::now();
    for (size_t i = 0; i < count; ++i)
      sum += static_cast<unsigned>(call(i & 0xffff));
    const auto end = Clock::now();
    sink = sum;
    sample = std::chrono::duration<double, std::nano>(end - start).count() / count;
  }
  std::sort(samples.begin(), samples.end());
  return samples[2];
}
} // namespace
int main() {
  constexpr size_t iterations = 200000, installs = 100;
  const double baseline = Calls(iterations);
  void *trampoline = nullptr;
  Check(DobbyPrepareEx(reinterpret_cast<void *>(Target), reinterpret_cast<void *>(Replacement), &trampoline, nullptr));
  original = reinterpret_cast<Function>(trampoline);
  Check(DobbyCommit(reinterpret_cast<void *>(Target)));
  const double inline_ns = Calls(iterations);
  Check(DobbyDestroy(reinterpret_cast<void *>(Target)));
  double instrument_ns = 0;
#ifdef DOBBY_BENCH_DBI
  Check(DobbyInstrument(reinterpret_cast<void *>(Target), Instrument));
  instrument_ns = Calls(iterations);
  Check(DobbyDestroy(reinterpret_cast<void *>(Target)));
  if (callbacks != iterations * 5)
    return 2;
#endif
  size_t relays = 0;
  const auto start = Clock::now();
  for (size_t i = 0; i < installs; ++i) {
    Check(
        DobbyPrepareEx(reinterpret_cast<void *>(Target), reinterpret_cast<void *>(Replacement), &trampoline, nullptr));
    original = reinterpret_cast<Function>(trampoline);
    Check(DobbyCommit(reinterpret_cast<void *>(Target)));
    DobbyHookInfo info{};
    info.struct_size = sizeof(info);
    Check(DobbyQueryHook(reinterpret_cast<void *>(Target), &info));
    relays += info.relay != nullptr;
    Check(DobbyDestroy(reinterpret_cast<void *>(Target)));
  }
  const double lifecycle_ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / installs;
  size_t near_attempts = 0, near_successes = 0;
#if defined(__aarch64__)
  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void *remote = mmap(nullptr, page_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (remote == MAP_FAILED)
    return 3;
  const uint32_t code[] = {0x528000e0, 0xd65f03c0};
  std::memcpy(remote, code, sizeof(code));
  __builtin___clear_cache(static_cast<char *>(remote), static_cast<char *>(remote) + sizeof(code));
  DobbyHookOptions options{sizeof(options), DOBBY_API_VERSION, DOBBY_NEAR_REQUIRED, 4};
  for (size_t i = 0; i < installs; ++i) {
    ++near_attempts;
    if (DobbyPrepareEx(remote, reinterpret_cast<void *>(Replacement), &trampoline, &options) == RS_SUCCESS) {
      DobbyHookInfo info{};
      info.struct_size = sizeof(info);
      Check(DobbyQueryHook(remote, &info));
      near_successes += info.patch_size == 4;
      relays += info.relay != nullptr;
      Check(DobbyDestroy(remote));
    }
  }
  munmap(remote, page_size);
#endif
  DobbyResourceStats stats{};
  stats.struct_size = sizeof(stats);
  Check(DobbyGetResourceStats(&stats));
  DobbyRuntimeInfo runtime{sizeof(runtime), DOBBY_API_VERSION};
  Check(DobbyGetRuntimeInfo(&runtime));
  Check(DobbyReleaseCodeMemory(nullptr));
  std::printf("{\"abi\":\"%s\",\"page_size\":%zu,\"iterations\":%zu,\"baseline_ns\":%.3f,"
              "\"inline_original_ns\":%.3f,\"instrument_ns\":%.3f,\"prepare_commit_query_destroy_ns\":%.3f,"
              "\"installs\":%zu,\"near_attempts\":%zu,\"near_successes\":%zu,\"relay_count\":%zu,\"retained_pool_bytes_"
              "before_release\":%zu}\n",
              runtime.architecture, runtime.page_size, iterations, baseline, inline_ns, instrument_ns, lifecycle_ns,
              installs, near_attempts, near_successes, relays, stats.retained_code_bytes);
}
