#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ps1 {
struct DisplayFrameSummary {
 uint64_t intervals=0,late_frames=0;
 double fps=0,average_ms=0,p95_ms=0;
};

class DisplayFrameMetrics {
 static constexpr size_t capacity=120;
 std::array<uint64_t,capacity> intervals{};
 size_t next=0,count=0;
 uint64_t last_submission_ns=0,late_frames=0;
public:
 void reset(){intervals.fill(0);next=count=0;last_submission_ns=late_frames=0;}
 void record_submission(uint64_t timestamp_ns,uint64_t frame_budget_ns){
  if(last_submission_ns&&timestamp_ns>last_submission_ns){
   const auto interval=timestamp_ns-last_submission_ns;
   intervals[next]=interval;next=(next+1)%capacity;count=std::min(count+1,capacity);
   if(frame_budget_ns&&interval>frame_budget_ns)late_frames++;
  }
  last_submission_ns=timestamp_ns;
 }
 DisplayFrameSummary summary()const{
  DisplayFrameSummary result;result.intervals=count;result.late_frames=late_frames;
  if(!count)return result;
  auto sorted=intervals;std::sort(sorted.begin(),sorted.begin()+count);
  uint64_t total=0;for(size_t i=0;i<count;i++)total+=sorted[i];
  result.average_ms=double(total)/double(count)/1000000.0;
  result.p95_ms=double(sorted[(count*95+99)/100-1])/1000000.0;
  result.fps=result.average_ms>0?1000.0/result.average_ms:0;
  return result;
 }
};
}