#include "core.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>

static void ck(bool value,const char* message){if(!value)throw std::runtime_error(message);}

int main(){try{
 constexpr unsigned byte_ticks=0x88*8;
 constexpr unsigned bit_ticks=0x88;
 constexpr unsigned ack_delay=100;
  constexpr unsigned ack_width=68;
 ps1::Bus context_bus;context_bus.writer_cpu=true;context_bus.writer_pc=0x80001234;context_bus.sio[0].context_pc=0x12345678;
 context_bus.tick_timers();ck(context_bus.sio[0].context_pc==0x12345678,"idle SIO diagnostic PC is not rewritten each machine clock");
 context_bus.sio[0].ack_delay=1;context_bus.tick_timers();
 ck(context_bus.sio[0].context_pc==context_bus.writer_pc&&context_bus.sio[0].ack_events==1&&context_bus.sio[0].ack_width==ack_width,"active SIO event retains current PC and exact ACK width");
 auto b=std::make_unique<ps1::Bus>();auto& p=b->sio[0];p.connected=true;p.buttons=0xbedf;
 auto ticks=[&](unsigned count){while(count--)b->tick_timers();};
 auto status=[&](){return b->read32(0x1f801044);};
 auto configure=[&](){b->write16(0x1f801048,0x0d);b->write16(0x1f80104e,0x88);b->write16(0x1f80104a,0x1003);};
 auto clear_ack_irq=[&](){b->write32(0x1f801070,~128u);b->write16(0x1f80104a,0x1013);ck(!p.irq,"I_STAT then ERRRST clears an inactive DSR source");};
 configure();
 b->write8(0x1f801040,1);
 ck(p.tx_busy&&!(status()&5)&&!(status()&2),"TX begins busy with TXRDY/TXU/RXRDY low");
 ticks(bit_ticks-1);ck(!(status()&1)&&p.tx_busy,"TXRDY stays low before one complete bit");
 ticks(1);ck((status()&1)&&!(status()&4)&&p.tx_busy,"TXRDY rises after one bit while TXU remains low");
 ticks(byte_ticks-bit_ticks-1);ck(!(status()&4)&&!(status()&2)&&p.tx_busy,"TXU/RXRDY remain low before all eight bits finish");
 ticks(1);ck(!p.tx_busy&&(status()&4)&&(status()&2)&&p.rx_count==1&&p.tx_completions==1,"byte completion raises TXU and enqueues RX");
 ck(b->read8(0x1f801040)==0xff,"address byte response is high impedance FF");
 ck(p.ack_delay==ack_delay&&!p.ack,"ACK delay starts at the final SCK edge");
 ticks(ack_delay-1);ck(!p.ack&&!(status()&128),"DSR does not assert before its minimum delay");
 ticks(1);ck(p.ack&&p.ack_width==ack_width&&(status()&128)&&(status()&512),"DSR asserts on time with an independent pulse-width timer");
 b->write16(0x1f80104a,0x1013);ck(p.irq&&p.ack,"ERRRST cannot clear DSR IRQ while ACK is asserted");
 ticks(ack_width-1);ck(p.ack&&p.ack_width==1,"ACK stays low through the penultimate width clock");
 ticks(1);ck(!p.ack&&p.ack_width==0,"ACK releases at the exact pulse-width deadline");
 b->tick_timers();ck(b->irq_status&128,"DSR IRQ edge reaches I_STAT.7");clear_ack_irq();

 const uint8_t send[]={0x42,0,0,0};const uint8_t want[]={0x41,0x5a,0xdf,0xbe};
 for(unsigned i=0;i<4;i++){
  b->write8(0x1f801040,send[i]);ticks(byte_ticks);
  ck(!p.tx_busy&&b->read8(0x1f801040)==want[i],"digital controller returns each byte in order after TX completion");
  if(i<3){ck(p.ack_delay==ack_delay,"ACK scheduled only for non-final bytes");ticks(ack_delay);ck(p.ack&&p.irq,"non-final byte raises DSR IRQ");ticks(ack_width);ck(!p.ack,"non-final ACK pulse has exact width");clear_ack_irq();}
  else ck(!p.ack_delay&&!p.ack,"final button byte does not ACK");
 }
 ck(p.polls==1&&p.completed_polls==1&&p.pressed_polls==1&&p.last_pressed_buttons==0xbedf,"poll counters commit only when the high button byte completes");

  auto bios_irq_bus=std::make_unique<ps1::Bus>();auto& bios_irq=bios_irq_bus->sio[0];bios_irq.connected=true;bios_irq.buttons=0xffef;
  bios_irq_bus->write16(0x1f801048,0x0d);bios_irq_bus->write16(0x1f80104e,0x88);bios_irq_bus->write16(0x1f80104a,0x1003);
  auto bios_ticks=[&](unsigned count){while(count--)bios_irq_bus->tick_timers();};
  const uint8_t bios_tx[]={0x01,0x42,0x00},bios_rx[]={0xff,0x41,0x5a};
  for(unsigned i=0;i<3;i++){
   bios_irq_bus->write8(0x1f801040,bios_tx[i]);bios_ticks(byte_ticks+ack_delay);
   ck(bios_irq_bus->read8(0x1f801040)==bios_rx[i]&&bios_irq.ack,"BIOS preamble response arrives with delayed DSR");
   if(i<2){bios_ticks(ack_width);bios_irq_bus->write16(0x1f80104a,0x1013);bios_irq_bus->write32(0x1f801070,~128u);bios_irq_bus->tick_timers();
	ck(!bios_irq.ack&&!bios_irq.irq&&!(bios_irq_bus->irq_status&128),"BIOS ERRRST then I_STAT acknowledge clears the prior DSR edge");}
  }
  bios_ticks(13);bios_irq_bus->write8(0x1f801040,0x00);bios_ticks(76);
  bios_irq_bus->write16(0x1f80104a,0x1013);bios_irq_bus->write32(0x1f801070,~128u);bios_irq_bus->tick_timers();
  ck(!bios_irq.ack&&!bios_irq.irq&&!(bios_irq_bus->irq_status&128),"BIOS ERRRST at ACK age 89 occurs after the 68-clock pulse");
  bios_ticks(byte_ticks-77);ck(bios_irq_bus->read8(0x1f801040)==0xef,"BIOS reads the active-low low button byte");bios_ticks(ack_delay);
  ck(bios_irq.ack&&bios_irq.irq&&(bios_irq_bus->irq_status&128),"low-byte ACK creates a fresh DSR IRQ after BIOS acknowledge ordering");
  bios_ticks(13);bios_irq_bus->write8(0x1f801040,0x00);bios_ticks(76);
  bios_irq_bus->write16(0x1f80104a,0x1013);bios_irq_bus->write32(0x1f801070,~128u);bios_irq_bus->tick_timers();
  bios_ticks(byte_ticks-77);
  ck(bios_irq_bus->read8(0x1f801040)==0xff&&bios_irq.completed_polls==1&&bios_irq.pressed_polls==1&&!bios_irq.ack_delay&&!bios_irq.ack,
	 "BIOS phase-2 trace completes both button bytes without ACK after the final byte");

 b->write16(0x1f80104a,1);b->write16(0x1f80104a,0x1003);
 b->write8(0x1f801040,1);ticks(byte_ticks+ack_delay);ck(p.ack,"new address byte receives delayed ACK");ticks(ack_width);ck(!p.ack,"address ACK releases before next transaction");clear_ack_irq();
 b->write8(0x1f801040,0x42);ticks(byte_ticks+ack_delay);ck(p.polls==2&&p.phase==2,"command advances only after its full byte transfer");ticks(ack_width);clear_ack_irq();
 b->write8(0x1f801040,0);ticks(byte_ticks+ack_delay);ticks(ack_width);clear_ack_irq();
 b->write8(0x1f801040,0);ticks(byte_ticks+ack_delay);ticks(ack_width);clear_ack_irq();
 const auto completed_before_abort=p.completed_polls;
 b->write8(0x1f801040,0);ck(p.tx_busy,"final byte is in flight before deselect");b->write16(0x1f80104a,1);
 ck(!p.tx_busy&&p.completed_polls==completed_before_abort&&p.tx_aborts==1&&p.abort_phases[4]>0,"deselect aborts the final byte without false poll completion");

 b->write16(0x1f80104a,0x40);p.connected=false;configure();
 b->write8(0x1f801040,1);ticks(byte_ticks);
 ck(!p.tx_busy&&b->read8(0x1f801040)==0xff&&!p.ack_delay&&!p.ack,"disconnected port returns FF without ACK");
 ticks(3387);ck(!p.ack&&!p.irq&&!(b->irq_status&128),"no-controller response remains unacknowledged past the timeout");
 b->write16(0x1f80104a,0x40);p.connected=true;configure();
 b->write8(0x1f801040,0x81);ticks(byte_ticks);
 ck(b->read8(0x1f801040)==0xff&&!p.ack&&!p.ack_delay,"memory-card address 81 is not a digital controller");
 b->write16(0x1f80104a,0x40);

 auto pipe_bus=std::make_unique<ps1::Bus>();auto& pipe=pipe_bus->sio[0];pipe.connected=true;pipe.buttons=0xbedf;
 pipe_bus->write16(0x1f801048,0x0d);pipe_bus->write16(0x1f80104e,0x88);pipe_bus->write16(0x1f80104a,0x1003);
 pipe_bus->write8(0x1f801040,1);for(unsigned n=0;n<bit_ticks;n++)pipe_bus->tick_timers();
 ck(pipe.tx_ready&&pipe.tx_busy,"TXRDY opens the holding register after the first bit");
 pipe_bus->write8(0x1f801040,0x42);ck(pipe.tx_pending_valid&&!pipe.tx_ready&&pipe.tx_busy,"second byte occupies the holding register");
 pipe_bus->write8(0x1f801040,0);ck(pipe.tx_pending_value==0x42&&pipe.tx_rejected==1,"full holding register rejects a third write without replacing the queued byte");
 for(unsigned n=0;n<byte_ticks-bit_ticks;n++)pipe_bus->tick_timers();
 ck(pipe.tx_busy&&pipe.tx_active_value==0x42&&!pipe.tx_ready&&pipe.phase==1&&pipe.tx_completions==1,"queued byte starts in order at the first byte's completion");
 ck(pipe_bus->read8(0x1f801040)==0xff,"first RX byte is published only after address completion");
 for(unsigned n=0;n<byte_ticks;n++)pipe_bus->tick_timers();
 ck(!pipe.tx_busy&&pipe.phase==2&&pipe.polls==1&&pipe_bus->read8(0x1f801040)==0x41,"queued command response follows the address response");
 ck(pipe.completed_polls==0,"queued bytes do not fabricate a completed poll");

 auto ack_write_bus=std::make_unique<ps1::Bus>();auto& ack_write=ack_write_bus->sio[0];ack_write.connected=true;
 ack_write_bus->write16(0x1f801048,0x0d);ack_write_bus->write16(0x1f80104e,0x88);ack_write_bus->write16(0x1f80104a,0x1003);
 ack_write_bus->write8(0x1f801040,1);for(unsigned n=0;n<byte_ticks+ack_delay;n++)ack_write_bus->tick_timers();
 ck(ack_write.ack&&ack_write.ack_width==ack_width,"ACK is asserted before the next DATA write");
 ack_write_bus->write8(0x1f801040,0x42);ck(ack_write.ack&&ack_write.ack_width==ack_width,"next DATA write does not clear or restart ACK");
 for(unsigned n=0;n<ack_width-1;n++)ack_write_bus->tick_timers();ck(ack_write.ack,"next shifter activity does not shorten ACK");
 ack_write_bus->tick_timers();ck(!ack_write.ack,"ACK width remains independent of the next byte");

 auto reset_bus=std::make_unique<ps1::Bus>();auto& reset_sio=reset_bus->sio[0];reset_sio.connected=true;
 reset_bus->write16(0x1f801048,0x0d);reset_bus->write16(0x1f80104e,0x88);reset_bus->write16(0x1f80104a,0x1003);
 reset_bus->write8(0x1f801040,1);for(unsigned n=0;n<bit_ticks;n++)reset_bus->tick_timers();reset_bus->write8(0x1f801040,0x42);
 reset_bus->write8(0x1f80104a,0x40);
 ck(!reset_sio.tx_busy&&!reset_sio.tx_pending_valid&&!reset_sio.rx_count&&!reset_sio.ack&&!reset_sio.ack_delay&&reset_sio.phase==0&&reset_sio.tx_aborts==1,"reset clears active/queued TX, RX, ACK and protocol state");

 auto rx_irq_bus=std::make_unique<ps1::Bus>();auto& rx_irq=rx_irq_bus->sio[0];rx_irq.connected=true;
 rx_irq_bus->write16(0x1f801048,0x0d);rx_irq_bus->write16(0x1f80104e,0x88);rx_irq_bus->write16(0x1f80104a,0x0903);
 rx_irq_bus->write8(0x1f801040,1);for(unsigned n=0;n<byte_ticks;n++)rx_irq_bus->tick_timers();
 ck(rx_irq.rx_count==1&&!rx_irq.irq,"RXIEN waits for its two-byte FIFO threshold");
 rx_irq_bus->write8(0x1f801040,0x42);for(unsigned n=0;n<byte_ticks;n++)rx_irq_bus->tick_timers();
 ck(rx_irq.rx_count==2&&rx_irq.irq,"RXIEN asserts when the threshold is reached");rx_irq_bus->tick_timers();ck(rx_irq_bus->irq_status&128,"RXIEN routes its source to IRQ7");
 ck(rx_irq_bus->read8(0x1f801040)==0xff&&rx_irq.rx_count==1,"RX FIFO preserves the oldest byte and falls below threshold");
 rx_irq_bus->write32(0x1f801070,~128u);rx_irq_bus->write16(0x1f80104a,0x0913);
 for(unsigned n=0;n<3;n++)rx_irq_bus->tick_timers();ck(!rx_irq.irq&&!(rx_irq_bus->irq_status&128),"I_STAT acknowledge then ERRRST clears the drained RX source");

 auto irq_bus=std::make_unique<ps1::Bus>();auto& irq_sio=irq_bus->sio[0];
 irq_bus->write16(0x1f801048,0x0d);irq_bus->write16(0x1f80104e,0x88);irq_bus->write16(0x1f80104a,3);irq_bus->write8(0x1f801040,1);irq_bus->write16(0x1f80104a,0x0403);
 for(unsigned n=0;n<bit_ticks-1;n++)irq_bus->tick_timers();ck(!irq_sio.irq&&!(irq_bus->read32(0x1f801044)&1),"TXIEN waits until TXRDY");
 irq_bus->tick_timers();ck(irq_sio.irq&&(irq_bus->read32(0x1f801044)&513)==513,"TXIEN fires when TXRDY rises");
 irq_bus->write32(0x1f801070,~128u);irq_bus->write16(0x1f80104a,0x0413);ck(!irq_sio.irq,"I_STAT then ERRRST lowers TX-ready IRQ briefly");
 irq_bus->tick_timers();ck(!irq_sio.irq,"TX IRQ remains low for the acknowledge edge");irq_bus->tick_timers();ck(irq_sio.irq,"enabled TX-ready source reasserts after ERRRST");irq_bus->tick_timers();ck(irq_bus->irq_status&128,"reasserted TX source returns to IRQ7");

 auto bios_trace_bus=std::make_unique<ps1::Bus>();auto& bios_trace=bios_trace_bus->sio[0];bios_trace.connected=true;bios_trace.buttons=0xbfff;
 bios_trace_bus->write16(0x1f801048,0x0d);bios_trace_bus->write16(0x1f80104e,0x88);bios_trace_bus->write16(0x1f80104a,0x1003);
 bios_trace_bus->write8(0x1f801040,0x01);for(unsigned n=0;n<byte_ticks+ack_delay;n++)bios_trace_bus->tick_timers();
 ck(bios_trace_bus->read8(0x1f801040)==0xff&&bios_trace.ack,"BIOS address phase receives FF and its delayed ACK");
 bios_trace_bus->write8(0x1f801040,0x42);for(unsigned n=0;n<byte_ticks+ack_delay;n++)bios_trace_bus->tick_timers();
 ck(bios_trace_bus->read8(0x1f801040)==0x41&&bios_trace.phase==2&&bios_trace.polls==1&&bios_trace.completed_polls==0,"BIOS trace receives 41 only after 01/42 byte completion");
 bios_trace_bus->write16(0x1f80104a,1);ck(bios_trace.abort_phases[2]==1&&bios_trace.completed_polls==0,"phase-2 deselect aborts without a false completed poll");
 const auto register_trace=bios_trace.diagnostic();
 ck(register_trace.find("DATA+0[1] write-data")!=std::string::npos&&register_trace.find("DATA+0[1] read-data")!=std::string::npos&&register_trace.find("cycle, PC, register")!=std::string::npos,"SIO trace retains cycle/PC register accesses");

 auto baud_bus=std::make_unique<ps1::Bus>();auto& baud_sio=baud_bus->sio[0];baud_sio.connected=true;
 baud_bus->write16(0x1f801048,0x0d);baud_bus->write16(0x1f80104e,0x44);baud_bus->write16(0x1f80104a,3);baud_bus->write8(0x1f801040,1);
 for(unsigned n=0;n<0x44*8-1;n++)baud_bus->tick_timers();ck(baud_sio.tx_busy&&!(baud_bus->read32(0x1f801044)&4),"alternate baud remains busy before its calculated byte deadline");
 baud_bus->tick_timers();ck(!baud_sio.tx_busy&&baud_sio.tx_completions==1,"alternate baud completes at 544 clocks, not a hardcoded 1088");
 std::cout<<"PASS: SIO0 TX/RX timing and FIFO, holding byte, ACK delay/width, TX/RX/DSR IRQ, abort/reset, BIOS phase-2 trace, no-controller/memory-card probe and baud scaling\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
