#pragma once
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
#include <array>
#include <chrono>
#include <cstdint>

namespace ps1 {
using CpuProfileClock=std::chrono::steady_clock;
struct CpuProfileCounters {
 uint64_t sampled_steps=0,fetch_ns=0,trace_ns=0,decode_ns=0,execute_ns=0;
 std::array<uint64_t,64> sampled_opcodes{};
};
inline thread_local CpuProfileCounters cpu_profile;
}
#endif
