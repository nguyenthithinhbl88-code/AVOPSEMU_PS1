#include "core.hpp"
#include "diagnostic_capture.hpp"
#include "wav.hpp"
#include <memory>
#include <iostream>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include "win_crash_diagnostics.hpp"
#endif
int main(int argc,char** argv){
#ifdef _WIN32
 ps1::win_diag::install_handlers();
 const bool diagnostics_ready=ps1::win_diag::initialize_default();
 if(!diagnostics_ready)std::cerr<<"Crash diagnostics could not open %LOCALAPPDATA%\\AVOPSEMU\\Logs.\n";
#endif
 try{
  ps1::Bus bus;ps1::Cpu cpu(bus);
  if(argc==1||(argc==2&&std::string(argv[1])=="--demo")){
   ps1::install_demo(bus);cpu.reset(0);for(int i=0;i<4;i++)cpu.step();std::cout<<"MIPS demo: 7 + 5 = "<<bus.read32(256)<<"\n";return 0;
  }
   std::string bios,exe,disc,wav,capture;
   uint64_t steps=1000000,capture_interval=0;
  for(int i=1;i<argc;i++){
   const std::string arg=argv[i];if(i+1>=argc)throw std::runtime_error("Missing option value");
   const std::string value=argv[++i];
	   if(arg=="--bios")bios=value;
	else if(arg=="--exe")exe=value;
	else if(arg=="--disc")disc=value;
	else if(arg=="--wav")wav=value;
	else if(arg=="--capture")capture=value;
	else if(arg=="--capture-every-instructions"){size_t used=0;capture_interval=std::stoull(value,&used);if(used!=value.size()||!capture_interval)throw std::runtime_error("Capture interval must be greater than zero");}
	else if(arg=="--steps"){size_t used=0;steps=std::stoull(value,&used);if(used!=value.size()||steps>1000000000)throw std::runtime_error("Step limit is 1000000000");}
   else throw std::runtime_error("Unknown CLI option");
  }
  if((bios.empty()&&exe.empty()&&disc.empty())||(!exe.empty()&&(!bios.empty()||!disc.empty())))throw std::runtime_error("Choose EXE, disc direct boot, or BIOS with optional disc");
  if(!bios.empty()){bus.load_bios(bios);cpu.reset();}
  if(!disc.empty()){bus.cd.mount(disc);ps1::boot_disc(bus,cpu,bios.empty());}
  if(!exe.empty()){
   std::ifstream file(exe,std::ios::binary|std::ios::ate);if(!file||file.tellg()>4*1024*1024)throw std::runtime_error("EXE unavailable or exceeds 4 MiB");
   file.seekg(0);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});ps1::load_exe(bus,cpu,bytes);
  }
  bus.sio[0].connected=true;
	if(capture_interval&&capture.empty())throw std::runtime_error("--capture-every-instructions requires --capture");
  std::unique_ptr<ps1::WaveWriter> writer;if(!wav.empty())writer=std::make_unique<ps1::WaveWriter>(wav);
	auto save_capture=[&](const std::string& prefix){
	const auto result=ps1::save_debug_images_detailed(bus,std::filesystem::path(prefix));
	if(!result.success()){
	 for(const auto& file:result.files)if(!file.success)std::cerr<<"Capture failed: "<<file.path.u8string()<<" operation="<<file.error<<" error="<<file.error_code<<" bytes_written="<<file.bytes_written<<" bytes_verified="<<file.bytes_verified<<"\n";
	 return false;
	}
	std::cout<<"Capture verified: prefix="<<prefix<<" framebuffer="<<result.framebuffer_width<<"x"<<result.framebuffer_height<<" pixel_format="<<result.pixel_format<<" bytes_written="<<result.total_bytes_written()<<" bytes_verified="<<result.total_bytes_verified()<<"\n";
	return true;
   };
   uint64_t last_capture=0;
	 if(capture_interval){
	for(uint64_t i=0;i<steps;i++){
	 cpu.step();if(cpu.bios_system_error){std::cerr<<cpu.failure_summary()<<"\n"<<cpu.diagnostic();return 1;}
	 if(writer&&(i&4095)==4095)writer->append(bus.spu.audio);
	 const uint64_t completed=i+1;
	 if(completed%capture_interval==0){if(!save_capture(capture+"-"+std::to_string(completed)))return 2;last_capture=completed;}
	}
   }else{
	for(uint64_t i=0;i<steps;i++){cpu.step();if(cpu.bios_system_error){std::cerr<<cpu.failure_summary()<<"\n"<<cpu.diagnostic();return 1;}if(writer&&(i&4095)==4095)writer->append(bus.spu.audio);}
   }
  if(writer){writer->append(bus.spu.audio);writer->finish();}
   if(!capture.empty()&&(!capture_interval||last_capture!=steps)){
	const std::string prefix=capture_interval?capture+"-"+std::to_string(steps):capture;
	if(!save_capture(prefix))return 2;
   }
  std::cout<<cpu.diagnostic();return 0;
   }catch(const std::exception& e){
#ifdef _WIN32
#endif
	 std::cerr<<"Stopped: "<<e.what()<<"\nUsage: ps1_lab [--demo | --exe file --steps N | --disc file.cue --steps N | --bios bios.bin [--disc file.cue] --steps N [--wav output.wav] [--capture prefix [--capture-every-instructions N]]]\n";return 1;
  }catch(...){
#ifdef _WIN32
#endif
   std::cerr<<"Stopped: non-standard exception\n";return 1;
  }
}
