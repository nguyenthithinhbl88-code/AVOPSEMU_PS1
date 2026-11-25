#include "core.hpp"
#include "cpu_crash_context.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>
namespace ps1 {
static std::runtime_error bus_error(const char* operation,uint32_t a){std::ostringstream s;s<<operation<<" @ 0x"<<std::hex<<std::setw(8)<<std::setfill('0')<<a;return std::runtime_error(s.str());}
static uint32_t physical(uint32_t a) { return a & 0x1fffffff; }
static uint32_t load_le32(const uint8_t* data){
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
 return uint32_t(data[0])|(uint32_t(data[1])<<8)|(uint32_t(data[2])<<16)|(uint32_t(data[3])<<24);
#else
 uint32_t value;std::memcpy(&value,data,sizeof(value));return value;
#endif
}
// Main RAM reads take 7 clocks including issue; Cpu::step already charges the issue clock.
static unsigned main_ram_read_wait_cycles(uint32_t a) { return physical(a)<0x800000?6:0; }
static bool timer_address(uint32_t a){a=physical(a);return a>=0x1f801100&&a<0x1f801130&&((a&15)==0||(a&15)==4||(a&15)==8);}
void Bus::sample_irq_lines(){
 uint16_t lines=(gpu.status&(1u<<24))?2:0;
 if(cd.irq())lines|=4;
 if(spu.irq)lines|=1u<<9;
 for(unsigned i=0;i<2;i++)if(sio[i].irq)lines|=uint16_t(1u<<(7+i));
 irq_status|=uint16_t(lines&~previous_irq_lines);previous_irq_lines=lines;
}
void Bus::tick_timers(CpuProfileCounters* profile){
#ifndef AVOPSEMU_ENABLE_CPU_PROFILE
 (void)profile;
#endif
 cd.clocks++;
 {
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  CpuProfileSubsystemTimer timer(profile,profile?&profile->sio_tick_ns:nullptr);
#endif
  for(auto& port:sio)if(port.tx_busy|bool(port.ack_delay)|bool(port.ack_width)|bool(port.irq_reassert)){port.context_pc=writer_cpu?writer_pc:0;port.tick(cd.clocks);}
 }
 {
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  CpuProfileSubsystemTimer timer(profile,profile?&profile->dma_tick_ns:nullptr);
#endif
  if(dma[2].chop_wait|dma[3].chop_wait|dma[4].chop_wait){
   if(dma[2].chop_wait)dma[2].chop_wait--;
   if(dma[3].chop_wait)dma[3].chop_wait--;
   if(dma[4].chop_wait)dma[4].chop_wait--;
  }
 }
 const unsigned sources=((timers[0].mode&0x100)?1:0)|(((timers[0].mode&1)||(timers[1].mode&0x100))?2:0);
 bool vblank=false;
 {
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  CpuProfileSubsystemTimer timer(profile,profile?&profile->gpu_video_ns:nullptr);
#endif
  vblank=gpu.tick_video(sources);
 }
 if(vblank){irq0_latch_requests++;if(!(irq_status&1))irq0_latch_edges++;irq_status|=1;}
 bool resample=false;
 {
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  CpuProfileSubsystemTimer timer(profile,profile?&profile->irq_sample_ns:nullptr);
#endif
  sample_irq_lines();
 }
 const bool cd_active=bool(cd.audio_start_delay)|bool(cd.irq_delay)|bool(cd.response_delay)|cd.busy|cd.reading|!cd.events.empty()|!cd.pending.empty();
 if(cd_active){
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  CpuProfileSubsystemTimer timer(profile,profile?&profile->cdrom_tick_ns:nullptr);
#endif
  cd.tick();resample=true;
 }
 const bool dma_active=bool(dma[3].control&(1u<<24))|bool(dma[0].control&(1u<<24))|bool(dma[1].control&(1u<<24))|((dma[2].control&0x1000100u)==0x1000100u)|((dma[4].control&0x1000100u)==0x1000100u);
 if(dma_active){
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  CpuProfileSubsystemTimer timer(profile,profile?&profile->dma_tick_ns:nullptr);
#endif
  if(dma[3].control&(1u<<24)){run_dma(3);resample=true;}
  if(dma[0].control&(1u<<24)){run_dma(0);resample=true;}
  if(dma[1].control&(1u<<24)){run_dma(1);resample=true;}
  if((dma[2].control&0x1000100u)==0x1000100u){run_dma(2);resample=true;}
  if((dma[4].control&0x1000100u)==0x1000100u){run_dma(4);resample=true;}
 }
 if(++spu.sample_clock==768){
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  CpuProfileSubsystemTimer timer(profile,profile?&profile->spu_tick_ns:nullptr);
#endif
  spu.sample_clock=0;const auto pcm=cd.audio_sample();spu.render_sample(pcm[0],pcm[1]);resample=true;
 }
 if(resample){
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  CpuProfileSubsystemTimer timer(profile,profile?&profile->irq_sample_ns:nullptr);
#endif
  sample_irq_lines();
 }
 {
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  CpuProfileSubsystemTimer timer(profile,profile?&profile->timer_tick_ns:nullptr);
#endif
  if(timers[0].tick(0,gpu.timer_dots,gpu.video_hblank,gpu.timer_hblank_rise))irq_status|=1u<<4;
  if(timers[1].tick(1,gpu.timer_hblank_rise?1:0,gpu.video_vblank,vblank))irq_status|=1u<<5;
  if(timers[2].tick(2))irq_status|=1u<<6;
 }
}

uint8_t Bus::read8(uint32_t a) const {
    const uint32_t fast=physical(a);
    if(fast<0x800000)return ram[fast&0x1fffff];
    if(fast>=0x1f800000&&fast<0x1f800400)return scratch[fast-0x1f800000];
    if(fast>=0x1fc00000&&fast<0x1fc80000)return bios[fast-0x1fc00000];
    if(physical(a)>=0x1f801040&&physical(a)<0x1f801060){auto& port=sio[(physical(a)-0x1f801040)/16];port.clocks=cd.clocks;port.context_pc=writer_cpu?writer_pc:0;return port.read(a&15);}
    if(physical(a)>=0x1f801800&&physical(a)<=0x1f801803)return cd.read(physical(a)-0x1f801800);
    // DMA registers are side-effect-free on reads at any supported CPU width.
    // Route each byte lane through the same register decoder used by LW.
    uint32_t dma_value=0;
    const auto p=physical(a);
    // Unlocked unused tail after timer 2 (psx-spx, Garbage Locations).
    // Stable zero is a compatibility policy; actual bus garbage is not modeled.
    if(p>=0x1f801130&&p<0x1f801140)return 0;
    if(p>=0x1f801080&&p<0x1f8010f8&&dma_read(p&~3u,dma_value))return uint8_t(dma_value>>((p&3)*8));
    if(p>=0x1f801c00&&p<0x1f801e80)return uint8_t(spu.read((p-0x1f801c00)&~1u)>>((p&1)*8));
    if(p==0x1f801814||p==0x1f801815||p==0x1f801816||p==0x1f801817)return uint8_t(gpu.status>>((p&3)*8));
    if(p>=0x1f801824&&p<0x1f801828)return uint8_t(mdec.status()>>((p&3)*8));
    uint8_t v=0;if(boot_io_read(a,v))return v;
    const uint32_t original=a;a=physical(a);
    if(a<0x800000) return ram[a&0x1fffff];
    if(a>=0x1f800000 && a<0x1f800400) return scratch[a-0x1f800000];
    if(a>=0x1fc00000 && a<0x1fc80000) return bios[a-0x1fc00000];
    throw bus_error("Unsupported bus read",original);
}
uint16_t Bus::read16(uint32_t a)const{
 const uint32_t fast=physical(a);
 if(!(a&1)&&fast<0x800000){unsigned i=fast&0x1fffff;return uint16_t(ram[i]|(uint16_t(ram[i+1])<<8));}
 if(!(a&1)&&fast>=0x1f800000&&fast<0x1f800400){unsigned i=fast-0x1f800000;return uint16_t(scratch[i]|(uint16_t(scratch[i+1])<<8));}
 if(!(a&1)&&fast>=0x1fc00000&&fast<0x1fc80000){unsigned i=fast-0x1fc00000;return uint16_t(bios[i]|(uint16_t(bios[i+1])<<8));}
 if(!(a&1)&&(fast==0x1f801040||fast==0x1f801050)){auto& port=sio[(fast-0x1f801040)/16];port.clocks=cd.clocks;port.context_pc=writer_cpu?writer_pc:0;return uint16_t(port.read_data(2));}
 if(timer_address(a))return timers[(physical(a)-0x1f801100)/16].read(a&15);
 if(a&1)throw bus_error("Unaligned halfword read",a);
 uint32_t p=physical(a);
 if(p>=0x1f801800&&p<=0x1f801802){const unsigned port=p-0x1f801800;const bool increment=memory_control[6]&0x2000;const uint16_t low=cd.read(port);return uint16_t(low|(uint16_t(cd.read(increment?port+1:port))<<8));}
 if(p>=0x1f801c00&&p<0x1f801e80)return spu.read(p-0x1f801c00);
 return uint16_t(read8(a)|(uint16_t(read8(a+1))<<8));
}
void Bus::write16(uint32_t a,uint16_t v){
 const uint32_t fast=physical(a);
 if(!(a&1)&&fast<0x800000){if(((fast&0x1fffff)^watched_byte)<2)watch_ram_write(a,v,2);unsigned i=fast&0x1fffff;ram[i]=uint8_t(v);ram[i+1]=uint8_t(v>>8);return;}
 if(!(a&1)&&fast>=0x1f800000&&fast<0x1f800400){unsigned i=fast-0x1f800000;scratch[i]=uint8_t(v);scratch[i+1]=uint8_t(v>>8);return;}
 if(timer_address(a)){timers[(physical(a)-0x1f801100)/16].write(a&15,v);return;}
 if(a&1)throw bus_error("Unaligned halfword write",a);
 uint32_t p=physical(a);
 if(dma_subword_write(a,v,2))return;
 if(p>=0x1f801800&&p<=0x1f801802){const unsigned port=p-0x1f801800;const bool increment=memory_control[6]&0x2000;cd.write(port,uint8_t(v));cd.write(increment?port+1:port,uint8_t(v>>8));return;}
 if(p>=0x1f801c00&&p<0x1f801e80){spu.write(p-0x1f801c00,v);return;}
 write8(a,uint8_t(v));write8(a+1,uint8_t(v>>8));
}
uint32_t Bus::read32(uint32_t a) const {
    if(!(a&3)&&(physical(a)==0x1f801040||physical(a)==0x1f801050)){auto& port=sio[(physical(a)-0x1f801040)/16];port.clocks=cd.clocks;port.context_pc=writer_cpu?writer_pc:0;return port.read_data(4);}
    // CPU instruction fetches and ordinary RAM loads avoid four MMIO decodes.
    const uint32_t p=physical(a);
     if(!(a&3)&&p<0x800000){const auto i=p&0x1fffff;return load_le32(ram.data()+i);}
     if(!(a&3)&&p>=0x1fc00000&&p<0x1fc80000){const auto i=p-0x1fc00000;return load_le32(bios.data()+i);}
    if(!(a&3)&&p==0x1f801800){uint32_t value=0;const bool increment=memory_control[6]&0x2000;for(unsigned i=0;i<4;i++)value|=uint32_t(cd.read(increment?i:0))<<(i*8);return value;}
    if(timer_address(a))return read16(a);
    if(!(a&3)&&physical(a)>=0x1f801c00&&physical(a)<0x1f801e80)return uint32_t(read16(a))|(uint32_t(read16(a+2))<<16);
    uint32_t dma_value=0;if(dma_read(physical(a),dma_value))return dma_value;
    if(p==0x1f801820)return mdec.read();
    if(p==0x1f801824)return mdec.status();
    if(physical(a)==0x1f801810)return gpu.read_gp0();
    if(physical(a)==0x1f801814)return gpu.status;
    if(a&3) throw std::runtime_error("Unaligned word read");
    uint32_t v=0; for(unsigned i=0;i<4;i++) v|=uint32_t(read8(a+i))<<(8*i); return v;
}
void Bus::write8(uint32_t a,uint8_t v) {
    const uint32_t fast=physical(a);
    if(fast>=0x1f801130&&fast<0x1f801140)return;
    if(fast<0x800000){if((fast&0x1fffff)==watched_byte)watch_ram_write(a,v,1);ram[fast&0x1fffff]=v;return;}
    if(fast>=0x1f800000&&fast<0x1f800400){scratch[fast-0x1f800000]=v;return;}
    if(physical(a)>=0x1f801040&&physical(a)<0x1f801060){auto& port=sio[(physical(a)-0x1f801040)/16];port.clocks=cd.clocks;port.context_pc=writer_cpu?writer_pc:0;port.write(a&15,v);return;}
    if(physical(a)>=0x1f801800&&physical(a)<=0x1f801803){cd.write(physical(a)-0x1f801800,v);return;}
    if(dma_subword_write(a,v,1))return;
    if(physical(a)>=0x1f801c00&&physical(a)<0x1f801e80){if(!(a&1))spu.write(physical(a)-0x1f801c00,v);return;}
    if(physical(a)>=0x1f801e80&&physical(a)<0x1f802000)return;
    if(boot_io_write(a,v))return;
    const uint32_t original=a;a=physical(a);
    if(a<0x800000) {ram[a&0x1fffff]=v;return;}
    if(a>=0x1f800000 && a<0x1f800400) {scratch[a-0x1f800000]=v;return;}
    throw bus_error("Unsupported/read-only bus write",original);
}
void Bus::write32(uint32_t a,uint32_t v) {
    const uint32_t p=physical(a);
    if(!(a&3)&&p<0x800000){if(((p&0x1fffff)^watched_byte)<4)watch_ram_write(a,v,4);const auto i=p&0x1fffff;for(unsigned byte=0;byte<4;byte++)ram[i+byte]=uint8_t(v>>(byte*8));return;}
    if(!(a&3)&&p==0x1f801800){const bool increment=memory_control[6]&0x2000;for(unsigned i=0;i<4;i++)cd.write(increment?i:0,uint8_t(v>>(i*8)));return;}
    if(timer_address(a)){write16(a,uint16_t(v));return;}
    if(!(a&3)&&physical(a)>=0x1f801c00&&physical(a)<0x1f801e80){write16(a,uint16_t(v));write16(a+2,uint16_t(v>>16));return;}
    if(p>=0x1f8010f8&&p<0x1f801100)return;
    if(p>=0x1f801e80&&p<0x1f802000)return;
    if(dma_write(physical(a),v))return;
    if(p==0x1f801820){mdec.write(v);return;}
    if(p==0x1f801824){mdec.write_control(v);return;}
    if(physical(a)==0x1f801810){gpu.gp0(v);return;}
    if(physical(a)==0x1f801814){gpu.gp1(v);return;}
    if(a&3) throw std::runtime_error("Unaligned word write");
    for(unsigned i=0;i<4;i++) write8(a+i,uint8_t(v>>(8*i)));
}
void Bus::load_bios(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f || f.tellg()!=std::streamoff(bios.size())) throw std::runtime_error("BIOS must be exactly 512 KiB");
    f.seekg(0); if(!f.read(reinterpret_cast<char*>(bios.data()),bios.size())) throw std::runtime_error("BIOS read failed");
}
static int64_t signed32(uint32_t v) { return v<0x80000000u ? int64_t(v) : int64_t(v)-0x100000000ll; }
static uint32_t sar(uint32_t v,unsigned n) {
    n&=31; if(!n) return v;
    return (v>>n) | ((v&0x80000000u) ? (~uint32_t(0)<<(32-n)) : 0);
}
void Cpu::reset(uint32_t entry) {
    clear_cpu_crash_context();
    gte=Gte{};first_fault=FaultSnapshot{};bios_system_error=false;
    bios_calls=0;last_bios_vector=last_bios_function=last_bios_return=0;last_bios_args.fill(0);
    instructions=interrupts=irq0_exceptions=reserved_exceptions=0;last_reserved_pc=last_reserved_opcode=0;trace.fill({});r.fill(0);cop0.fill(0);cop0[12]=0x00400000;cop0[15]=2;
    std::fill(instruction_trace_buffer.begin(),instruction_trace_buffer.end(),Trace{});instruction_trace_total=0;diagnostic_history.fill({});diagnostic_total=0;profile_counters=CpuProfileCounters{};
    if(profiling_active)profile_counters.clock_pair_overhead_ns=cpu_profile_clock_pair_overhead_ns();
    debug_break_pending=false;debug_break_reason=CpuDebugBreakReason::None;skip_pc_break_once=false;debug_break_pc=debug_break_opcode=debug_break_code=0;
    hi=lo=0;pc=entry;next_pc=entry+4;pending_reg=0;pending_value=0;delay_slot=false;branch_taken=false;
}
bool Cpu::configure_debug(const CpuDebugOptions& options,std::string& error){
    error.clear();
    if(options.profile_sample_interval<minimum_profile_sample_interval||options.profile_sample_interval>maximum_profile_sample_interval){error="CPU profile sample interval must be from 64 to 65536 instructions";return false;}
    if(options.trace_buffer_size<minimum_trace_buffer_size||options.trace_buffer_size>maximum_trace_buffer_size||(options.trace_buffer_size&(options.trace_buffer_size-1))){error="CPU trace buffer size must be a power of two from 128 to 16384 records";return false;}
    if(options.pc_breakpoint_enabled&&(options.pc_breakpoint_address&3)){error="CPU PC breakpoint address must be 4-byte aligned";return false;}
    if(instruction_trace_buffer.size()!=options.trace_buffer_size){
        std::vector<Trace> resized(options.trace_buffer_size);
        const auto retained=instruction_trace_records();
        const auto first=instruction_trace_total-retained;
        for(std::size_t i=0;i<retained;i++)resized[i]=instruction_trace_buffer[(first+i)%instruction_trace_buffer.size()];
        instruction_trace_buffer.swap(resized);instruction_trace_total=retained;
    }
    const bool profile_changed=debug_options.profiler_enabled!=options.profiler_enabled||debug_options.profile_sample_interval!=options.profile_sample_interval;
    const bool pc_breakpoint_changed=debug_options.pc_breakpoint_enabled!=options.pc_breakpoint_enabled||debug_options.pc_breakpoint_address!=options.pc_breakpoint_address;
    debug_options=options;
    profiling_active=options.profiler_enabled;instruction_trace_active=options.instruction_trace_enabled;pc_breakpoint_active=options.pc_breakpoint_enabled;
    debugger_tools_active=options.break_on_reserved_instruction||options.break_on_cpu_exception||options.pc_breakpoint_enabled;
    if(pc_breakpoint_changed)skip_pc_break_once=false;
    if(!debugger_tools_active){debug_break_pending=false;debug_break_reason=CpuDebugBreakReason::None;skip_pc_break_once=false;}
    if(profile_changed&&options.profiler_enabled){profile_counters=CpuProfileCounters{};profile_counters.clock_pair_overhead_ns=cpu_profile_clock_pair_overhead_ns();}
    return true;
}
std::size_t Cpu::instruction_trace_records()const noexcept{
    return static_cast<std::size_t>(std::min<uint64_t>(instruction_trace_total,instruction_trace_buffer.size()));
}
std::size_t Cpu::diagnostic_event_records()const noexcept{
    return static_cast<std::size_t>(std::min<uint64_t>(diagnostic_total,diagnostic_history.size()));
}
std::string Cpu::instruction_trace_export_text()const{
    std::ostringstream out;const auto count=instruction_trace_records();
    out<<"AVOPSEMU CPU instruction trace\nRecords retained: "<<count<<" / "<<instruction_trace_buffer.size()<<"\n";
    if(!count)out<<"No instruction records were captured.\n";
    out<<std::hex<<std::setfill('0');const uint64_t first=instruction_trace_total-count;
    for(uint64_t i=first;i<instruction_trace_total;i++){
        const auto& item=instruction_trace_buffer[i%instruction_trace_buffer.size()];
        out<<std::setw(8)<<item.pc<<": "<<std::setw(8)<<item.opcode<<" rs="<<std::setw(8)<<item.rs_value<<" rt="<<std::setw(8)<<item.rt_value<<" pending=r"<<std::dec<<item.load_reg<<":"<<std::hex<<std::setw(8)<<item.load_value<<"\n";
    }
    return out.str();
}
std::string Cpu::diagnostic_log_text()const{
    std::ostringstream out;const auto count=diagnostic_event_records();
    out<<"AVOPSEMU CPU diagnostics (exception and host-error events only)\n"
       <<"Logging: "<<(debug_options.diagnostic_logging?"Enabled":"Disabled")<<"; retained: "<<count<<" / "<<diagnostic_history.size()<<"\n"
       <<"Current instruction count: "<<instructions<<" PC=0x"<<std::hex<<std::setfill('0')<<std::setw(8)<<pc
       <<" Status=0x"<<std::setw(8)<<cop0[12]<<" Cause=0x"<<std::setw(8)<<cop0[13]<<" EPC=0x"<<std::setw(8)<<cop0[14]<<"\n";
    const uint64_t first=diagnostic_total-count;
    for(uint64_t i=first;i<diagnostic_total;i++){
        const auto& item=diagnostic_history[i%diagnostic_history.size()];
        if(item.kind==DiagnosticKind::HostError){
            out<<"ERROR instruction="<<std::dec<<item.instruction<<" PC=0x"<<std::hex<<std::setw(8)<<item.pc;
            if(item.has_opcode)out<<" opcode=0x"<<std::setw(8)<<item.opcode;
            out<<" Status=0x"<<std::setw(8)<<item.status<<" Cause=0x"<<std::setw(8)<<item.cause<<" EPC=0x"<<std::setw(8)<<item.epc<<" message=\""<<item.message<<"\"\n";
        }else{
            out<<"EXCEPTION instruction="<<std::dec<<item.instruction<<" code="<<unsigned(item.code)<<" PC=0x"<<std::hex<<std::setw(8)<<item.pc;
            if(item.has_opcode)out<<" opcode=0x"<<std::setw(8)<<item.opcode;
            out<<" StatusBefore=0x"<<std::setw(8)<<item.status<<" Cause=0x"<<std::setw(8)<<item.cause<<" EPC=0x"<<std::setw(8)<<item.epc<<" delay="<<std::dec<<item.in_delay<<"\n";
        }
    }
    return out.str();
}
std::string Cpu::debug_break_description()const{
    const char* reason="none";
    switch(debug_break_reason){case CpuDebugBreakReason::ProgramCounter:reason="PC breakpoint";break;case CpuDebugBreakReason::ReservedInstruction:reason="reserved instruction";break;case CpuDebugBreakReason::CpuException:reason="CPU exception";break;case CpuDebugBreakReason::None:break;}
    std::ostringstream out;out<<"CPU debugger break: "<<reason<<" at PC=0x"<<std::hex<<std::setfill('0')<<std::setw(8)<<debug_break_pc;
    if(debug_break_reason!=CpuDebugBreakReason::ProgramCounter)out<<" opcode=0x"<<std::setw(8)<<debug_break_opcode<<" exception="<<std::dec<<debug_break_code;
    return out.str();
}
void Cpu::resume_from_debug_break()noexcept{
    if(debug_break_pending&&debug_break_reason==CpuDebugBreakReason::ProgramCounter)skip_pc_break_once=true;
    debug_break_pending=false;debug_break_reason=CpuDebugBreakReason::None;
}
void Cpu::record_diagnostic_error(const char* message)noexcept{
    if(!debug_options.diagnostic_logging)return;
    auto& item=diagnostic_history[diagnostic_total++%diagnostic_history.size()];item=DiagnosticEvent{};item.kind=DiagnosticKind::HostError;item.instruction=instructions;
    const auto context=current_cpu_crash_context();item.pc=context.valid?context.pc:pc;item.opcode=context.valid?context.opcode:0;item.has_opcode=context.valid;
    item.status=cop0[12];item.cause=cop0[13];item.epc=cop0[14];if(message){std::strncpy(item.message,message,sizeof(item.message)-1);item.message[sizeof(item.message)-1]='\0';}
}
void Cpu::step() {
    struct WriterScope {Bus& b;~WriterScope(){b.writer_cpu=false;}} writer_scope{bus};
    bus.writer_cpu=false;
    const uint32_t ip=pc;
    uint32_t fetched_opcode=0;bool has_opcode=false;
    auto request_debug_break=[&](CpuDebugBreakReason reason,unsigned code){debug_break_pending=true;debug_break_reason=reason;debug_break_pc=ip;debug_break_opcode=has_opcode?fetched_opcode:0;debug_break_code=code;};
    if(debugger_tools_active){
        if(debug_break_pending)return;
        if(pc_breakpoint_active&&ip==debug_options.pc_breakpoint_address){
            if(skip_pc_break_once)skip_pc_break_once=false;
            else{request_debug_break(CpuDebugBreakReason::ProgramCounter,0);return;}
        }
    }
    const bool profile_sample=profiling_active&&debug_options.profile_sample_interval&&(instructions%debug_options.profile_sample_interval==0);
    CpuProfileStepTimer sampled_step(profile_sample?&profile_counters:nullptr);
    if(profile_sample)profile_counters.sampled_steps++;
    auto tick_devices=[&](){
        if(!profile_sample){bus.tick_timers();return;}
        profile_counters.device_tick_calls++;
        const auto start=CpuProfileClock::now();
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
        bus.tick_timers(&profile_counters);
#else
        bus.tick_timers();
#endif
        profile_counters.device_tick_ns+=cpu_profile_elapsed_ns(start,profile_counters.clock_pair_overhead_ns);
    };
    const bool in_delay=delay_slot,taken_delay=delay_slot&&branch_taken;
    const uint32_t delay_target=next_pc;
    auto exception=[&](unsigned code,unsigned coprocessor=0,bool debug=false) {
        const uint32_t previous_status=cop0[12];
        if(code==10){reserved_exceptions++;last_reserved_pc=ip;last_reserved_opcode=fetched_opcode;}
        cop0[13]=(cop0[13]&0x0000ff00u)|(code<<2)|(code==11?(coprocessor<<28):0)|(in_delay?0x80000000u:0)|(taken_delay?0x40000000u:0);
        cop0[14]=in_delay?ip-4:ip;if(taken_delay)cop0[6]=delay_target;
        cop0[12]=(cop0[12]&~0x3fu)|((cop0[12]<<2)&0x3f);
        if(code!=0&&code!=8&&!first_fault.valid){
            first_fault.valid=true;first_fault.has_opcode=has_opcode;first_fault.in_delay=in_delay;
            first_fault.code=code;first_fault.pc=ip;first_fault.opcode=fetched_opcode;
            first_fault.status=previous_status;first_fault.cause=cop0[13];first_fault.epc=cop0[14];first_fault.instruction=instructions;
            first_fault.registers=r;first_fault.trace=trace;first_fault.gte_data=gte.data;first_fault.gte_control=gte.control;
        }
        pc=(cop0[12]&0x400000)?(debug?0xbfc00140u:0xbfc00180u):(debug?0x80000040u:0x80000080u);next_pc=pc+4;delay_slot=false;branch_taken=false;
        if(debug_options.diagnostic_logging){
            auto& item=diagnostic_history[diagnostic_total++%diagnostic_history.size()];item=DiagnosticEvent{};item.kind=DiagnosticKind::Exception;item.instruction=instructions;item.pc=ip;item.opcode=fetched_opcode;item.has_opcode=has_opcode;item.status=previous_status;item.cause=cop0[13];item.epc=cop0[14];item.code=uint8_t(code);item.in_delay=in_delay;
        }
        if(code==10&&debug_options.break_on_reserved_instruction)request_debug_break(CpuDebugBreakReason::ReservedInstruction,code);
        else if(debug_options.break_on_cpu_exception)request_debug_break(CpuDebugBreakReason::CpuException,code);
    };
    cop0[13]=(cop0[13]&~0x400u)|((bus.irq_status&bus.irq_mask)?0x400u:0);
    if((cop0[12]&1)&&(cop0[12]&cop0[13]&0xff00)){
        if(pending_reg)r[pending_reg]=pending_value;
        const bool irq0=(bus.irq_status&bus.irq_mask&1)!=0;
        pending_reg=0;exception(0);tick_devices();interrupts++;if(irq0)irq0_exceptions++;return;
    }
    if((ip&3)||((cop0[12]&2)&&(ip&0x80000000u))){ if(pending_reg)r[pending_reg]=pending_value;pending_reg=0;cop0[8]=ip;exception(4);tick_devices();return; }
    const uint32_t dcic=cop0[7];
    const uint32_t debug_mode=(cop0[12]&2)?0x40000000u:0x20000000u;
    if((dcic&0x01800000u)==0x01800000u&&(dcic&debug_mode)&&!((ip^cop0[3])&cop0[11])){
        cop0[7]|=3;
        if(dcic&0x80000000u){if(pending_reg)r[pending_reg]=pending_value;pending_reg=0;exception(9,0,true);tick_devices();return;}
    }
    const uint32_t vector=physical(ip);
    if(vector>=0xa0&&vector<=0xc0&&(vector&15)==0){
        bios_calls++;last_bios_vector=vector;last_bios_function=r[9];last_bios_return=r[31];
        for(unsigned i=0;i<4;i++)last_bios_args[i]=r[4+i];
        if(vector==0xa0&&r[9]==0x40&&!bus.direct_exe_boot)bios_system_error=true;
        if(bus.direct_exe_boot&&bus.read32(vector)==0&&bus.read32(vector+4)==0&&bus.read32(vector+8)==0&&bus.read32(vector+12)==0){
            std::ostringstream message;message<<"Direct EXE called BIOS "<<std::hex<<vector<<"("<<r[9]<<"), but kernel RAM vector is empty. Nap BIOS -> Boot Game CD -> Run.";
            throw std::runtime_error(message.str());
        }
    }
    // Fetch RAM/BIOS directly after the architectural alignment/IRQ checks.
    // Other address regions retain the bus decoder and its explicit faults.
    auto word=[](const uint8_t* data){return uint32_t(data[0])|(uint32_t(data[1])<<8)|(uint32_t(data[2])<<16)|(uint32_t(data[3])<<24);};
    const auto fetch_start=profile_sample?CpuProfileClock::now():CpuProfileClock::time_point{};
    const uint32_t ins=vector<0x800000?load_le32(bus.ram.data()+(vector&0x1fffff)):
        vector>=0x1fc00000&&vector<0x1fc80000?load_le32(bus.bios.data()+vector-0x1fc00000):bus.read32(ip);
    if(profile_sample){profile_counters.phase_timer_pairs++;profile_counters.fetch_ns+=cpu_profile_elapsed_ns(fetch_start,profile_counters.clock_pair_overhead_ns);}
    const auto trace_start=profile_sample?CpuProfileClock::now():CpuProfileClock::time_point{};
    fetched_opcode=ins;has_opcode=true;record_cpu_crash_context(ip,ins);bus.writer_pc=ip;bus.writer_opcode=ins;bus.writer_instruction=instructions;bus.writer_cpu=true;
    const Trace trace_entry{ip,ins,r[(ins>>21)&31],r[(ins>>16)&31],pending_reg,pending_value};
    trace[instructions%trace.size()]=trace_entry;
    if(instruction_trace_active&&!instruction_trace_buffer.empty())instruction_trace_buffer[instruction_trace_total++%instruction_trace_buffer.size()]=trace_entry;
    instructions++;
    // Observe BIOS putchar entry without replacing or skipping its instructions.
    if(((ip&0x1fffffff)==0xa0 && r[9]==0x3c)||((ip&0x1fffffff)==0xb0 && r[9]==0x3d))bus.append_tty(char(r[4]&255));
    if(profile_sample){profile_counters.phase_timer_pairs++;profile_counters.trace_ns+=cpu_profile_elapsed_ns(trace_start,profile_counters.clock_pair_overhead_ns);}
    const auto decode_start=profile_sample?CpuProfileClock::now():CpuProfileClock::time_point{};
    const unsigned op=ins>>26,s=(ins>>21)&31,t=(ins>>16)&31,d=(ins>>11)&31,sh=(ins>>6)&31;
    const uint32_t a=r[s],b=r[t],imm=uint32_t(int32_t(int16_t(ins&65535)));
    if(profile_sample){profile_counters.phase_timer_pairs++;profile_counters.decode_ns+=cpu_profile_elapsed_ns(decode_start,profile_counters.clock_pair_overhead_ns);profile_counters.sampled_opcodes[op]++;}
    const unsigned old_pending=pending_reg;const uint32_t old_value=pending_value;
    if(pending_reg)r[pending_reg]=pending_value;
    pending_reg=0;pc=next_pc;next_pc=pc+4;delay_slot=false;branch_taken=false;
    auto put=[&](unsigned reg,uint32_t value){if(reg)r[reg]=value;};
    auto load=[&](uint32_t value){
        // A second load to the same destination cancels the older writeback.
        // Its operand reads still use the pre-load GPR values captured above.
        if(t&&old_pending==t)r[t]=b;
        pending_reg=t;pending_value=value;
    };
    auto branch=[&](bool take){delay_slot=true;branch_taken=take;if(take)next_pc=ip+4+(imm<<2);};
    auto arithmetic=[&](unsigned reg,int64_t value){if(value>2147483647ll||value< -2147483648ll)exception(12);else put(reg,uint32_t(value));};
    const uint32_t address=a+imm;
    auto bus_read8=[&](uint32_t target)->uint8_t{
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
        if(profile_sample){profile_counters.bus_access_calls++;const auto start=CpuProfileClock::now();const auto value=bus.read8(target);profile_counters.bus_read_ns+=cpu_profile_elapsed_ns(start,profile_counters.clock_pair_overhead_ns);return value;}
#endif
        return bus.read8(target);
    };
    auto bus_read16=[&](uint32_t target)->uint16_t{
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
        if(profile_sample){profile_counters.bus_access_calls++;const auto start=CpuProfileClock::now();const auto value=bus.read16(target);profile_counters.bus_read_ns+=cpu_profile_elapsed_ns(start,profile_counters.clock_pair_overhead_ns);return value;}
#endif
        return bus.read16(target);
    };
    auto bus_read32=[&](uint32_t target)->uint32_t{
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
        if(profile_sample){profile_counters.bus_access_calls++;const auto start=CpuProfileClock::now();const auto value=bus.read32(target);profile_counters.bus_read_ns+=cpu_profile_elapsed_ns(start,profile_counters.clock_pair_overhead_ns);return value;}
#endif
        return bus.read32(target);
    };
    auto bus_write8=[&](uint32_t target,uint8_t value){
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
        if(profile_sample){profile_counters.bus_access_calls++;const auto start=CpuProfileClock::now();bus.write8(target,value);profile_counters.bus_write_ns+=cpu_profile_elapsed_ns(start,profile_counters.clock_pair_overhead_ns);return;}
#endif
        bus.write8(target,value);
    };
    auto bus_write16=[&](uint32_t target,uint16_t value){
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
        if(profile_sample){profile_counters.bus_access_calls++;const auto start=CpuProfileClock::now();bus.write16(target,value);profile_counters.bus_write_ns+=cpu_profile_elapsed_ns(start,profile_counters.clock_pair_overhead_ns);return;}
#endif
        bus.write16(target,value);
    };
    auto bus_write32=[&](uint32_t target,uint32_t value){
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
        if(profile_sample){profile_counters.bus_access_calls++;const auto start=CpuProfileClock::now();bus.write32(target,value);profile_counters.bus_write_ns+=cpu_profile_elapsed_ns(start,profile_counters.clock_pair_overhead_ns);return;}
#endif
        bus.write32(target,value);
    };
    auto dma_subword_write=[&](uint32_t target,uint32_t value,unsigned width){
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
        if(profile_sample){profile_counters.bus_access_calls++;const auto start=CpuProfileClock::now();const bool handled=bus.dma_subword_write(target,value,width);profile_counters.bus_write_ns+=cpu_profile_elapsed_ns(start,profile_counters.clock_pair_overhead_ns);return handled;}
#endif
        return bus.dma_subword_write(target,value,width);
    };
    auto spu_register_write=[&](uint32_t target,uint16_t value){
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
        if(profile_sample){profile_counters.bus_access_calls++;const auto start=CpuProfileClock::now();bus.spu.write(target,value);profile_counters.bus_write_ns+=cpu_profile_elapsed_ns(start,profile_counters.clock_pair_overhead_ns);return;}
#endif
        bus.spu.write(target,value);
    };
    auto store=[&](unsigned width){
        const auto p=physical(address);
        if(dma_subword_write(address,b,width))return;
        if(width==1&&p>=0x1f801c00&&p<0x1f801e80){if(!(p&1))spu_register_write(p-0x1f801c00,uint16_t(b));return;}
        if(p==0x1f801820||p==0x1f801824){bus_write32(address,b);return;}
        if(width==1)bus_write8(address,uint8_t(b));else bus_write16(address,uint16_t(b));
    };
    auto aligned=[&](unsigned mask,bool store){if(address&mask){cop0[8]=address;exception(store?5:4);return false;}return true;};
    auto half=[&](){return uint32_t(bus_read16(address));};
    unsigned extra_cycles=0;
    // KUc applies even to byte/unaligned transfers and isolated-cache stores.
    const bool memory_load=(op>=32&&op<=38)||op==50;
    const bool memory_store=(op>=40&&op<=43)||op==46||op==58;
    const auto execute_start=profile_sample?CpuProfileClock::now():CpuProfileClock::time_point{};
    if((cop0[12]&2)&&(address&0x80000000u)&&(memory_load||memory_store)){
        // COP2 permission is checked before its memory address.
        if((op==50||op==58)&&!(cop0[12]&(1u<<30)))exception(11,2);
        else{cop0[8]=address;exception(memory_store?5:4);}
        r[0]=0;tick_devices();if(profile_sample){profile_counters.phase_timer_pairs++;const auto elapsed=cpu_profile_elapsed_ns(execute_start,profile_counters.clock_pair_overhead_ns);profile_counters.execute_ns+=elapsed;profile_counters.sampled_execute_ns[op]+=elapsed;}return;
    }
    {
    switch(op) {
    case 0:
        switch(ins&63) {
        case 0:put(d,b<<sh);break;case 2:put(d,b>>sh);break;case 3:put(d,sar(b,sh));break;
        case 4:put(d,b<<(a&31));break;case 6:put(d,b>>(a&31));break;case 7:put(d,sar(b,a));break;
        case 8:delay_slot=true;branch_taken=true;next_pc=a;break;
        case 9:put(d,ip+8);delay_slot=true;branch_taken=true;next_pc=a;break;
        case 12:exception(8);break;case 13:exception(9);break;
        case 16:put(d,hi);break;case 17:hi=a;break;case 18:put(d,lo);break;case 19:lo=a;break;
        case 24:{uint64_t v=uint64_t(signed32(a)*signed32(b));lo=uint32_t(v);hi=uint32_t(v>>32);break;}
        case 25:{uint64_t v=uint64_t(a)*b;lo=uint32_t(v);hi=uint32_t(v>>32);break;}
        case 26:if(!b){lo=signed32(a)<0?1:0xffffffff;hi=a;}else{lo=uint32_t(signed32(a)/signed32(b));hi=uint32_t(signed32(a)%signed32(b));}break;
        case 27:if(!b){lo=0xffffffff;hi=a;}else{lo=a/b;hi=a%b;}break;
        case 32:arithmetic(d,signed32(a)+signed32(b));break;
        case 33:put(d,a+b);break;case 34:arithmetic(d,signed32(a)-signed32(b));break;case 35:put(d,a-b);break;
        case 36:put(d,a&b);break;case 37:put(d,a|b);break;case 38:put(d,a^b);break;case 39:put(d,~(a|b));break;
        case 42:put(d,signed32(a)<signed32(b));break;case 43:put(d,a<b);break;
        default:exception(10);break;
        }break;
    case 1:
        if(t==0||t==1||t==16||t==17){if(t&16)put(31,ip+8);branch((t&1)?signed32(a)>=0:signed32(a)<0);}else exception(10);break;
    case 2:case 3:if(op==3)put(31,ip+8);delay_slot=true;branch_taken=true;next_pc=((ip+4)&0xf0000000)|((ins&0x3ffffff)<<2);break;
    case 4:branch(a==b);break;case 5:branch(a!=b);break;
    case 6:branch(signed32(a)<=0);break;case 7:branch(signed32(a)>0);break;
    case 8:arithmetic(t,signed32(a)+signed32(imm));break;case 9:put(t,a+imm);break;
    case 10:put(t,signed32(a)<signed32(imm));break;case 11:put(t,a<imm);break;
    case 12:put(t,a&(ins&65535));break;case 13:put(t,a|(ins&65535));break;case 14:put(t,a^(ins&65535));break;case 15:put(t,(ins&65535)<<16);break;
    case 16:
        if(s==8&&(t<=3)){
            if(!(cop0[12]&(1u<<28)))exception(11,0);
            else branch((t&1)==0); // PS1 COP0 condition is false; rt bit 1 is ignored.
            break;
        }
        if((cop0[12]&2)&&!(cop0[12]&(1u<<28))&&!(s==0&&d>=16)){exception(11,0);break;}
        if(s==0){if(d==0||d==1||d==2||d==4||d==10)exception(10);else load(cop0[d]);}
        else if(s==4){
            if(d==12||d==14||d==3||d==5||d==9||d==11)cop0[d]=b;
            else if(d==7)cop0[7]=b&0xff80f03fu;
            else if(d==13)cop0[13]=(cop0[13]&~0x300u)|(b&0x300);
        }else if(s==16&&(ins&63)==16)cop0[12]=(cop0[12]&~15u)|((cop0[12]>>2)&15);
        else exception(10);
        break;
    case 18:
        if(!(cop0[12]&(1u<<30))){exception(11,2);break;}
        if(s==0)load(gte.read_data(d));
        else if(s==2)load(gte.read_control(d));
        else if(s==4)gte.write_data(d,b);
        else if(s==6)gte.write_control(d,b);
        else if(s==8&&t<=3)branch((t&1)==0); // BC2F always; BC2T never; rt bit 1 is ignored.
        else if(s>=16){if(!gte.execute(ins))exception(10);}
        else exception(10);
        break;
    case 50:
        if(!(cop0[12]&(1u<<30)))exception(11,2);
        else if(aligned(3,false)){extra_cycles=main_ram_read_wait_cycles(address);gte.write_data(t,bus_read32(address));}
        break;
    case 58:
        if(!(cop0[12]&(1u<<30)))exception(11,2);
        else if(aligned(3,true)&&!(cop0[12]&0x10000))bus_write32(address,gte.read_data(t));
        break;
    case 48:case 56:exception(11,0);break;
    case 17:case 19:case 49:case 51:case 57:case 59:exception(11,op&3);break;
    case 32:extra_cycles=main_ram_read_wait_cycles(address);load(uint32_t(int32_t(int8_t(bus_read8(address)))));break;
    case 33:if(aligned(1,false)){extra_cycles=main_ram_read_wait_cycles(address);load(uint32_t(int32_t(int16_t(half()))));}break;
    case 35:if(aligned(3,false)){extra_cycles=main_ram_read_wait_cycles(address);load(bus_read32(address));}break;
    case 36:extra_cycles=main_ram_read_wait_cycles(address);load(bus_read8(address));break;
    case 37:if(aligned(1,false)){extra_cycles=main_ram_read_wait_cycles(address);load(half());}break;
    case 34:case 38:{
        extra_cycles=main_ram_read_wait_cycles(address);
        const uint32_t word=bus_read32(address&~3u),previous=(old_pending==t&&t)?old_value:b;
        const unsigned n=address&3;
        if(op==34){unsigned shift=(3-n)*8;load((previous&(0x00ffffffu>>(n*8)))|(word<<shift));}
        else {unsigned shift=n*8;uint32_t mask=n?0xffffffffu<<(32-shift):0;load((previous&mask)|(word>>shift));}break;}
    case 40:if(!(cop0[12]&0x10000))store(1);break;
    case 41:if(aligned(1,true)&&!(cop0[12]&0x10000)){store(2);}break;
    case 43:if(aligned(3,true)&&!(cop0[12]&0x10000))bus_write32(address,b);break;
    case 42:case 46:if(!(cop0[12]&0x10000)){
        uint32_t word=bus_read32(address&~3u);unsigned n=address&3;
        if(op==42){unsigned shift=(3-n)*8;uint32_t mask=n==3?0:0xffffffffu<<((n+1)*8);word=(word&mask)|(b>>shift);}
        else {unsigned shift=n*8;uint32_t mask=n?(1u<<shift)-1:0;word=(word&mask)|(b<<shift);}
        bus_write32(address&~3u,word);
    }break;
    default:exception(10);break;
    }
    }
    if(profile_sample){profile_counters.phase_timer_pairs++;const auto elapsed=cpu_profile_elapsed_ns(execute_start,profile_counters.clock_pair_overhead_ns);profile_counters.execute_ns+=elapsed;profile_counters.sampled_execute_ns[op]+=elapsed;}
    r[0]=0;tick_devices();for(unsigned cycle=0;cycle<extra_cycles;cycle++)tick_devices();
}
std::string Cpu::failure_summary()const{
 std::ostringstream out;out<<"BIOS SystemError A0(40)";
 if(first_fault.valid)out<<" after CPU exception "<<std::dec<<first_fault.code<<" at 0x"<<std::hex<<first_fault.pc<<" opcode=0x"<<first_fault.opcode;
 out<<". Save log includes the first fault snapshot.";return out.str();
}
std::string Cpu::diagnostic()const{
 std::ostringstream out;out<<"AVOPSEMU PS1 v51 diagnostic (SIO RX FIFO + pad protocol trace)\nInstructions: "<<instructions<<"\n";
 out<<std::hex<<std::setfill('0');
 out<<"PC="<<std::setw(8)<<pc<<" Next="<<std::setw(8)<<next_pc<<" SR="<<std::setw(8)<<cop0[12]<<" Cause="<<std::setw(8)<<cop0[13]<<" EPC="<<std::setw(8)<<cop0[14]<<"\n";
 if(first_fault.valid){
  out<<"FIRST CPU FAULT code="<<std::dec<<first_fault.code<<" instruction="<<first_fault.instruction<<" PC="<<std::hex<<first_fault.pc<<" opcode="<<first_fault.opcode<<" fetched="<<first_fault.has_opcode<<" delay="<<first_fault.in_delay<<" SR="<<first_fault.status<<" Cause="<<first_fault.cause<<" EPC="<<first_fault.epc<<"\n";
  for(unsigned i=0;i<32;i++)out<<"F_R"<<std::dec<<i<<"="<<std::hex<<std::setw(8)<<first_fault.registers[i]<<((i%4==3)?"\n":" ");
  out<<"GTE registers at first fault:\n";for(unsigned i=0;i<32;i++)out<<"D"<<std::dec<<i<<"="<<std::hex<<first_fault.gte_data[i]<<" C"<<std::dec<<i<<"="<<std::hex<<first_fault.gte_control[i]<<"\n";
  out<<"Trace saved at first fault (oldest first):\n";
  const uint64_t first=first_fault.instruction>trace.size()?first_fault.instruction-trace.size():0;
  for(uint64_t i=first;i<first_fault.instruction;i++){const auto& item=first_fault.trace[i%trace.size()];out<<std::hex<<std::setw(8)<<item.pc<<": "<<std::setw(8)<<item.opcode<<"\n";}
 }
 out<<"GTE commands="<<std::dec<<gte.commands<<" last="<<std::hex<<gte.last_command<<" FLAG="<<gte.read_control(31)<<" BIOS_SystemError="<<bios_system_error<<"\n";
 out<<"Boot mode="<<(bus.direct_exe_boot?"direct-exe (kernel not initialized by loader)":"machine/BIOS path")<<" BIOS reset word="<<bus.read32(0xbfc00000)<<"\n";
 out<<"BIOS calls="<<std::dec<<bios_calls<<" last_vector="<<std::hex<<last_bios_vector<<" function="<<last_bios_function<<" return="<<last_bios_return<<" args=";for(auto value:last_bios_args)out<<value<<" ";out<<"\n";
 out<<"RAM vectors A0/B0/C0:";for(unsigned vector=0xa0;vector<=0xc0;vector+=16){out<<" ["<<std::hex<<vector<<"]";for(unsigned i=0;i<16;i+=4)out<<" "<<bus.read32(vector+i);}out<<"\n";
 out<<"POST="<<unsigned(bus.post)<<" RAM control="<<bus.ram_control<<" Cache control="<<bus.cache_control<<"\n";
 for(unsigned i=0;i<32;i++)out<<"R"<<std::dec<<i<<"="<<std::hex<<std::setw(8)<<r[i]<<((i%4==3)?"\n":" ");
 out<<"Recent fetched instructions (oldest first):\n";
 uint64_t start=instructions>trace.size()?instructions-trace.size():0;
 for(uint64_t i=start;i<instructions;i++){const auto& t=trace[i%trace.size()];out<<std::setw(8)<<t.pc<<": "<<std::setw(8)<<t.opcode<<" rs="<<t.rs_value<<" rt="<<t.rt_value<<" pending="<<t.load_reg<<":"<<t.load_value<<"\n";}
 out<<bus.watch_diagnostic();
 for(unsigned i=0;i<3;i++)out<<"Timer"<<i<<" count="<<bus.timers[i].count<<" mode="<<bus.timers[i].mode<<" target="<<bus.timers[i].target<<"\n";
 out<<"CPU IRQ exceptions="<<std::dec<<interrupts<<" reserved_exceptions="<<reserved_exceptions<<" last_reserved_pc="<<std::hex<<last_reserved_pc<<" last_reserved_opcode="<<last_reserved_opcode<<"\n";
 out<<"IRQ0 VBlank events="<<std::dec<<bus.gpu.vblank_events<<" latch_requests="<<bus.irq0_latch_requests<<" latch_edges="<<bus.irq0_latch_edges<<" ack_writes="<<bus.irq0_ack_writes<<" ack_clears="<<bus.irq0_ack_clears<<" CPU_exceptions="<<irq0_exceptions<<"\n";
 out<<"DPCR="<<std::hex<<bus.dpcr<<" DICR="<<bus.dicr<<" I_STAT="<<bus.irq_status<<" I_MASK="<<bus.irq_mask<<"\n";
 for(unsigned ch=0;ch<7;ch++){const auto& d=bus.dma[ch];if(d.chop_bursts||d.chop_wait)out<<"DMA chopping channel="<<ch<<" control="<<std::hex<<d.control<<" address="<<d.address<<" block="<<d.block<<" remaining="<<std::dec<<d.remaining<<" cpu_wait="<<d.chop_wait<<" bursts="<<d.chop_bursts<<"\n";}
 out<<"MDEC status="<<std::hex<<bus.mdec.status()<<" control="<<bus.mdec.control<<" command="<<bus.mdec.command<<" remaining="<<std::dec<<bus.mdec.remaining<<" input_halfwords="<<bus.mdec.input.size()<<" output_words="<<bus.mdec.output.size()<<" commands="<<bus.mdec.commands<<" decoded_macroblocks="<<bus.mdec.macroblocks<<" words_in="<<bus.mdec.words_in<<" words_out="<<bus.mdec.words_out<<" resets="<<bus.mdec.resets<<"\n";
 for(unsigned i=0;i<2;i++){const auto& d=bus.dma[i];out<<"MDEC DMA"<<i<<" address="<<std::hex<<d.address<<" block="<<d.block<<" control="<<d.control<<" remaining="<<std::dec<<d.remaining<<"\n";}
 out<<"CD sectors="<<std::dec<<bus.cd.sectors<<" lba="<<bus.cd.lba<<" target="<<bus.cd.target_lba<<" setloc_pending="<<bus.cd.setloc_pending<<" commands="<<bus.cd.commands<<" sectors_read="<<bus.cd.sectors_read<<" irq="<<unsigned(bus.cd.flag)<<"\n";
 out<<"Disc layout: "<<bus.cd.disc.description()<<"\n";
 out<<"CD mode="<<std::hex<<unsigned(bus.cd.mode)<<" last_command="<<unsigned(bus.cd.last_command)<<" error="<<unsigned(bus.cd.last_error)<<" data_fifo="<<std::dec<<bus.cd.data.size()<<" pending="<<bus.cd.pending.size()<<" region_emulated="<<bus.cd.effective_region()<<" boot="<<bus.cd.boot_name<<"\n";
 out<<"CD audio: playing="<<bus.cd.playing<<" track="<<bus.cd.playing_track<<" sectors="<<std::dec<<bus.cd.cdda_sectors<<" frames="<<bus.cd.cdda_frames<<" muted="<<bus.cd.muted<<" ATV=";for(auto v:bus.cd.active_volume)out<<std::hex<<unsigned(v)<<" ";out<<"\n";
 out<<"XA audio: sectors="<<std::dec<<bus.cd.xa.sectors<<" frames="<<bus.cd.xa.frames<<" queued="<<bus.cd.xa.pcm.size()<<" dropped="<<bus.cd.xa.dropped<<" invalid_coding="<<bus.cd.xa.invalid<<"\n";
 out<<"SPU frames="<<std::dec<<bus.spu.generated_frames<<" queued_frames="<<bus.spu.audio.size()/2<<" dropped_frames="<<bus.spu.dropped_frames<<" active_voices="<<bus.spu.active_voices()<<" irq9="<<bus.spu.irq<<" capture="<<std::hex<<bus.spu.capture_position<<" CNT="<<bus.spu.read(0x1aa)<<" STAT="<<bus.spu.read(0x1ae)<<"\n";
 for(unsigned i=0;i<24;i++){const auto& v=bus.spu.voices[i];if(v.phase!=Spu::Voice::Off)out<<"SPU voice"<<std::dec<<i<<" phase="<<v.phase<<" level="<<v.level<<" address="<<std::hex<<v.address<<" pitch="<<bus.spu.regs[i*8+2]<<"\n";}
 out<<"CD controller: index="<<std::dec<<unsigned(bus.cd.index)<<" enable="<<std::hex<<unsigned(bus.cd.enable)<<" flag="<<unsigned(bus.cd.flag)<<" busy="<<bus.cd.busy<<" status="<<unsigned(bus.cd.status())<<" irq_ack_delay="<<std::dec<<bus.cd.irq_delay<<" sector_buffer="<<std::dec<<bus.cd.sector_buffer.size()<<" sector_pending="<<bus.cd.sector_pending<<" sector_delay="<<bus.cd.sector_delay<<" response_delay="<<bus.cd.response_delay<<" response_fifo="<<std::dec<<bus.cd.response.size()<<" parameter_fifo="<<bus.cd.params.size()<<" queued_events="<<bus.cd.events.size()<<" queued_commands="<<bus.cd.pending.size()<<" DEV5="<<std::hex<<bus.memory_control[6]<<"\n";
 out<<"CD protocol history (oldest first):\n";
 const uint64_t first_cd=bus.cd.transactions>bus.cd.history.size()?bus.cd.transactions-bus.cd.history.size():0;
 for(uint64_t i=first_cd;i<bus.cd.transactions;i++){
  const auto& item=bus.cd.history[i%bus.cd.history.size()];out<<std::dec<<i<<" clock="<<item.clock<<" "<<item.event<<" value="<<std::hex<<unsigned(item.value)<<" index="<<unsigned(item.index)<<" irq="<<unsigned(item.flag)<<" enable="<<unsigned(item.enable)<<" status="<<unsigned(item.status)<<" lba="<<std::dec<<item.lba<<" bytes="<<std::hex;
  for(unsigned j=0;j<item.size;j++)out<<unsigned(item.bytes[j])<<" ";
  out<<"\n";
 }
 out<<"CD DMA history (oldest first, checksum=FNV1a):\n";
 const uint64_t first_dma=bus.cd_dma_transfers>bus.cd_dma_history.size()?bus.cd_dma_transfers-bus.cd_dma_history.size():0;
 for(uint64_t i=first_dma;i<bus.cd_dma_transfers;i++){
  const auto& d=bus.cd_dma_history[i%bus.cd_dma_history.size()];out<<std::dec<<i<<" clock="<<d.clock<<" lba="<<d.lba<<" address="<<std::hex<<d.address<<" words="<<std::dec<<d.words<<" checksum="<<std::hex<<d.checksum<<" first_words=";
  for(auto word:d.sample){out<<word<<" ";}out<<"\n";
 }
 auto ram_window=[&](uint32_t address){
  const uint32_t base=physical(address)&~15u;if(base>=0x800000)return;
  out<<"RAM window "<<std::hex<<address<<" (live RAM, no MMIO):\n";
  for(unsigned offset=0;offset<128;offset+=16){const auto at=base+offset;if(at>=0x800000)break;out<<std::setw(8)<<at<<":";for(unsigned lane=0;lane<16;lane+=4)out<<" "<<std::setw(8)<<bus.read32(at+lane);out<<"\n";}
 };
  ram_window(0x10000);if((physical(r[4])&~15u)!=0x10000)ram_window(r[4]);
 out<<"Pad connected="<<bus.sio[0].connected<<" buttons="<<std::hex<<bus.sio[0].buttons<<" phase="<<std::dec<<bus.sio[0].phase<<" polls="<<bus.sio[0].polls<<" ACK_events="<<bus.sio[0].ack_events<<" ACK_delay="<<bus.sio[0].ack_delay<<" ACK_level="<<bus.sio[0].ack<<"\n";
 out<<"Pad protocol transfers="<<std::dec<<bus.sio[0].transfers<<" complete="<<bus.sio[0].completed_polls<<" pressed="<<bus.sio[0].pressed_polls<<" latched="<<std::hex<<bus.sio[0].latched_buttons<<" last_pressed="<<bus.sio[0].last_pressed_buttons<<" CTRL="<<bus.sio[0].control<<" MODE="<<bus.sio[0].mode<<" BAUD="<<bus.sio[0].baud<<" IRQ="<<bus.sio[0].irq<<" last_TX="<<unsigned(bus.sio[0].last_tx)<<"\n";
 out<<bus.sio[0].diagnostic();
 out<<"GPUSTAT="<<std::hex<<bus.gpu.status<<" DrawMode="<<bus.gpu.draw_mode<<" TextureWindow="<<bus.gpu.texture_window<<" PacketWords="<<std::dec<<bus.gpu.count<<"\n";
 out<<"GPU words="<<bus.gpu.words<<" polygons="<<bus.gpu.polygons<<" rectangles="<<bus.gpu.rectangles<<" lines="<<bus.gpu.line_segments<<"\n";
  out<<"Crash trace capacity="<<trace.size()<<" (always enabled); optional trace enabled="<<debug_options.instruction_trace_enabled<<" retained="<<std::dec<<instruction_trace_records()<<" capacity="<<instruction_trace_buffer.size()<<"\n";
  out<<"CPU_PROFILE enabled="<<debug_options.profiler_enabled<<" sample_every="<<debug_options.profile_sample_interval<<" sampled_steps="<<profile_counters.sampled_steps
     <<" sampled_host_ns(step/fetch/trace/decode/execute/device_tick)="<<profile_counters.step_ns<<"/"<<profile_counters.fetch_ns<<"/"<<profile_counters.trace_ns<<"/"<<profile_counters.decode_ns<<"/"<<profile_counters.execute_ns<<"/"<<profile_counters.device_tick_ns
     <<" bus_read_write_ns="<<profile_counters.bus_read_ns<<"/"<<profile_counters.bus_write_ns<<" clock_pair_overhead_ns="<<profile_counters.clock_pair_overhead_ns
     <<" timing_scopes(phase/device_tick/bus/subsystem)="<<profile_counters.phase_timer_pairs<<"/"<<profile_counters.device_tick_calls<<"/"<<profile_counters.bus_access_calls<<"/"<<profile_counters.subsystem_timer_scopes<<" opcode_samples_ns=";
  for(unsigned opcode=0;opcode<profile_counters.sampled_opcodes.size();opcode++)if(profile_counters.sampled_opcodes[opcode])out<<std::hex<<opcode<<":"<<std::dec<<profile_counters.sampled_opcodes[opcode]<<"/"<<profile_counters.sampled_execute_ns[opcode]<<" ";
#ifdef AVOPSEMU_ENABLE_CPU_PROFILE
  out<<"\nCPU_PROFILE subsystem_sampled_host_ns(sio/gpu_video/irq/cdrom/dma/spu/timers)="<<std::dec<<profile_counters.sio_tick_ns<<"/"<<profile_counters.gpu_video_ns<<"/"<<profile_counters.irq_sample_ns<<"/"<<profile_counters.cdrom_tick_ns<<"/"<<profile_counters.dma_tick_ns<<"/"<<profile_counters.spu_tick_ns<<"/"<<profile_counters.timer_tick_ns;
#else
  out<<"\nCPU_PROFILE subsystem timing=not compiled (build with AVOPSEMU_ENABLE_CPU_PROFILE)";
#endif
  out<<"\nCPU diagnostic logging enabled="<<debug_options.diagnostic_logging<<" retained_events="<<std::dec<<diagnostic_event_records()<<"/"<<diagnostic_history.size()<<"\n";
  out<<"GPU host rasterizer time ns="<<std::dec<<bus.gpu.raster_time_ns<<" (since GPU reset; host CPU time, not emulated GPU time)\n";
 out<<"Video line="<<std::dec<<bus.gpu.video_line<<" dot="<<bus.gpu.video_dot<<" field="<<bus.gpu.video_field<<" vblank="<<bus.gpu.video_vblank<<" frames="<<bus.gpu.video_frames<<" vblank_events="<<bus.gpu.vblank_events<<"\n";
 out<<"GPU display registers:";for(auto word:bus.gpu.display)out<<" "<<std::hex<<word;out<<"\n";
 out<<"Recent GP0 words (oldest first):\n";
 const auto first=bus.gpu.words>bus.gpu.history.size()?bus.gpu.words-bus.gpu.history.size():0;
 for(uint64_t i=first;i<bus.gpu.words;i++)out<<std::hex<<std::setw(8)<<bus.gpu.history[i%bus.gpu.history.size()]<<((i%8==7)?"\n":" ");
 out<<"\n";
 out<<"TTY (observed BIOS putchar entries):\n"<<bus.tty<<"\n";return out.str();
}
void install_demo(Bus& bus) {
    // addiu t0,zero,7; addiu t1,zero,5; addu t2,t0,t1; sw t2,256(zero)
    const uint32_t code[]={0x24080007,0x24090005,0x01095021,0xac0a0100};
    for(unsigned i=0;i<4;i++) bus.write32(i*4,code[i]);
}
}
