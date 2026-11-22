#pragma once
#include <chrono>
#include <cstdint>

namespace ps1 {
class ScopedGpuRasterTimer {
 uint64_t& total;
 std::chrono::steady_clock::time_point start;
public:
 explicit ScopedGpuRasterTimer(uint64_t& counter):total(counter),start(std::chrono::steady_clock::now()){}
 ~ScopedGpuRasterTimer(){total+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());}
};
}