#include "core.hpp"
#include "display.hpp"
#include <stdexcept>
#include <iostream>
#include <vector>
void check(bool v){if(!v)throw std::runtime_error("GPU test failed");}
void check_display_snapshot(const ps1::Gpu& source,const ps1::DisplayView& view){
 ps1::Gpu snapshot;ps1::snapshot_display_vram(source,snapshot,view);
 for(unsigned y=0;y<view.height;y++)for(unsigned x=0;x<view.width;x++)
  check(ps1::display_pixel(source,view,x,y)==ps1::display_pixel(snapshot,view,x,y));
}
void check_display_conversion(const ps1::Gpu& gpu,const ps1::DisplayView& view){
  std::vector<uint32_t> pixels(std::size_t(view.width)*view.height);ps1::display_pixels(gpu,view,pixels.data());
 for(unsigned y=0;y<view.height;y++)for(unsigned x=0;x<view.width;x++)
  check(pixels[y*view.width+x]==ps1::display_pixel(gpu,view,x,y));
}
int main(){try{
 ps1::Bus b;ps1::Cpu c(b);
 auto send=[&](uint32_t v){b.write32(0xbf801810,v);};
	 ps1::Gpu lut_gpu;
   for(unsigned pixel=0;pixel<65536;pixel++){
   const unsigned r=pixel&31,g=(pixel>>5)&31,blue=(pixel>>10)&31;
   const uint32_t expected=(((r<<3)|(r>>2))<<16)|(((g<<3)|(g>>2))<<8)|((blue<<3)|(blue>>2));
	  lut_gpu.vram[pixel]=uint16_t(pixel);check(lut_gpu.rgb(pixel&1023,pixel>>10)==expected);
  }
 // Fill waits for complete packet, aligns X down and rounds width up to 16.
 send(0x020000ff);send((511u<<16)|1023);check(b.gpu.vram[511*1024+1008]==0);
 send((2u<<16)|17);check(b.gpu.vram[511*1024+1008]==31);check(b.gpu.vram[0]==31);check(b.gpu.vram[16]==0);check(b.gpu.rgb(0,0)==0xff0000);
 // Zero-height fill draws nothing.
 send(0x0200ff00);send(0);send(16);check(b.gpu.vram[0]==31);
 // Odd transfer, wraps at both edges, discards last unused halfword.
 send(0xa0000000);send((511u<<16)|1023);send((1u<<16)|3);
 send(0x12348001);send(0xffff4321);
 check(b.gpu.vram[511*1024+1023]==0x8001);check(b.gpu.vram[511*1024]==0x1234);check(b.gpu.vram[511*1024+1]==0x4321);
 send(0); // This must be NOP, not more transfer data.
 send(0xa0000000);send((511u<<16)|1023);send((2u<<16)|1);send(0x56781234);check(b.gpu.vram[1023]==0x5678);
 // Reset command parser aborts an incomplete packet, preserves VRAM.
 send(0x020000ff);b.write32(0x1f801814,0x01000000);send(0);check(b.gpu.count==0);
 b.write32(0x1f801814,0x03000000);check(!(b.read32(0x1f801814)&(1u<<23)));
 b.write32(0x1f801814,0);check(b.gpu.status==0x14802000&&b.gpu.vram[1023]==0x5678);
 bool rejected=false;try{send(0x90000000);}catch(const std::runtime_error&){rejected=true;}check(rejected);
 const unsigned n=ps1::install_gpu_demo(b);c.reset(0);for(unsigned i=0;i<n;i++)c.step();
 check(b.gpu.rgb(32,32)==0xff4242);check(b.gpu.rgb(128,32)==0x42e742);check(b.gpu.rgb(208,32)==0x3184f7);check(b.gpu.rgb(32,208)==0xffffff);
 // Display origin, 640x480 BIOS mode and framebuffer page selection.
 auto& g=b.gpu;g.reset();auto view=ps1::display_view(g);check(view.width==320&&view.height==240&&!view.disabled);
 g.gp1(0x05000000|100|(2<<10));g.gp1(0x06000000|620|(3180<<12));
 g.gp1(0x07000000|16|(256<<10));g.gp1(0x08000027);g.gp1(0x03000000);
 view=ps1::display_view(g);check(view.x==100&&view.y==2&&view.width==640&&view.height==480);
 g.vram[2*1024+100]=31;check(ps1::display_pixel(g,view,0,0)==0xff0000);
   check_display_conversion(g,view);
 g.gp1(0x05000000|100|(256<<10));view=ps1::display_view(g);g.vram[256*1024+100]=0x3e0;
 check(ps1::display_pixel(g,view,0,0)==0x00ff00);
   g.gp1(0x03000001);check(ps1::display_pixel(g,ps1::display_view(g),0,0)==0);check_display_conversion(g,ps1::display_view(g));g.gp1(0x03000000);
 // Mode fallback widths and invalid ranges cannot allocate unbounded frames.
 g.display[1]=g.display[2]=0;
 const unsigned widths[]={256,320,512,640};for(unsigned i=0;i<4;i++){g.gp1(0x08000000|i);check(ps1::display_view(g).width==widths[i]);}
 g.gp1(0x08000040);check(ps1::display_view(g).width==368);
 g.display[1]=4095<<12;g.display[2]=1023<<10;g.gp1(0x08000027);view=ps1::display_view(g);check(view.width<=1024&&view.height==512);
 // RGB24 byte packing crosses halfwords and wraps within the VRAM row.
 g.gp1(0x050003ff);g.gp1(0x08000010);view=ps1::display_view(g);
   g.vram[1023]=0x2211;g.vram[0]=0x4433;g.vram[1]=0x6655;
  check(ps1::display_pixel(g,view,0,0)==0x112233&&ps1::display_pixel(g,view,1,0)==0x445566);check_display_conversion(g,view);
	 g.reset();for(unsigned i=0;i<g.vram.size();i++)g.vram[i]=uint16_t(i*37u^(i>>9));
  g.gp1(0x05000000|1000|(500<<10));g.gp1(0x08000003);g.gp1(0x03000000);
	 view=ps1::display_view(g);check(view.x==1000&&view.y==500&&view.width==640);check_display_snapshot(g,view);check_display_conversion(g,view);
  g.reset();for(unsigned i=0;i<g.vram.size();i++)g.vram[i]=uint16_t(i*37u^(i>>9));
  g.gp1(0x05000000|1000|(500<<10));g.gp1(0x06000000|(2560<<12));g.gp1(0x07000000|(480<<10));g.gp1(0x08000013);g.gp1(0x03000000);
	 view=ps1::display_view(g);check(view.rgb24&&view.x==1000&&view.y==500&&view.width==640&&view.height==480);check_display_snapshot(g,view);check_display_conversion(g,view);
 auto rect=ps1::fit_display(1200,600);check(rect.width==800&&rect.height==600&&rect.x==200&&rect.y==0);
 rect=ps1::fit_display(640,800);check(rect.width==640&&rect.height==480&&rect.y==160);
   rect=ps1::fit_display(1200,600,true);check(rect.x==0&&rect.y==0&&rect.width==1200&&rect.height==600);
  rect=ps1::fit_display(640,800,true);check(rect.x==0&&rect.y==0&&rect.width==640&&rect.height==800);
 rect=ps1::fit_display(0,0);check(rect.width==0&&rect.height==0);
 g.reset();check(!g.display_configured);
 std::cout<<"PASS: GPU packet, fill rounding/wrap, zero height, odd upload/wrap, reset, GPUSTAT subset, CPU GPU demo, GP1 display source/modes/RGB24/blanking, 4:3 fit\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
