#include "sio.hpp"
#include <algorithm>
#include <sstream>

namespace ps1 {
namespace {
constexpr unsigned ack_delay_clocks=100;
constexpr unsigned ack_width_clocks=68; // ceil(2 us at the 33.8688 MHz PS1 clock)
}

void Sio::record(const char* event,uint32_t value,uint8_t offset,uint8_t width,bool register_access){
 Trace entry{event,clocks,context_pc,value,control,mode,baud,status(),offset,width,tx_active_value,data,uint8_t(phase),uint8_t(rx_count),uint8_t(register_access)};
 if(history.size()<history_limit)history.push_back(entry);
 else history[history_count%history_limit]=entry;
 history_count++;
}

void Sio::receive(uint8_t value){
 data=value;
 if(rx_count<rx_fifo.size()){rx_fifo[(rx_head+rx_count)%rx_fifo.size()]=value;rx_count++;}
 else{rx_fifo[(rx_head+rx_fifo.size()-1)%rx_fifo.size()]=value;rx_overflows++;}
 rx=rx_count!=0;
 control&=~4u;
 if(rx_irq_condition())irq=true;
}

void Sio::complete_transfer(){
 uint8_t response=0xff;
 bool acknowledge=false;
 if(connected&&(control&2)&&!(control&0x2000)){
  switch(phase){
   case 0:
    if(tx_active_value==0x01){phase=1;acknowledge=true;}
    else phase=5;
    break;
   case 1:
    if(tx_active_value==0x42){
     response=0x41;phase=2;acknowledge=true;polls++;
     if(host_input){buttons=host_input->held_buttons();latched_buttons=host_input->consume_poll();}
     else latched_buttons=buttons;
    }else phase=5;
    break;
   case 2:
    response=0x5a;phase=3;acknowledge=true;
    break;
   case 3:
    response=uint8_t(latched_buttons);phase=4;acknowledge=true;
    break;
   case 4:
    response=uint8_t(latched_buttons>>8);phase=5;
    completed_polls++;
    if(latched_buttons!=0xffff){pressed_polls++;last_pressed_buttons=latched_buttons;}
    break;
   default:
    break;
  }
 }
 tx_response=response;data=response;
 if(control&6)receive(response);
 record("tx-complete",response,0,1);
 if(acknowledge&&(control&2)&&!(control&0x2000)){
  ack_delay=ack_delay_clocks;
  record("ack-scheduled",ack_delay,0,0);
 }
}

uint32_t Sio::read_data(unsigned bytes){
 uint32_t value=0;
 for(unsigned i=0;i<bytes;i++)value|=uint32_t(i<rx_count?rx_fifo[(rx_head+i)%rx_fifo.size()]:data)<<(i*8);
 const unsigned remove=bytes==4?4:1;
 const unsigned count=std::min(remove,rx_count);
 rx_head=(rx_head+count)%rx_fifo.size();rx_count-=count;rx=rx_count!=0;
 record("read-data",value,0,uint8_t(bytes),true);
 return value;
}

void Sio::start_transfer(uint8_t value){
 tx_active_value=last_tx=value;
 const unsigned factor=(mode&3)==2?16:(mode&3)==3?64:1;
 const unsigned bit_clocks=std::max(1u,(unsigned(baud)*factor)&~1u);
 const unsigned bits=5+((mode>>2)&3)+((mode&0x10)?1u:0u);
 tx_ready=false;tx_busy=true;tx_ready_delay=bit_clocks;tx_delay=bit_clocks*bits;
 record("tx-start",value,0,1);
}

void Sio::tick(uint64_t clock){
 clocks=clock?clock:clocks+1;
 if(irq_reassert&&!--irq_reassert&&(tx_irq_condition()||rx_irq_condition()||((control&0x1000)&&ack))){irq=true;record("irq-reassert");}
 if(ack_width&&!--ack_width){ack=false;record("ack-end");}
 if(ack_delay&&!--ack_delay){ack=true;ack_width=ack_width_clocks;ack_events++;if(control&0x1000)irq=true;record("ack-start",ack_width);}
 if(!tx_busy)return;
 if(tx_ready_delay&&!--tx_ready_delay){
  if(!tx_pending_valid){tx_ready=true;if(control&0x400)irq=true;record("tx-ready");}
 }
 if(!tx_delay||--tx_delay)return;
 const bool has_pending=tx_pending_valid;
 if(!has_pending){tx_busy=false;tx_ready=true;}
 tx_ready_delay=0;tx_completions++;
 complete_transfer();
 if(has_pending){
  const uint8_t next=tx_pending_value;tx_pending_valid=false;
  start_transfer(next);
 }else if(control&0x400)irq=true;
}

uint8_t Sio::read(unsigned offset){
 if(offset==0)return uint8_t(read_data(1));
 if(offset<4){
  const uint8_t value=offset<rx_count?rx_fifo[(rx_head+offset)%rx_fifo.size()]:data;
  record("read-data-preview",value,uint8_t(offset),1,true);
  return value;
 }
 uint8_t value=0;
 if(offset<8){value=uint8_t(status()>>((offset-4)*8));record("read-stat",value,uint8_t(offset),1,true);return value;}
 if(offset==8||offset==9){value=uint8_t(mode>>((offset-8)*8));record("read-mode",value,uint8_t(offset),1,true);return value;}
 if(offset==10||offset==11){value=uint8_t(control>>((offset-10)*8));record("read-control",value,uint8_t(offset),1,true);return value;}
 if(offset==14||offset==15){value=uint8_t(baud>>((offset-14)*8));record("read-baud",value,uint8_t(offset),1,true);return value;}
 return 0;
}

void Sio::write(unsigned offset,uint8_t value){
 auto put=[&](uint16_t& reg,unsigned lane){reg=uint16_t((reg&~(255u<<(lane*8)))|(uint16_t(value)<<(lane*8)));};
 if(offset==0){
  transfers++;
  record("write-data",value,0,1,true);
  if(!(control&1)){record("tx-disabled",value);return;}
  if(tx_busy){
   if(tx_ready&&!tx_pending_valid){
    tx_pending_valid=true;tx_pending_value=value;tx_ready=false;tx_ready_delay=0;
    record("tx-queued",value,0,1);
   }else{
    tx_rejected++;
    record("tx-rejected",value,0,1);
   }
   return;
  }
  start_transfer(value);
  return;
 }
 if(offset==8||offset==9){put(mode,offset-8);mode&=0x1ff;record("write-mode",value,uint8_t(offset),1,true);return;}
 if(offset==14||offset==15){put(baud,offset-14);record("write-baud",value,uint8_t(offset),1,true);return;}
 if(offset!=10&&offset!=11)return;
 const auto old_control=control;
 put(control,offset-10);
 const bool acknowledge=control&0x10;
 const bool reset=control&0x40;
 const bool deselect=!(control&2)||((old_control^control)&0x2000);
 record("write-control",value,uint8_t(offset),1,true);
 if(reset||deselect){
  if(phase>=2&&phase<5)abort_phases[phase]++;
  record(reset?"reset":"deselect");phase=0;ack=false;ack_delay=ack_width=0;
  if(tx_busy){tx_busy=false;tx_ready=true;tx_delay=tx_ready_delay=0;tx_aborts++;}
  tx_pending_valid=false;
 }
 if(reset){
  mode=control=baud=0;rx=irq=ack=false;tx_busy=false;tx_ready=true;tx_pending_valid=false;
  tx_delay=tx_ready_delay=ack_delay=ack_width=irq_reassert=0;rx_head=rx_count=0;data=0;tx_response=0xff;
  record("reset-complete");
  return;
 }
 if(acknowledge){
  if(!(control&0x1000)||!ack){irq=false;irq_reassert=(tx_irq_condition()||rx_irq_condition()||((control&0x1000)&&ack))?2:0;}
  record("irq-ack");
 }
 control&=0x3faf;
 if(!acknowledge&&!irq_reassert&&(tx_irq_condition()||rx_irq_condition()||((control&0x1000)&&ack)))irq=true;
 record("control-update",value,uint8_t(offset),1,true);
}

std::string Sio::diagnostic()const{
 std::ostringstream out;
 out<<"SIO TXRDY="<<tx_ready<<" TXU="<<!tx_busy<<" TX_remaining="<<tx_delay
    <<" TX_pending="<<tx_pending_valid<<':'<<unsigned(tx_pending_value)
    <<" TX_completed="<<tx_completions<<" TX_rejected="<<tx_rejected<<" TX_aborts="<<tx_aborts
    <<" DSR_ACK="<<ack<<" IRQ="<<irq<<"\n";
 out<<"SIO RX FIFO count="<<rx_count<<" overflows="<<rx_overflows
    <<" threshold="<<(1u<<((control>>8)&3))<<" completed_polls="<<completed_polls
    <<" pressed_polls="<<pressed_polls<<" abort phases:";
 for(unsigned i=2;i<5;i++)out<<' '<<i<<'='<<abort_phases[i];
 const auto retained=std::min<uint64_t>(history_count,history.size());
 out<<"\nSIO register trace total="<<history_count<<" retained="<<retained
    <<" dropped="<<(history_count-retained)<<" (cycle, PC, register, access, value, TXRDY/TXU/RXRDY/DSR/IRQ):\n";
 for(uint64_t i=history_count-retained;i<history_count;i++){
  const auto& item=history[history_count<=history.size()?std::size_t(i):std::size_t(i%history.size())];
  out<<std::dec<<item.clock<<" pc="<<std::hex<<item.pc<<' ';
  if(item.register_access){
   const char* reg=item.offset<4?"DATA":item.offset<8?"STAT":item.offset<10?"MODE":item.offset<12?"CTRL":item.offset>=14&&item.offset<16?"BAUD":"?";
   out<<reg<<"+"<<std::dec<<unsigned(item.offset)<<'['<<unsigned(item.width)<<"] ";
  }
  out<<item.event<<" value="<<std::hex<<item.value<<" STAT="<<item.status
     <<" TXRDY="<<bool(item.status&1)<<" TXU="<<bool(item.status&4)
     <<" RXRDY="<<bool(item.status&2)<<" DSR="<<bool(item.status&128)
     <<" IRQ="<<bool(item.status&512)<<" CTRL="<<item.control<<" MODE=";
  out<<item.mode<<" BAUD="<<item.baud<<" tx="<<unsigned(item.tx)<<" rx="<<unsigned(item.rx)
     <<" phase="<<std::dec<<unsigned(item.phase)<<" fifo="<<unsigned(item.count)<<'\n';
 }
 return out.str();
}
}
