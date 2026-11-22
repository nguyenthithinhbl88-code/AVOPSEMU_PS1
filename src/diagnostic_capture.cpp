#include "diagnostic_capture.hpp"
#include "display.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <system_error>
#endif
namespace ps1 {
void Bus::watch_ram_write(uint32_t address,uint32_t value,unsigned width){
 const uint32_t base=(address&0x1fffffff)&0x1fffff;
 if(watched_byte<base||watched_byte-base>=width)return;
 const auto after=uint8_t(value>>((watched_byte-base)*8));
 watch_writes[watch_total++%watch_writes.size()]={cd.clocks,writer_instruction,writer_cpu?writer_pc:0,writer_cpu?writer_opcode:0,address,ram[watched_byte],after,uint8_t(width),writer_dma,writer_cpu};
}
std::string Bus::watch_diagnostic()const{
 std::ostringstream o;o<<"WRITE_WATCH 801f0066 physical=001f0066 value="<<std::hex<<unsigned(ram[watched_byte])<<std::dec<<" writes="<<watch_total<<" (RAM aliases; diagnostic only, never clears the byte)\n";
 o<<"CD_FIFO accepted_lba="<<std::dec<<cd.data_lba<<" bytes="<<cd.data.size()<<" next_lba="<<cd.lba<<" sectors_with_old_fifo="<<cd.sectors_with_fifo_tail<<"\n";
 const auto count=std::min<uint64_t>(watch_total,watch_writes.size());
 for(uint64_t i=watch_total-count;i<watch_total;i++){
  const auto& w=watch_writes[i%watch_writes.size()];
  o<<"watch clock="<<std::dec<<w.clock<<" ins="<<w.instruction<<" source=";
  if(w.dma>=0)o<<"DMA"<<w.dma;else o<<(w.cpu?"CPU":"host");
  o<<" trigger_pc="<<std::hex<<w.pc<<" opcode="<<w.opcode<<" address="<<w.address<<" width="<<std::dec<<unsigned(w.width)<<" value="<<std::hex<<unsigned(w.before)<<"->"<<unsigned(w.after)<<"\n";
 }
 return o.str();
}
namespace {
void fail(CaptureFileResult& result,const char* operation,uint32_t code){
 result.error=operation;result.error_code=code;result.success=false;
}
void append_le(std::vector<uint8_t>& bytes,uint32_t value,unsigned width){
 for(unsigned i=0;i<width;i++)bytes.push_back(uint8_t(value>>(i*8)));
}
template<class Pixel>std::vector<uint8_t> bmp(unsigned width,unsigned height,Pixel pixel){
 const uint64_t row=(uint64_t(width)*3+3)&~uint64_t(3),image_size=row*height,total=54+image_size;
 if(!width||!height||total>UINT32_MAX)throw std::runtime_error("Invalid diagnostic BMP dimensions");
 std::vector<uint8_t> bytes;bytes.reserve(static_cast<size_t>(total));
 bytes.push_back('B');bytes.push_back('M');append_le(bytes,uint32_t(total),4);append_le(bytes,0,4);append_le(bytes,54,4);
 append_le(bytes,40,4);append_le(bytes,width,4);append_le(bytes,height,4);append_le(bytes,1,2);append_le(bytes,24,2);
 append_le(bytes,0,4);append_le(bytes,uint32_t(image_size),4);append_le(bytes,0,4);append_le(bytes,0,4);append_le(bytes,0,4);append_le(bytes,0,4);
 for(unsigned y=height;y>0;y--){
  for(unsigned x=0;x<width;x++){const auto color=pixel(x,y-1);bytes.push_back(uint8_t(color));bytes.push_back(uint8_t(color>>8));bytes.push_back(uint8_t(color>>16));}
  while((bytes.size()-54)%row)bytes.push_back(0);
 }
 return bytes;
}
}
CaptureFileResult write_verified_file(const std::filesystem::path& path,const void* data,size_t size){
 CaptureFileResult result;result.path=path;result.expected_bytes=size;
 if(size&&!data){
#ifdef _WIN32
  fail(result,"invalid input buffer",ERROR_INVALID_PARAMETER);
#else
  fail(result,"invalid input buffer",EINVAL);
#endif
  return result;
 }
#ifdef _WIN32
 HANDLE output=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(output==INVALID_HANDLE_VALUE){fail(result,"CreateFileW",GetLastError());return result;}
 bool okay=true;const auto* bytes=static_cast<const uint8_t*>(data);
 while(result.bytes_written<size){
  const DWORD requested=static_cast<DWORD>(std::min<size_t>(size-static_cast<size_t>(result.bytes_written),0x7fffffff));DWORD written=0;
  if(!WriteFile(output,bytes+result.bytes_written,requested,&written,nullptr)){fail(result,"WriteFile",GetLastError());okay=false;break;}
  if(!written){fail(result,"WriteFile returned zero bytes",ERROR_WRITE_FAULT);okay=false;break;}
  result.bytes_written+=written;
 }
 if(okay&&!FlushFileBuffers(output)){fail(result,"FlushFileBuffers",GetLastError());okay=false;}
 LARGE_INTEGER output_size{};
 if(okay&&!GetFileSizeEx(output,&output_size)){fail(result,"GetFileSizeEx",GetLastError());okay=false;}
 if(okay&&output_size.QuadPart!=static_cast<LONGLONG>(size)){fail(result,"written file size mismatch",ERROR_FILE_INVALID);okay=false;}
 if(!CloseHandle(output)&&okay){fail(result,"CloseHandle after write",GetLastError());okay=false;}
 if(!okay)return result;

 HANDLE input=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(input==INVALID_HANDLE_VALUE){fail(result,"CreateFileW for read-back",GetLastError());return result;}
 LARGE_INTEGER input_size{};
 if(!GetFileSizeEx(input,&input_size)){fail(result,"GetFileSizeEx for read-back",GetLastError());okay=false;}
 if(okay&&input_size.QuadPart!=static_cast<LONGLONG>(size)){fail(result,"read-back file size mismatch",ERROR_FILE_INVALID);okay=false;}
 std::array<uint8_t,65536> read_buffer{};
 while(okay&&result.bytes_verified<size){
  const DWORD requested=static_cast<DWORD>(std::min<uint64_t>(size-result.bytes_verified,read_buffer.size()));DWORD read=0;
  if(!ReadFile(input,read_buffer.data(),requested,&read,nullptr)){fail(result,"ReadFile for read-back",GetLastError());okay=false;break;}
  if(!read){fail(result,"ReadFile reached EOF early",ERROR_HANDLE_EOF);okay=false;break;}
  if(std::memcmp(bytes+result.bytes_verified,read_buffer.data(),read)!=0){fail(result,"read-back content mismatch",ERROR_CRC);okay=false;break;}
  result.bytes_verified+=read;
 }
 if(!CloseHandle(input)&&okay){fail(result,"CloseHandle after read-back",GetLastError());okay=false;}
 if(okay)result.success=true;
#else
 errno=0;std::ofstream output(path,std::ios::binary|std::ios::trunc);
 if(!output){fail(result,"open output file",errno?uint32_t(errno):uint32_t(EIO));return result;}
 const auto* bytes=static_cast<const char*>(data);
 if(size){output.write(bytes,static_cast<std::streamsize>(size));if(output)result.bytes_written=size;}
 output.flush();if(!output){fail(result,"write/flush output file",errno?uint32_t(errno):uint32_t(EIO));output.close();return result;}
 output.close();if(!output){fail(result,"close output file",errno?uint32_t(errno):uint32_t(EIO));return result;}
 std::error_code file_error;const auto actual_size=std::filesystem::file_size(path,file_error);
 if(file_error){fail(result,"file_size after write",uint32_t(file_error.value()));return result;}
 if(actual_size!=size){fail(result,"written file size mismatch",uint32_t(EIO));return result;}
 std::ifstream input(path,std::ios::binary);if(!input){fail(result,"open file for read-back",errno?uint32_t(errno):uint32_t(EIO));return result;}
 std::array<uint8_t,65536> read_buffer{};const auto* expected=static_cast<const uint8_t*>(data);
 while(result.bytes_verified<size){
  const auto requested=static_cast<std::streamsize>(std::min<uint64_t>(size-result.bytes_verified,read_buffer.size()));
  input.read(reinterpret_cast<char*>(read_buffer.data()),requested);const auto read=input.gcount();
  if(read<=0){fail(result,"read-back reached EOF early",uint32_t(EIO));return result;}
  if(std::memcmp(expected+result.bytes_verified,read_buffer.data(),static_cast<size_t>(read))!=0){fail(result,"read-back content mismatch",uint32_t(EIO));return result;}
  result.bytes_verified+=static_cast<uint64_t>(read);
 }
 input.close();if(!input){fail(result,"close read-back file",errno?uint32_t(errno):uint32_t(EIO));return result;}
 result.success=true;
#endif
 return result;
}

bool CaptureResult::success()const noexcept{
 return files.size()==3&&std::all_of(files.begin(),files.end(),[](const auto& file){return file.success;});
}
uint64_t CaptureResult::total_bytes_written()const noexcept{
 uint64_t total=0;for(const auto& file:files)total+=file.bytes_written;return total;
}
uint64_t CaptureResult::total_bytes_verified()const noexcept{
 uint64_t total=0;for(const auto& file:files)total+=file.bytes_verified;return total;
}

CaptureResult save_debug_images_detailed(const Bus& b,const std::filesystem::path& log){
 CaptureResult result;result.pixel_format="VRAM RGB555 little-endian; BMP 24-bit BGR bottom-up";
 auto raw_path=log;raw_path+=".vram.bin";std::vector<uint8_t> raw;raw.reserve(b.gpu.vram.size()*2);
 for(auto pixel:b.gpu.vram){raw.push_back(uint8_t(pixel));raw.push_back(uint8_t(pixel>>8));}
 result.files.push_back(write_verified_file(raw_path,raw.data(),raw.size()));

 auto vram_path=log;vram_path+=".vram.bmp";
 auto vram_bmp=bmp(1024,512,[&](unsigned x,unsigned y){return b.gpu.rgb(x,y);});
 result.files.push_back(write_verified_file(vram_path,vram_bmp.data(),vram_bmp.size()));

 const auto view=display_view(b.gpu);result.framebuffer_width=view.width;result.framebuffer_height=view.height;
 auto frame_path=log;frame_path+=".frame.bmp";
 auto frame_bmp=bmp(view.width,view.height,[&](unsigned x,unsigned y){return display_pixel(b.gpu,view,x,y);});
 result.files.push_back(write_verified_file(frame_path,frame_bmp.data(),frame_bmp.size()));
 return result;
}

bool save_debug_images(const Bus& b,const std::filesystem::path& log){
 return save_debug_images_detailed(b,log).success();
}
}
