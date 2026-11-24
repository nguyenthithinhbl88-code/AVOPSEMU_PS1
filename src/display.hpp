#pragma once
#include "gpu.hpp"
#include <algorithm>
#include <cstddef>
namespace ps1 {
// Functional presentation only: no video clocks, field timing or CRT overscan.
struct DisplayView {unsigned x=0,y=0,width=320,height=240;bool rgb24=false,disabled=false;};
inline DisplayView display_view(const Gpu& gpu){
 DisplayView v;if(!gpu.display_configured)return v;
 const auto mode=gpu.display[3];
 const unsigned widths[]={256,320,512,640},clocks[]={10,8,5,4};
 v.x=gpu.display[0]&1023;v.y=(gpu.display[0]>>10)&511;
 v.width=(mode&64)?368:widths[mode&3];
 const unsigned x1=gpu.display[1]&4095,x2=(gpu.display[1]>>12)&4095;
 if(x2>x1)v.width=std::clamp((((x2-x1)/((mode&64)?7:clocks[mode&3]))+2)&~3u,4u,1024u);
 const unsigned y1=gpu.display[2]&1023,y2=(gpu.display[2]>>10)&1023;
 const unsigned scale=(mode&0x24)==0x24?2:1;
 v.height=std::clamp((y2>y1?y2-y1:240)*scale,1u,512u);
 v.rgb24=mode&16;v.disabled=(gpu.status&(1u<<23))!=0||((gpu.horizontal_range_set||gpu.display[1])&&x2<=x1)||((gpu.vertical_range_set||gpu.display[2])&&y2<=y1);return v;
}
inline uint32_t display_pixel(const Gpu& gpu,const DisplayView& v,unsigned x,unsigned y){
 if(v.disabled)return 0;
 if(!v.rgb24)return gpu.rgb(v.x+x,v.y+y);
 const unsigned row=((v.y+y)&511)*1024;
 auto byte=[&](unsigned i){i&=2047;return unsigned(gpu.vram[row+i/2]>>((i&1)*8))&255;};
 const unsigned start=v.x*2+x*3;
 return (byte(start)<<16)|(byte(start+1)<<8)|byte(start+2);
}
inline void display_pixels(const Gpu& gpu,const DisplayView& view,uint32_t* output){
 if(!view.width||!view.height)return;
  const std::size_t pixel_count=std::size_t(view.width)*view.height;
 if(view.disabled){std::fill_n(output,pixel_count,0u);return;}
 if(!view.rgb24){
  const unsigned start=view.x&1023,first=std::min(view.width,1024-start);
  for(unsigned y=0;y<view.height;y++){
   const auto* source=gpu.vram.data()+((view.y+y)&511)*1024;
	  auto* destination=output+std::size_t(y)*view.width;
   for(unsigned x=0;x<first;x++)destination[x]=Gpu::rgb555_to_rgb888(source[start+x]);
   for(unsigned x=first;x<view.width;x++)destination[x]=Gpu::rgb555_to_rgb888(source[x-first]);
  }
  return;
 }
 for(unsigned y=0;y<view.height;y++){
  const unsigned row=((view.y+y)&511)*1024;unsigned byte_index=(view.x*2)&2047;
	auto* destination=output+std::size_t(y)*view.width;
  for(unsigned x=0;x<view.width;x++){
   const auto byte=[&](){const unsigned index=byte_index&2047;const unsigned value=(gpu.vram[row+(index>>1)]>>((index&1)*8))&255;++byte_index;return value;};
   const unsigned r=byte(),g=byte(),b=byte();destination[x]=(r<<16)|(g<<8)|b;
  }
 }
}
inline void snapshot_display_vram(const Gpu& source,Gpu& snapshot,const DisplayView& view){
 if(view.disabled)return;
 const unsigned row_words=view.rgb24?(view.width*3+1)/2:view.width;
 const unsigned copy_words=std::min(row_words,1024u);
 for(unsigned y=0;y<view.height;y++){
  const unsigned row=((view.y+y)&511)*1024;
  unsigned x=view.x&1023,remaining=copy_words;
  while(remaining){
   const unsigned count=std::min(remaining,1024-x),offset=row+x;
   std::copy_n(source.vram.begin()+offset,count,snapshot.vram.begin()+offset);
   remaining-=count;x=0;
  }
 }
}
struct ViewRect {int x=0,y=0,width=0,height=0;};
inline ViewRect fit_display(int width,int height,bool stretch=false){
 if(width<=0||height<=0)return {};
  if(stretch)return {0,0,width,height};
 int w=width,h=width*3/4;if(h>height){h=height;w=height*4/3;}
 return {(width-w)/2,(height-h)/2,w,h};
}
}
