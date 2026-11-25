#pragma once
#include <array>
#include <chrono>
#include <cstdint>

namespace ps1 {
using CpuProfileClock=std::chrono::steady_clock;
struct CpuProfileCounters {
	uint64_t sampled_steps=0,step_ns=0,fetch_ns=0,trace_ns=0,decode_ns=0,execute_ns=0,device_tick_ns=0,bus_read_ns=0,bus_write_ns=0;
	uint64_t clock_pair_overhead_ns=0;
	uint64_t phase_timer_pairs=0,device_tick_calls=0,bus_access_calls=0,subsystem_timer_scopes=0;
	uint64_t sio_tick_ns=0,gpu_video_ns=0,irq_sample_ns=0,cdrom_tick_ns=0,dma_tick_ns=0,spu_tick_ns=0,timer_tick_ns=0;
 std::array<uint64_t,64> sampled_opcodes{};
	std::array<uint64_t,64> sampled_execute_ns{};
};

inline uint64_t cpu_profile_clock_pair_overhead_ns(){
	static const uint64_t overhead=[](){
		constexpr uint64_t iterations=20000;
		volatile uint64_t sink=0;
		const auto empty_begin=CpuProfileClock::now();for(uint64_t i=0;i<iterations;i++)sink+=i;const auto empty_end=CpuProfileClock::now();
		const auto timed_begin=CpuProfileClock::now();for(uint64_t i=0;i<iterations;i++){const auto first=CpuProfileClock::now();const auto second=CpuProfileClock::now();(void)first;(void)second;sink+=i;}const auto timed_end=CpuProfileClock::now();(void)sink;
		const auto empty_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(empty_end-empty_begin).count();
		const auto timed_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(timed_end-timed_begin).count();
		return timed_ns>empty_ns?uint64_t(timed_ns-empty_ns)/iterations:0;
	}();
	return overhead;
}

inline uint64_t cpu_profile_elapsed_ns(CpuProfileClock::time_point start,uint64_t overhead){
	const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(CpuProfileClock::now()-start).count());
	return elapsed>overhead?elapsed-overhead:0;
}

class CpuProfileStepTimer {
	CpuProfileCounters* counters;
	CpuProfileClock::time_point start;
public:
	explicit CpuProfileStepTimer(CpuProfileCounters* value):counters(value),start(value?CpuProfileClock::now():CpuProfileClock::time_point{}){}
	~CpuProfileStepTimer(){if(counters)counters->step_ns+=cpu_profile_elapsed_ns(start,counters->clock_pair_overhead_ns);}
};

#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
class CpuProfileSubsystemTimer {
	CpuProfileCounters* counters;
	uint64_t* total;
	CpuProfileClock::time_point start;
public:
	CpuProfileSubsystemTimer(CpuProfileCounters* profile,uint64_t* counter):counters(profile),total(counter),start(counter?CpuProfileClock::now():CpuProfileClock::time_point{}){if(counter)counters->subsystem_timer_scopes++;}
	~CpuProfileSubsystemTimer(){if(total)*total+=cpu_profile_elapsed_ns(start,counters->clock_pair_overhead_ns);}
};
#endif
}
