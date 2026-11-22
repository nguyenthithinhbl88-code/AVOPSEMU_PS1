#include "runner.hpp"
#include <chrono>
#include <stdexcept>
#ifdef _WIN32
#include "win_crash_diagnostics.hpp"
#endif
namespace ps1 {
namespace {
struct ElapsedCounter {
 std::atomic<uint64_t>& counter;
 std::chrono::steady_clock::time_point start;
 ElapsedCounter(std::atomic<uint64_t>& value,std::chrono::steady_clock::time_point begin):counter(value),start(begin){}
 ~ElapsedCounter(){counter.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count()),std::memory_order_relaxed);}
};
}
void Runner::stop(){quit=true;wake.notify_all();if(worker.joinable())worker.join();alive=false;}
void Runner::start(bool paced){
 stop();{std::lock_guard<std::mutex> lock(machine);failure.clear();}
 core_time_ns.store(0,std::memory_order_relaxed);machine_lock_wait_ns.store(0,std::memory_order_relaxed);
 quit=false;alive=true;
 try{worker=std::thread([this,paced]{
  using Clock=std::chrono::steady_clock;const auto start=Clock::now();uint64_t clocks=0;
  try{
   while(!quit){
     {const auto wait_start=Clock::now();std::unique_lock<std::mutex> lock(machine);const auto acquired=Clock::now();
      machine_lock_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(acquired-wait_start).count()),std::memory_order_relaxed);
      ElapsedCounter core_timer(core_time_ns,acquired);
      for(unsigned i=0;i<2048&&!quit;i++){cpu.step();clocks++;if(cpu.bios_system_error)throw std::runtime_error(cpu.failure_summary());}}
    std::this_thread::yield(); // allow UI/audio consumers to acquire the shared machine mutex
    if(paced){const auto target=start+std::chrono::nanoseconds(int64_t(double(clocks)*1e9/33868800.0));
     if(target-Clock::now()>std::chrono::milliseconds(2)){std::unique_lock<std::mutex> lock(wait_mutex);wake.wait_until(lock,target,[this]{return quit.load();});}
    }
   }
   }catch(const std::exception& e){
#ifdef _WIN32
    win_diag::write_event("Runner",e.what(),true);
#endif
    std::lock_guard<std::mutex> lock(machine);failure=e.what();
   }catch(...){
#ifdef _WIN32
    win_diag::write_event("Runner","Non-standard exception in CPU worker",true);
#endif
    std::lock_guard<std::mutex> lock(machine);failure="Non-standard exception in CPU worker";
   }
  alive=false;
  });}catch(const std::exception& e){
   alive=false;
#ifdef _WIN32
   win_diag::write_event("Runner",e.what(),true);
#endif
   throw;
  }catch(...){
   alive=false;
#ifdef _WIN32
   win_diag::write_event("Runner","Could not create CPU worker thread",true);
#endif
   throw;
  }
}
}
