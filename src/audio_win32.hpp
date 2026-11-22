#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include "spu.hpp"
#include "audio_playback.hpp"
namespace ps1 {
class AudioOutput {
 static constexpr unsigned Frames=512,Blocks=8;
 HWAVEOUT device=nullptr;
 HANDLE event=nullptr;
 struct Block {WAVEHDR header{};std::array<int16_t,Frames*2> samples{};bool prepared=false;};
 std::array<Block,Blocks> blocks{};
 std::thread worker;
 std::atomic<bool> quit{false},muted{false},mmcss{false};
 std::atomic<MMRESULT> result{MMSYSERR_NOERROR};
 std::atomic<double> playback_ratio{1};
 std::atomic<uint64_t> starvations{0},host_drops{0};
   std::atomic<uint64_t> work_ns{0},machine_lock_wait_ns{0};
 std::atomic<unsigned> buffered{0};
 void run(Spu& spu,std::mutex& machine);
public:
 ~AudioOutput(){close();}
 bool open(Spu& spu,std::mutex& machine);
 void close();
 void set_muted(bool value){muted=value;}
 bool muted_state()const{return muted.load();}
 MMRESULT error()const{return result.load();}
 bool ready()const{return device!=nullptr&&result.load()==MMSYSERR_NOERROR;}
 double ratio()const{return playback_ratio.load();}
 uint64_t underruns()const{return starvations.load();}
 uint64_t dropped()const{return host_drops.load();}
   uint64_t processing_nanoseconds()const{return work_ns.load(std::memory_order_relaxed);}
 uint64_t machine_lock_wait_nanoseconds()const{return machine_lock_wait_ns.load(std::memory_order_relaxed);}
 bool multimedia_scheduled()const{return mmcss.load();}
 unsigned buffered_frames()const{return buffered.load();}
};
}
