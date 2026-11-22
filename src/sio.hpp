#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include "host_pad.hpp"
namespace ps1 {
// Functional digital pad model with baud-timed SIO0 byte transfers.
struct Sio {
 // Non-owning: attach only while the emulation worker is stopped.
 HostPad* host_input=nullptr;
 uint16_t mode=0,control=0,baud=0;
 bool rx=false,irq=false,connected=false,ack=false;
 bool tx_busy=false,tx_ready=true,tx_pending_valid=false;
 unsigned tx_delay=0,tx_ready_delay=0;
 uint8_t tx_response=255,tx_pending_value=0,tx_active_value=255;
 unsigned phase=0;
 std::array<uint8_t,8> rx_fifo{};
 unsigned rx_head=0,rx_count=0;
 unsigned irq_reassert=0;
 uint64_t rx_overflows=0,clocks=0;
 uint32_t context_pc=0;
 struct Trace {
  const char* event="";
  uint64_t clock=0;
  uint32_t pc=0,value=0;
  uint16_t control=0,mode=0,baud=0,status=0;
  uint8_t offset=0,width=0,tx=0,rx=0,phase=0,count=0,register_access=0;
 };
 static constexpr std::size_t history_limit=131072;
 std::vector<Trace> history;
 uint64_t history_count=0,tx_rejected=0;
 std::array<uint64_t,6> abort_phases{};
 uint16_t status()const{return uint16_t((tx_ready?1u:0u)|(rx?2u:0u)|(!tx_busy?4u:0u)|(irq?512u:0u)|(ack?128u:0u));}
 void record(const char* event,uint32_t value=0,uint8_t offset=0,uint8_t width=0,bool register_access=false);
 void receive(uint8_t value);
 void complete_transfer();
 uint32_t read_data(unsigned bytes);
 bool rx_irq_condition()const{return (control&0x800)&&rx_count>=(1u<<((control>>8)&3));}
 bool tx_irq_condition()const{return (control&0x400)&&(tx_ready||!tx_busy);}
 std::string diagnostic()const;
 uint16_t buttons=0xffff,latched_buttons=0xffff;
 uint8_t data=255;
 unsigned ack_delay=0,ack_width=0;
 uint64_t polls=0,ack_events=0,tx_completions=0,tx_aborts=0;
 uint64_t completed_polls=0,pressed_polls=0,transfers=0;
 uint16_t last_pressed_buttons=0xffff;
 uint8_t last_tx=255;
 void start_transfer(uint8_t value);
 void tick(uint64_t clock=0);
 uint8_t read(unsigned offset);
 void write(unsigned offset,uint8_t value);
};
}
