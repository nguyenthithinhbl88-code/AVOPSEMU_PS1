#pragma once
#include <cstdint>

namespace ps1 {
struct CpuCrashContext {
	uint32_t pc = 0;
	uint32_t opcode = 0;
	bool valid = false;
};

inline thread_local CpuCrashContext cpu_crash_context{};

inline void record_cpu_crash_context(uint32_t pc, uint32_t opcode) noexcept {
	cpu_crash_context = {pc, opcode, true};
}

inline void clear_cpu_crash_context() noexcept {
	cpu_crash_context = {};
}

inline CpuCrashContext current_cpu_crash_context() noexcept {
	return cpu_crash_context;
}
}
