#include "diagnostic_capture.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <string>
static void ck(bool b,const char* m){if(!b)throw std::runtime_error(m);}
namespace {
struct TempDirectory {
 std::filesystem::path path;
 TempDirectory(){path=std::filesystem::temp_directory_path()/("avopsemu_capture_tests_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(path);}
 ~TempDirectory(){std::error_code error;std::filesystem::remove_all(path,error);}
 void cleanup(){std::error_code error;std::filesystem::remove_all(path,error);ck(!error,"capture test files could not be removed");}
};
}
int main(){try{TempDirectory temp;auto b=std::make_unique<ps1::Bus>();
 b->write32(0x801f0064,0x12345678);ck(b->watch_total==1&&b->watch_writes[0].after==0x34,"watch word byte-lane");
 b->write16(0xa07f0066,0xabcd);ck(b->watch_total==2&&b->watch_writes[1].before==0x34&&b->watch_writes[1].after==0xcd,"watch RAM aliases and halfword");
 b->cd.requested=true;b->cd.data={1,2,3,4};b->dpcr=0x8000;
 b->write32(0x1f8010b0,0x1f0064);b->write32(0x1f8010b4,1);b->write32(0x1f8010b8,0x11000000);
 ck(b->watch_total==3&&b->watch_writes[2].dma==3&&b->watch_writes[2].after==3&&b->writer_dma==-1,"DMA source attribution and scope restoration");
 const auto n=b->watch_total;b->watch_diagnostic();ck(b->watch_total==n&&b->read8(0x801f0066)==3,"watch snapshot never mutates target");
 for(unsigned i=0;i<20;i++){b->write8(0x801f0066,uint8_t(i));}ck(b->watch_total==23&&b->watch_diagnostic().find("writes=23")!=std::string::npos,"bounded chronological watch ring");
  b->gpu.vram[0]=31;auto path=temp.path/"bios_capture.txt";
 const auto capture=ps1::save_debug_images_detailed(*b,path);ck(capture.success(),"capture files saved and verified");
 ck(capture.framebuffer_width==320&&capture.framebuffer_height==240,"framebuffer dimensions recorded");
 ck(capture.files.size()==3&&capture.total_bytes_written()==capture.total_bytes_verified(),"all capture bytes read back");
 for(size_t i=0;i<capture.files.size();i++){
  const auto& artifact=capture.files[i];ck(artifact.success&&artifact.bytes_written==artifact.expected_bytes&&artifact.bytes_verified==artifact.expected_bytes,"capture artifact byte counts");
  std::ifstream f(artifact.path,std::ios::binary);char h[2]{};f.read(h,2);ck(bool(f),"capture readable");
  if(i)ck(h[0]=='B'&&h[1]=='M',"standard BMP header");
  ck(std::filesystem::file_size(artifact.path)==artifact.expected_bytes,"capture file size");
 }
 const std::array<uint8_t,4> sample{{1,2,3,4}};
 const auto verified_log=ps1::write_verified_file(temp.path/"verified.log",sample.data(),sample.size());
 ck(verified_log.success&&verified_log.bytes_written==sample.size()&&verified_log.bytes_verified==sample.size(),"diagnostic log write and read-back");
 const auto missing=ps1::write_verified_file(temp.path/"missing"/"capture.bin",sample.data(),sample.size());
 ck(!missing.success&&missing.error_code!=0,"missing output directory reports write error");
 const auto blocked=temp.path/"blocked.bin";std::filesystem::create_directory(blocked);
 const auto write_failure=ps1::write_verified_file(blocked,sample.data(),sample.size());
 ck(!write_failure.success&&write_failure.error_code!=0,"write failure reports system error");
#ifdef _WIN32
 const auto invalid_path=temp.path/"invalid|capture.bin";
#else
 const auto invalid_parent=temp.path/"invalid";std::filesystem::create_directory(invalid_parent);
 std::string invalid_name=invalid_parent.string();invalid_name.push_back('\0');invalid_name+="capture.bin";
 const std::filesystem::path invalid_path(invalid_name);
#endif
 const auto invalid=ps1::write_verified_file(invalid_path,sample.data(),sample.size());
 ck(!invalid.success&&invalid.error_code!=0,"invalid output path reports system error");
 temp.cleanup();
 std::cout<<"PASS: watch attribution, verified VRAM/BMP/framebuffer capture, and invalid/missing/unwritable paths\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
