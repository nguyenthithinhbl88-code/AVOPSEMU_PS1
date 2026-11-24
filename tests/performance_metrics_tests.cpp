#include "performance_metrics.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}

int main(){try{
 ps1::DisplayFrameMetrics metrics;
 auto summary=metrics.summary();
 check(summary.intervals==0&&summary.fps==0,"empty window has no FPS sample");
 constexpr uint64_t start=1000000000, budget=20000000;
 metrics.record_submission(start,budget);
 metrics.record_submission(start+10000000,budget);
 metrics.record_submission(start+30000000,budget);
 metrics.record_submission(start+60000000,budget);
 metrics.record_submission(start+100000000,budget);
 summary=metrics.summary();
 check(summary.intervals==4,"frame intervals are measured between submissions");
 check(std::abs(summary.average_ms-25.0)<1e-9,"average frame interval");
 check(std::abs(summary.p95_ms-40.0)<1e-9,"p95 frame interval");
 check(std::abs(summary.fps-40.0)<1e-9,"submitted frame rate");
 check(summary.late_frames==2,"late intervals use the supplied frame budget");
 metrics.reset();summary=metrics.summary();
 check(summary.intervals==0&&summary.late_frames==0,"reset clears the measurement window");
 std::cout<<"PASS: displayed-frame interval, p95, FPS, late-frame threshold and reset\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}