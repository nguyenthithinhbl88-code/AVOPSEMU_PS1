#include "display.hpp"
#include "gpu.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr unsigned texture_x=640;
constexpr unsigned texture_y=256;
constexpr unsigned texture_size=256;
constexpr unsigned raster_iterations=250;
constexpr unsigned measured_runs=10;

struct Workload {
 const char* name;
 bool textured;
 bool gouraud;
 bool semi;
};

constexpr std::array<Workload,5> workloads{{
 {"flat untextured",false,false,false},
 {"Gouraud untextured",false,true,false},
 {"textured opaque/modulated",true,false,false},
 {"textured semi-transparent",true,false,true},
 {"textured Gouraud",true,true,false}
}};

uint64_t hash_vram(const ps1::Gpu& gpu){
 uint64_t hash=1469598103934665603ull;
 for(const uint16_t pixel:gpu.vram){hash^=pixel;hash*=1099511628211ull;}
 return hash;
}

void prepare(ps1::Gpu& gpu,bool textured){
 std::fill(gpu.vram.begin(),gpu.vram.end(),uint16_t(0));
 for(unsigned y=0;y<240;y++)std::fill_n(gpu.vram.begin()+y*1024,320,uint16_t(0x4210));
 gpu.left=0;gpu.top=0;gpu.right=319;gpu.bottom=239;
 gpu.offset_x=0;gpu.offset_y=0;gpu.force_mask=false;gpu.check_mask=false;
 gpu.texture_window=0;gpu.allow_texture_y512=false;gpu.raster_time_ns=0;
 // Page X=10, page Y=1, 15-bit texture depth. The page starts at VRAM (640,256).
 gpu.draw_mode=textured?0x11au:0u;
 if(!textured)return;
 for(unsigned v=0;v<texture_size;v++)for(unsigned u=0;u<texture_size;u++){
  const uint16_t red=uint16_t((u*3+v+1)&31);
  const uint16_t green=uint16_t((u+v*5+1)&31);
  const uint16_t blue=uint16_t((u*7+v*11+1)&31);
  const uint16_t stp=((u^v)&1)?0x8000:0;
  gpu.vram[(texture_y+v)*1024+texture_x+u]=uint16_t(red|(green<<5)|(blue<<10)|stp);
 }
}

void render_workload(ps1::Gpu& gpu,const Workload& workload){
 const uint32_t color0=workload.gouraud?0x00ff2040:0x0080a0c0;
 const uint32_t color1=workload.gouraud?0x0020e060:color0;
 const uint32_t color2=workload.gouraud?0x004060ff:color0;
 const ps1::Gpu::Vertex a{0,0,0,0,color0};
 const ps1::Gpu::Vertex b{319,0,255,0,color1};
 const ps1::Gpu::Vertex c{0,239,0,255,color2};
 for(unsigned n=0;n<raster_iterations;n++)
  gpu.render_triangle(a,b,c,0,workload.textured,false,workload.semi,false);
}

double median(std::vector<double> values){
 std::sort(values.begin(),values.end());
 const size_t middle=values.size()/2;
 return values.size()%2?values[middle]:(values[middle-1]+values[middle])*0.5;
}

void benchmark_workload(ps1::Gpu& gpu,const Workload& workload){
 std::vector<double> raster_ms,wall_ms;
 uint64_t expected_hash=0;
 for(unsigned run=0;run<=measured_runs;run++){
  prepare(gpu,workload.textured);
  const auto start=std::chrono::steady_clock::now();
  render_workload(gpu,workload);
  const auto end=std::chrono::steady_clock::now();
  const double wall=std::chrono::duration<double,std::milli>(end-start).count();
  const double raster=double(gpu.raster_time_ns)/1000000.0;
  const uint64_t hash=hash_vram(gpu);
  if(run==0)expected_hash=hash;
  else if(hash!=expected_hash)throw std::runtime_error(std::string(workload.name)+" produced a non-repeatable full-VRAM hash");
  const char* label=run==0?"warmup":"run";
  std::cout<<workload.name<<" "<<label;
  if(run)std::cout<<"="<<run;
  std::cout<<" raster_ms="<<std::fixed<<std::setprecision(3)<<raster
		   <<" wall_ms="<<wall<<" hash=0x"<<std::hex<<hash<<std::dec<<"\n";
  if(run){raster_ms.push_back(raster);wall_ms.push_back(wall);}
 }
 const auto raster_minmax=std::minmax_element(raster_ms.begin(),raster_ms.end());
 const auto wall_minmax=std::minmax_element(wall_ms.begin(),wall_ms.end());
 std::cout<<workload.name<<" summary raster_ms median="<<median(raster_ms)
		  <<" min="<<*raster_minmax.first<<" max="<<*raster_minmax.second
		  <<" wall_ms median="<<median(wall_ms)
		  <<" min="<<*wall_minmax.first<<" max="<<*wall_minmax.second
		  <<" hash=0x"<<std::hex<<expected_hash<<std::dec<<"\n";
}

void benchmark_snapshots(const ps1::Gpu& gpu){
 ps1::Gpu snapshot;
 const ps1::DisplayView view{0,2,640,478,false,false};
 constexpr unsigned copies=256;
 volatile uint64_t checksum=0;
 const auto full_start=std::chrono::steady_clock::now();
 for(unsigned n=0;n<copies;n++){
  std::copy(gpu.vram.begin(),gpu.vram.end(),snapshot.vram.begin());
  checksum=checksum+snapshot.vram[n%snapshot.vram.size()];
 }
 const double full_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-full_start).count();
 const auto display_start=std::chrono::steady_clock::now();
 for(unsigned n=0;n<copies;n++){
  ps1::snapshot_display_vram(gpu,snapshot,view);
  checksum=checksum+snapshot.vram[n%snapshot.vram.size()];
 }
 const double display_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-display_start).count();
 std::cout<<"snapshot-copy full-VRAM="<<full_ms<<" ms display-640x478="<<display_ms
		  <<" ms checksum="<<checksum<<"\n";
}
}

int main(){
 try{
  ps1::Gpu gpu;
  for(const auto& workload:workloads)benchmark_workload(gpu,workload);
  benchmark_snapshots(gpu);
 }catch(const std::exception& error){
  std::cerr<<"GPU benchmark failed: "<<error.what()<<"\n";
  return 1;
 }
 return 0;
}
