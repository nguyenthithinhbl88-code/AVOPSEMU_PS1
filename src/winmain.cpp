#include <windows.h>
#include <commdlg.h>
#include "ui_theme.hpp"
#include "ui_layout.hpp"
#include "frontend_settings.hpp"
#include "frontend_library.hpp"
#include "core.hpp"
#include "display.hpp"
#include "diagnostic_capture.hpp"
#include "runner.hpp"
#include "audio_win32.hpp"
#include "win_crash_diagnostics.hpp"
#include "keyboard_pad.hpp"
#include "performance_metrics.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <memory>
#include <chrono>
#include <atomic>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {
namespace win_diag=ps1::win_diag;
std::unique_ptr<ps1::Bus> bus;
std::unique_ptr<ps1::Cpu> cpu;
std::mutex machine_mutex;
std::unique_ptr<ps1::Runner> runner;
ps1::AudioOutput audio_output;
std::vector<uint32_t> presentation_pixels;
ps1::Gpu presentation; // UI-owned VRAM snapshot; convert RGB after releasing the core lock
bool limit_speed=true,muted=false;
avopsui::Preferences preferences;
std::filesystem::path preferences_path;
HWND settings_button=nullptr,settings_window=nullptr,library_button=nullptr,library_window=nullptr;
HMENU main_menu=nullptr;
std::unique_ptr<avopsui::GameLibrary> game_library;
std::filesystem::path game_library_path,current_disc_path;
bool library_error_reported=false,game_booted=false;
ULONGLONG last_library_refresh=0;
bool toolbar_visible=true,status_visible=true,fps_hud_visible=true;
uint64_t painted_words=~uint64_t(0),painted_display=~uint64_t(0),painted_video=~uint64_t(0);
HWND output,screen,pad_hint;
ps1::KeyboardPad keyboard;
ps1::HostPad host_pad;
bool wasd_profile=false;
uint64_t input_events=0,input_samples=0;
uint16_t last_host_buttons=0xffff,mouse_pad_buttons=0xffff;
ULONGLONG mouse_pad_until=0;
bool input_focused=false;
std::array<HWND,22> buttons{};
bool large_view=false,debug_visible=false,show_welcome=true;
bool fullscreen=false,windowed_placement_saved=false;
WINDOWPLACEMENT windowed_placement{sizeof(WINDOWPLACEMENT)};
LONG_PTR windowed_style=0,fullscreen_style=0;
constexpr unsigned fullscreen_command=136;
HFONT ui_font=nullptr,title_font=nullptr,small_font=nullptr;
HBRUSH background_brush=nullptr,panel_brush=nullptr;
void paint_shell(HWND w,HDC dc);

std::wstring bios_name=L"Chưa nạp BIOS",disc_name=L"Chưa mở đĩa";
avopsui::Layout shell_layout;
void persist_preferences(HWND w){
 if(preferences_path.empty())return;
 std::wstring error;
 if(!avopsui::write_preferences(preferences_path,preferences,error)){
  win_diag::write_event("Settings","Could not save user preferences",true);
  MessageBoxW(w,error.c_str(),L"AVOPSEMU settings could not be saved",MB_OK|MB_ICONERROR);
 }
}
void save_keyboard(HWND w,bool=false){
 preferences.wasd_profile=wasd_profile;preferences.neutral_opposites=keyboard.neutral_opposites;preferences.keyboard_keys=keyboard.keys;persist_preferences(w);
}
void layout(HWND w){
  RECT r{};GetClientRect(w,&r);
  if(fullscreen){
   for(auto button:buttons)ShowWindow(button,SW_HIDE);
   if(settings_button)ShowWindow(settings_button,SW_HIDE);if(library_button)ShowWindow(library_button,SW_HIDE);
   if(output)ShowWindow(output,SW_HIDE);if(pad_hint)ShowWindow(pad_hint,SW_HIDE);
   if(screen){ShowWindow(screen,SW_SHOW);MoveWindow(screen,0,0,std::max(1L,r.right),std::max(1L,r.bottom),TRUE);}
   InvalidateRect(w,nullptr,FALSE);if(screen)InvalidateRect(screen,nullptr,FALSE);return;
  }
  shell_layout=avopsui::layout(r.right,r.bottom,large_view,debug_visible);
 for(auto button:buttons)ShowWindow(button,SW_HIDE);
  if(settings_button)ShowWindow(settings_button,SW_HIDE);if(library_button)ShowWindow(library_button,SW_HIDE);
  const unsigned toolbar[]={1,11,15,4,5,21};
  const int button_width=std::min(164,(static_cast<int>(r.right)-40-7*8)/8);
 if(toolbar_visible){for(unsigned n=0;n<6;n++){auto button=buttons[toolbar[n]];ShowWindow(button,SW_SHOW);MoveWindow(button,20+int(n)*(button_width+8),94,button_width,40,TRUE);}
   if(library_button){ShowWindow(library_button,SW_SHOW);MoveWindow(library_button,20+6*(button_width+8),94,button_width,40,TRUE);}
   if(settings_button){ShowWindow(settings_button,SW_SHOW);MoveWindow(settings_button,20+7*(button_width+8),94,button_width,40,TRUE);}}
 const unsigned side[]={18,19,13,10};
 if(!large_view)for(unsigned n=0;n<4;n++){auto button=buttons[side[n]];ShowWindow(button,SW_SHOW);MoveWindow(button,36,shell_layout.sidebar.y+256+int(n)*42,192,34,TRUE);}
 auto move=[](HWND child,const avopsui::Rect& r){MoveWindow(child,r.x,r.y,r.width,r.height,TRUE);};
 ShowWindow(output,debug_visible?SW_SHOW:SW_HIDE);if(debug_visible)move(output,shell_layout.debug);
 move(screen,shell_layout.screen);move(pad_hint,shell_layout.hint);
 InvalidateRect(w,nullptr,FALSE);InvalidateRect(screen,nullptr,FALSE);
}

std::wstring display_api_error(const wchar_t* operation,DWORD code){
 return std::wstring(operation)+L" failed (Windows error "+std::to_wstring(code)+L").";
}
void save_windowed_size(const WINDOWPLACEMENT& placement){
 const LONG width=placement.rcNormalPosition.right-placement.rcNormalPosition.left;
 const LONG height=placement.rcNormalPosition.bottom-placement.rcNormalPosition.top;
 if(width>0&&height>0){preferences.window_width=static_cast<unsigned>(std::clamp(width,1040L,8192L));preferences.window_height=static_cast<unsigned>(std::clamp(height,780L,8192L));}
 preferences.window_maximized=placement.showCmd==SW_SHOWMAXIMIZED;
}
bool set_window_style(HWND w,LONG_PTR style,std::wstring& error){
 SetLastError(ERROR_SUCCESS);const LONG_PTR previous=SetWindowLongPtrW(w,GWL_STYLE,style);const DWORD code=GetLastError();
 if(!previous&&code!=ERROR_SUCCESS){error=display_api_error(L"SetWindowLongPtrW",code);return false;}
 return true;
}
bool position_fullscreen(HWND w,std::wstring& error){
 const HMONITOR monitor=MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST);MONITORINFO info{};info.cbSize=sizeof(info);
 if(!monitor||!GetMonitorInfoW(monitor,&info)){error=display_api_error(L"GetMonitorInfoW",GetLastError());return false;}
 if(!SetWindowPos(w,HWND_TOP,info.rcMonitor.left,info.rcMonitor.top,info.rcMonitor.right-info.rcMonitor.left,info.rcMonitor.bottom-info.rcMonitor.top,SWP_NOOWNERZORDER|SWP_FRAMECHANGED)){
  error=display_api_error(L"SetWindowPos fullscreen",GetLastError());return false;
 }
 return true;
}
bool set_fullscreen_window(HWND w,bool enable,std::wstring& error){
 if(fullscreen==enable)return true;
 error.clear();
 if(enable){
  WINDOWPLACEMENT placement{};placement.length=sizeof(placement);
  if(!GetWindowPlacement(w,&placement)){error=display_api_error(L"GetWindowPlacement",GetLastError());return false;}
  SetLastError(ERROR_SUCCESS);const LONG_PTR style=GetWindowLongPtrW(w,GWL_STYLE);const DWORD style_error=GetLastError();
  if(!style&&style_error!=ERROR_SUCCESS){error=display_api_error(L"GetWindowLongPtrW",style_error);return false;}
    if(main_menu&&(!SetMenu(w,nullptr)||!DrawMenuBar(w))){error=display_api_error(L"SetMenu fullscreen",GetLastError());if(main_menu){SetMenu(w,main_menu);DrawMenuBar(w);}return false;}
  windowed_style=style;
  fullscreen_style=(style&~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW|WS_MINIMIZE|WS_MAXIMIZE))|WS_POPUP|WS_CLIPCHILDREN;
   if(!set_window_style(w,fullscreen_style,error)){
     if(main_menu){SetMenu(w,main_menu);DrawMenuBar(w);}
    return false;
   }
  if(!position_fullscreen(w,error)){
   std::wstring rollback_error;
    if(!set_window_style(w,windowed_style,rollback_error)||!SetWindowPlacement(w,&placement))win_diag::write_event("Fullscreen","Could not restore window style after a failed fullscreen transition",true);
    if(main_menu){SetMenu(w,main_menu);DrawMenuBar(w);}
   return false;
  }
  windowed_placement=placement;windowed_placement_saved=true;fullscreen=true;save_windowed_size(placement);
  return true;
 }
 if(!windowed_placement_saved){error=L"No saved windowed placement is available.";return false;}
 if(!set_window_style(w,windowed_style,error))return false;
 if(!SetWindowPlacement(w,&windowed_placement)){
  error=display_api_error(L"SetWindowPlacement",GetLastError());std::wstring rollback_error;
  if(!set_window_style(w,fullscreen_style,rollback_error)||!position_fullscreen(w,rollback_error))win_diag::write_event("Fullscreen","Could not restore fullscreen after a failed window restore",true);
  return false;
 }
 if(!SetWindowPos(w,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED)){
  error=display_api_error(L"SetWindowPos restore",GetLastError());std::wstring rollback_error;
  if(!set_window_style(w,fullscreen_style,rollback_error)||!position_fullscreen(w,rollback_error))win_diag::write_event("Fullscreen","Could not restore fullscreen after a failed frame refresh",true);
  return false;
 }
 if(main_menu&&(!SetMenu(w,main_menu)||!DrawMenuBar(w))){
  error=display_api_error(L"Restore menu bar",GetLastError());std::wstring rollback_error;
  if(!set_window_style(w,fullscreen_style,rollback_error)||!position_fullscreen(w,rollback_error))win_diag::write_event("Fullscreen","Could not restore fullscreen after a menu failure",true);
  SetMenu(w,nullptr);DrawMenuBar(w);return false;
 }
 fullscreen=false;windowed_placement_saved=false;
 return true;
}
void show_fullscreen_error(HWND w,const std::wstring& error){
 win_diag::write_event("Fullscreen","Window mode transition failed",true);
 MessageBoxW(w,error.c_str(),L"AVOPSEMU display settings",MB_OK|MB_ICONERROR);
}
void toggle_fullscreen(HWND w){
 std::wstring error;
 if(!set_fullscreen_window(w,!fullscreen,error)){preferences.fullscreen=fullscreen;show_fullscreen_error(w,error);}
 else preferences.fullscreen=fullscreen;
 layout(w);
 if(GetMenu(w))CheckMenuItem(GetMenu(w),fullscreen_command,MF_BYCOMMAND|(fullscreen?MF_CHECKED:MF_UNCHECKED));
 if(screen)InvalidateRect(screen,nullptr,FALSE);
 persist_preferences(w);
 if(settings_window)avopsui::refresh_settings_window(settings_window,preferences.font_size);
}

HFONT font;
uint32_t entry=0;
bool running=false;
ULONGLONG last_refresh=0;
bool exe_loaded=false,has_bios=false;
ULONGLONG perf_time=0;
uint64_t perf_frames=0,perf_instructions=0;
double video_hz=0,core_mips=0;
using PerfClock=std::chrono::steady_clock;
ps1::DisplayFrameMetrics display_frame_metrics;
ps1::DisplayFrameSummary display_frame_summary;
uint64_t ui_mutex_wait_ns=0,paint_work_ns=0,perf_process_cpu_ns=0,perf_core_time_ns=0,perf_gpu_time_ns=0,perf_audio_time_ns=0,perf_runner_wait_ns=0,perf_audio_wait_ns=0,perf_ui_wait_ns=0,perf_paint_work_ns=0;
double process_cpu_ms_per_second=0,core_time_ms_per_second=0,gpu_time_ms_per_second=0,audio_time_ms_per_second=0,runner_wait_ms_per_second=0,audio_wait_ms_per_second=0,ui_wait_ms_per_second=0,paint_time_ms_per_second=0;
bool perf_process_cpu_valid=false,process_cpu_interval_valid=false;
DWORD process_cpu_last_error=0;
uint64_t duration_ns(PerfClock::duration duration){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());}
bool read_process_cpu_nanoseconds(uint64_t& nanoseconds){
 FILETIME created{},exited{},kernel{},user{};
 if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)){process_cpu_last_error=GetLastError();return false;}
 ULARGE_INTEGER kernel_time{},user_time{};kernel_time.LowPart=kernel.dwLowDateTime;kernel_time.HighPart=kernel.dwHighDateTime;
 user_time.LowPart=user.dwLowDateTime;user_time.HighPart=user.dwHighDateTime;
 nanoseconds=(kernel_time.QuadPart+user_time.QuadPart)*100;process_cpu_last_error=0;return true;
}
void start_perf(){
 std::lock_guard<std::mutex> lock(machine_mutex);
 perf_time=GetTickCount64();perf_frames=bus->gpu.video_frames;perf_instructions=cpu->instructions;video_hz=core_mips=0;
 bus->gpu.raster_time_ns=0;
 display_frame_metrics.reset();display_frame_summary={};ui_mutex_wait_ns=paint_work_ns=0;
 perf_process_cpu_ns=0;perf_process_cpu_valid=read_process_cpu_nanoseconds(perf_process_cpu_ns);process_cpu_interval_valid=perf_process_cpu_valid;
 perf_core_time_ns=perf_gpu_time_ns=perf_audio_time_ns=perf_runner_wait_ns=perf_audio_wait_ns=perf_ui_wait_ns=perf_paint_work_ns=0;
 process_cpu_ms_per_second=core_time_ms_per_second=gpu_time_ms_per_second=audio_time_ms_per_second=runner_wait_ms_per_second=audio_wait_ms_per_second=ui_wait_ms_per_second=paint_time_ms_per_second=0;
}
void sample_perf(){
 const auto now=GetTickCount64();if(now-perf_time<1000)return;
 const auto wait_start=PerfClock::now();
 std::lock_guard<std::mutex> lock(machine_mutex);
 ui_mutex_wait_ns+=duration_ns(PerfClock::now()-wait_start);
 const double seconds=double(now-perf_time)/1000;
 if(bus->gpu.video_frames>=perf_frames&&cpu->instructions>=perf_instructions){video_hz=double(bus->gpu.video_frames-perf_frames)/seconds;core_mips=double(cpu->instructions-perf_instructions)/seconds/1000000;}
 uint64_t current_process_cpu=0;const bool current_process_cpu_valid=read_process_cpu_nanoseconds(current_process_cpu);
 const uint64_t current_core_time=runner?runner->core_time_nanoseconds():0;
 const uint64_t current_gpu_time=bus->gpu.raster_time_ns;
 const uint64_t current_audio_time=audio_output.processing_nanoseconds();
 const uint64_t current_runner_wait=runner?runner->machine_lock_wait_nanoseconds():0;
 const uint64_t current_audio_wait=audio_output.machine_lock_wait_nanoseconds();
 const uint64_t current_ui_wait=ui_mutex_wait_ns;
 const auto delta=[](uint64_t current,uint64_t previous){return current>=previous?current-previous:current;};
 process_cpu_interval_valid=current_process_cpu_valid&&perf_process_cpu_valid;
 process_cpu_ms_per_second=process_cpu_interval_valid?double(delta(current_process_cpu,perf_process_cpu_ns))/1000000.0/seconds:0;
 core_time_ms_per_second=double(delta(current_core_time,perf_core_time_ns))/1000000.0/seconds;
 gpu_time_ms_per_second=double(delta(current_gpu_time,perf_gpu_time_ns))/1000000.0/seconds;
 audio_time_ms_per_second=double(delta(current_audio_time,perf_audio_time_ns))/1000000.0/seconds;
 runner_wait_ms_per_second=double(delta(current_runner_wait,perf_runner_wait_ns))/1000000.0/seconds;
 audio_wait_ms_per_second=double(delta(current_audio_wait,perf_audio_wait_ns))/1000000.0/seconds;
 ui_wait_ms_per_second=double(delta(current_ui_wait,perf_ui_wait_ns))/1000000.0/seconds;
 paint_time_ms_per_second=double(delta(paint_work_ns,perf_paint_work_ns))/1000000.0/seconds;
 display_frame_summary=display_frame_metrics.summary();
 if(current_process_cpu_valid)perf_process_cpu_ns=current_process_cpu;
 perf_process_cpu_valid=current_process_cpu_valid;perf_core_time_ns=current_core_time;perf_gpu_time_ns=current_gpu_time;perf_audio_time_ns=current_audio_time;
 perf_runner_wait_ns=current_runner_wait;perf_audio_wait_ns=current_audio_wait;perf_ui_wait_ns=current_ui_wait;perf_paint_work_ns=paint_work_ns;
 perf_time=now;perf_frames=bus->gpu.video_frames;perf_instructions=cpu->instructions;
}
void update_pad(HWND w,bool sample_physical=false){
 const bool focused=GetAncestor(GetForegroundWindow(),GA_ROOT)==w;
 input_focused=focused;
 if(sample_physical){keyboard.sample([](unsigned key){return (GetAsyncKeyState(int(key))&0x8000)!=0;},focused);input_samples++;}
 if(!focused){keyboard.release();mouse_pad_buttons=0xffff;host_pad.clear();}
 if(GetTickCount64()>=mouse_pad_until)mouse_pad_buttons=0xffff;
 const auto host=uint16_t(keyboard.buttons(focused)&(focused?mouse_pad_buttons:0xffff));
 if(host!=0xffff)last_host_buttons=host;
 host_pad.publish(host);
}
std::array<uint32_t,32> initial_regs{};
std::wstring last_fault;
std::wstring status=L"Sẵn sàng • Nạp BIOS → Mở đĩa → Boot đĩa → Chạy. Z = Cross, Enter = Start.";
void persist_library(HWND w){
 if(!game_library)return;std::wstring error;
 if(!game_library->save(error)){
  if(!library_error_reported){const int bytes=WideCharToMultiByte(CP_UTF8,0,error.c_str(),-1,nullptr,0,nullptr,nullptr);std::string message="Game-library persistence failed";if(bytes>1){message.resize(static_cast<size_t>(bytes));WideCharToMultiByte(CP_UTF8,0,error.c_str(),-1,message.data(),bytes,nullptr,nullptr);message.resize(static_cast<size_t>(bytes-1));}win_diag::write_event("Game library",message.c_str(),true);}
  library_error_reported=true;if(w)status=L"Game library could not be saved; see the diagnostics log.";
 }else library_error_reported=false;
}
void refresh(){
 // Hidden diagnostics must not format every register or send Win32 messages under the core mutex.
 if(!debug_visible){InvalidateRect(GetParent(screen),nullptr,FALSE);if(screen&&!running)InvalidateRect(screen,nullptr,FALSE);return;}
 const auto wait_start=PerfClock::now();
 std::unique_lock<std::mutex> lock(machine_mutex);
 ui_mutex_wait_ns+=duration_ns(PerfClock::now()-wait_start);
 std::wostringstream s;
 s<<L"AVOPSEMU PS1 v51 | PS1 DEBUGGER\r\n"<<status<<L"\r\n\r\n";
  s<<L"Video Hz (model): "<<std::fixed<<std::setprecision(2)<<video_hz<<L" | CPU MIPS: "<<core_mips<<L" | Process CPU: ";
  if(process_cpu_interval_valid)s<<process_cpu_ms_per_second<<L" ms/s";else if(process_cpu_last_error)s<<L"N/A (GetProcessTimes error "<<process_cpu_last_error<<L")";else s<<L"warming up";
  s<<L"\r\n";
  s<<L"GDI submitted FPS: "<<display_frame_summary.fps<<L" | Frame interval avg/p95 (last <=120): "<<display_frame_summary.average_ms<<L"/"<<display_frame_summary.p95_ms<<L" ms | Late (>budget, since reset): "<<display_frame_summary.late_frames<<L"\r\n";
  s<<L"Elapsed work (ms/s; GPU is included in Runner): Runner batches "<<core_time_ms_per_second<<L" | GPU raster "<<gpu_time_ms_per_second<<L" | audio worker "<<audio_time_ms_per_second<<L" | UI paint "<<paint_time_ms_per_second<<L"\r\n";
  s<<L"Mutex wait (thread-ms/s): Runner "<<runner_wait_ms_per_second<<L" | audio "<<audio_wait_ms_per_second<<L" | UI "<<ui_wait_ms_per_second<<L"\r\n";
 s<<L"PC: "<<std::hex<<std::setfill(L'0')<<std::setw(8)<<cpu->pc<<L"    Next: "<<std::setw(8)<<cpu->next_pc<<L"\r\n\r\n";
 s<<L"Boot path: "<<(bus->direct_exe_boot?L"DIRECT EXE (no BIOS initialization)":L"Machine / BIOS reset")<<L"\r\n";
 s<<L"IRQ exceptions: "<<std::dec<<cpu->interrupts<<L" | Reserved: "<<cpu->reserved_exceptions<<L"\r\n";
 s<<L"GTE commands: "<<cpu->gte.commands<<L"\r\n";
 s<<L"MDEC blocks: "<<bus->mdec.macroblocks<<L" | Status: "<<std::hex<<bus->mdec.status()<<std::dec<<L"\r\n";
 if(cpu->first_fault.valid)s<<L"First CPU fault: "<<std::dec<<cpu->first_fault.code<<L" PC="<<std::hex<<cpu->first_fault.pc<<L" opcode="<<cpu->first_fault.opcode<<L"\r\n";
 s<<std::hex;
 s<<L"HI: "<<std::setw(8)<<cpu->hi<<L" LO: "<<std::setw(8)<<cpu->lo<<L"\r\n";
 s<<L"SR: "<<std::setw(8)<<cpu->cop0[12]<<L" Cause: "<<std::setw(8)<<cpu->cop0[13]<<L" EPC: "<<std::setw(8)<<cpu->cop0[14]<<L"\r\n\r\n";
 for(unsigned i=0;i<32;i++){
  s<<L"R"<<std::dec<<std::setfill(L'0')<<std::setw(2)<<i<<L"  "<<std::hex<<std::setw(8)<<cpu->r[i]<<((i%2==1)?L"\r\n":L"    ");
 }
 s<<L"\r\nInstructions: "<<std::dec<<cpu->instructions<<L" POST: "<<std::hex<<unsigned(bus->post)<<L"\r\n";
 s<<L"\r\nPAD1 host="<<std::hex<<host_pad.held_buttons()<<L" pending="<<host_pad.pending_presses()<<L" last_host="<<last_host_buttons<<L" reply="<<bus->sio[0].latched_buttons<<L" last_pressed_reply="<<bus->sio[0].last_pressed_buttons<<L"\r\n";
 s<<L"Host focus="<<input_focused<<L" events="<<std::dec<<input_events<<L" samples="<<input_samples<<L" | SIO polls="<<bus->sio[0].polls<<L" complete="<<bus->sio[0].completed_polls<<L" pressed="<<bus->sio[0].pressed_polls<<L" CTRL="<<std::hex<<bus->sio[0].control<<L" phase="<<std::dec<<bus->sio[0].phase<<L"\r\n";
 s<<L"\r\nDMA words: "<<std::dec<<bus->dma_words<<L"\r\n";
 const auto disc_info=bus->cd.disc.description();s<<L"Disc: "<<std::wstring(disc_info.begin(),disc_info.end())<<L"\r\n";
 s<<L"CD sectors: "<<std::dec<<bus->cd.sectors<<L" Read: "<<bus->cd.sectors_read<<L" Commands: "<<bus->cd.commands<<L"\r\n";
 s<<L"Video frames: "<<bus->gpu.video_frames<<L" Line: "<<bus->gpu.video_line<<L" VBlank: "<<bus->gpu.vblank_events<<L"\r\n";
 s<<L"GPU polygons: "<<bus->gpu.polygons<<L" Sprites: "<<bus->gpu.rectangles<<L"\r\n";
 s<<L"SPU writes: "<<bus->spu.writes<<L" | Voices: "<<bus->spu.active_voices()<<L" | Samples: "<<bus->spu.generated_frames<<L"\r\n";
 s<<L"Audio: "<<(audio_output.ready()?L"44100 Hz stereo / WSOLA":L"device closed")<<L" | MMCSS: "<<audio_output.multimedia_scheduled()<<L" | Underruns: "<<audio_output.underruns()<<L" | Tempo / WSOLA: "<<audio_output.ratio()<<L" | Buffered: "<<audio_output.buffered_frames()<<L" | Dropped: "<<(bus->spu.dropped_frames+audio_output.dropped())<<L" | Pace: "<<(limit_speed?L"1x model":L"Max")<<L"\r\n";
 s<<L"\r\nRAM[0x100]: "<<std::dec<<bus->read32(256)<<L"\r\n\r\n";
 const auto view=ps1::display_view(bus->gpu);
 s<<L"Display source: "<<view.x<<L","<<view.y<<L" "<<view.width<<L"x"<<view.height<<L" | Fit 4:3\r\n";
 s<<L"GPU: partial. SPU: ADPCM/ADSR stereo (partial). CD-ROM: ISO/BIN/CUE subset.\r\nBIOS co the dung o lenh hoac MMIO chua ho tro.\r\nPad: mui ten, Z/X/A/S, Enter/Shift. Homebrew direct boot; GTA1 da vao man choi tren may nguoi dung; tuong thich chua day du.";
 std::string tty=bus->tty;s<<L"\r\nTTY: "<<std::wstring(tty.begin(),tty.end());
 const auto text=s.str();lock.unlock();
 SetWindowTextW(output,text.c_str());
 InvalidateRect(GetParent(screen),nullptr,FALSE);
 if(screen&&!running)InvalidateRect(screen,nullptr,FALSE);
}
void stop(HWND w,bool finalize_session=false){
 host_pad.clear();mouse_pad_buttons=0xffff;mouse_pad_until=0;keyboard.release();update_pad(w);running=false;KillTimer(w,1);
 if(runner){runner->stop();runner.reset();}audio_output.close();
 if(game_library&&!current_disc_path.empty()){
  if(finalize_session)game_library->stop_session();else game_library->pause_session();
  persist_library(w);if(library_window)avopsui::refresh_game_library_window(library_window);
 }
}
void start_run(HWND w){
 show_welcome=false;
 start_perf();update_pad(w);
 {std::lock_guard<std::mutex> lock(machine_mutex);bus->spu.audio.clear();bus->sio[0].connected=true;bus->sio[0].host_input=&host_pad;}
 audio_output.set_muted(muted);
 const bool audio_ok=audio_output.open(bus->spu,machine_mutex);
 try{runner=std::make_unique<ps1::Runner>(*cpu,machine_mutex);runner->start(limit_speed);}
   catch(const std::exception& e){win_diag::write_event("Runner startup",e.what(),true);stop(w,true);std::string message=e.what();status=L"Run: "+std::wstring(message.begin(),message.end());refresh();return;}
 running=SetTimer(w,1,16,nullptr)!=0;
  if(!running){stop(w,true);status=L"Khong tao duoc UI timer.";}
  else{SetFocus(screen);status=audio_ok?L"Đang chạy • Âm thanh stereo 44,1 kHz.":L"Đang chạy • Không mở được thiết bị âm thanh Windows.";
   if(game_booted&&!current_disc_path.empty()&&game_library){game_library->begin_session(current_disc_path);persist_library(w);if(library_window)avopsui::refresh_game_library_window(library_window);}}
 refresh();
}
void rebuild_ui_resources(){
 if(background_brush)DeleteObject(background_brush);if(panel_brush)DeleteObject(panel_brush);
 background_brush=CreateSolidBrush(ps1ui::background);panel_brush=CreateSolidBrush(ps1ui::panel);
 if(ui_font)DeleteObject(ui_font);if(title_font)DeleteObject(title_font);if(small_font)DeleteObject(small_font);if(font)DeleteObject(font);
 const int size=static_cast<int>(preferences.font_size);
 ui_font=CreateFontW(-size,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 title_font=CreateFontW(-size*2,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 small_font=CreateFontW(-std::max(10,size*13/16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 font=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
 for(auto button:buttons)if(button)SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(ui_font),TRUE);
 if(settings_button)SendMessageW(settings_button,WM_SETFONT,reinterpret_cast<WPARAM>(ui_font),TRUE);
  if(library_button)SendMessageW(library_button,WM_SETFONT,reinterpret_cast<WPARAM>(ui_font),TRUE);
 if(pad_hint)SendMessageW(pad_hint,WM_SETFONT,reinterpret_cast<WPARAM>(small_font),TRUE);
 if(output)SendMessageW(output,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
}
void apply_preferences(HWND w){
 const bool restart=running&&limit_speed!=preferences.limit_speed;
 if(restart)stop(w);
 if(w&&preferences.fullscreen!=fullscreen){
  std::wstring error;
  if(!set_fullscreen_window(w,preferences.fullscreen,error)){preferences.fullscreen=fullscreen;show_fullscreen_error(w,error);}
 }
 limit_speed=preferences.limit_speed;muted=preferences.muted;wasd_profile=preferences.wasd_profile;
 keyboard.keys=preferences.keyboard_keys;keyboard.neutral_opposites=preferences.neutral_opposites;keyboard.release();host_pad.clear();
 large_view=!preferences.show_sidebar;debug_visible=preferences.show_debugger;toolbar_visible=preferences.show_toolbar;status_visible=preferences.show_status;fps_hud_visible=preferences.show_fps;
  avopsui::apply_palette(preferences.theme,preferences.accent);avopsui::set_game_library_font_size(preferences.font_size);rebuild_ui_resources();
 if(audio_output.ready())audio_output.set_muted(muted);
 if(w){SetWindowTextW(buttons[18],limit_speed?L"Tốc độ: 1x":L"Tốc độ: Max");SetWindowTextW(buttons[19],muted?L"Âm thanh: Tắt":L"Âm thanh: Bật");SetWindowTextW(buttons[13],large_view?L"Hiện thanh bên":L"Chế độ rạp");
   if(GetMenu(w)){CheckMenuItem(GetMenu(w),123,MF_BYCOMMAND|(debug_visible?MF_CHECKED:MF_UNCHECKED));CheckMenuItem(GetMenu(w),114,MF_BYCOMMAND|(large_view?MF_CHECKED:MF_UNCHECKED));CheckMenuItem(GetMenu(w),fullscreen_command,MF_BYCOMMAND|(fullscreen?MF_CHECKED:MF_UNCHECKED));}
  update_pad(w);layout(w);
 }
 if(restart)start_run(w);else if(w)refresh();
  if(settings_window)avopsui::refresh_settings_window(settings_window,preferences.font_size);
  if(library_window)avopsui::refresh_game_library_window(library_window);
 persist_preferences(w);
}
void advance(HWND w,unsigned n){
 show_welcome=false;
 try{for(unsigned i=0;i<n;i++){cpu->step();if(cpu->bios_system_error)throw std::runtime_error(cpu->failure_summary());}status=L"Da thuc thi lenh.";}
  catch(const std::exception& e){win_diag::write_event("CPU step",e.what(),true);stop(w);std::string message=e.what();status=L"Dừng: "+std::wstring(message.begin(),message.end());last_fault=status;}
 refresh();
}
void loadBios(HWND w){
 wchar_t file[32768]{};OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.hwndOwner=w;o.lpstrFile=file;o.nMaxFile=32768;
 o.lpstrFilter=L"BIOS (*.bin)\0*.bin\0All files\0*.*\0";o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
 if(!GetOpenFileNameW(&o))return;
  stop(w,true);
 try {
  std::ifstream f(std::filesystem::path(file),std::ios::binary|std::ios::ate);
  if(!f||f.tellg()!=std::streamoff(512*1024))throw std::runtime_error("BIOS must be exactly 512 KiB");
  std::vector<uint8_t> image(512*1024);f.seekg(0);
  if(!f.read(reinterpret_cast<char*>(image.data()),image.size()))throw std::runtime_error("Read failed");
   show_welcome=false;ps1::boot_bios(*bus,*cpu,image);bios_name=std::filesystem::path(file).filename().wstring();preferences.bios_path=file;exe_loaded=false;has_bios=true;game_booted=false;entry=0xbfc00000;last_fault.clear();persist_preferences(w);
  status=bus->cd.sectors?L"Đã nạp BIOS, giữ nguyên đĩa. Chọn Boot đĩa → Chạy.":L"Đã nạp BIOS. Bấm Chạy để xem BIOS hoặc Mở đĩa → Boot đĩa → Chạy.";
  }catch(const std::exception& error){win_diag::write_event("Load BIOS",error.what(),true);status=L"Không nạp được BIOS. Cần tệp BIOS PS1 hợp lệ, đúng 512 KiB.";}
 refresh();
}
void openExe(HWND w){
 wchar_t file[32768]{};OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.hwndOwner=w;o.lpstrFile=file;o.nMaxFile=32768;
 o.lpstrFilter=L"PS-X EXE (*.exe;*.ps-exe)\0*.exe;*.ps-exe\0All files\0*.*\0";o.Flags=OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
 if(!GetOpenFileNameW(&o))return;
  stop(w,true);
 try{
  std::ifstream f(std::filesystem::path(file),std::ios::binary|std::ios::ate);
  auto size=f.tellg();if(!f||size<0x800||size>4*1024*1024)throw std::runtime_error("EXE file size invalid (max 4 MiB)");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));f.seekg(0);
  if(!f.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))throw std::runtime_error("EXE read failed");
  auto fresh=std::make_unique<ps1::Bus>();auto freshCpu=std::make_unique<ps1::Cpu>(*fresh);
  show_welcome=false;ps1::load_exe(*fresh,*freshCpu,bytes);
   cpu.reset();bus=std::move(fresh);cpu=std::move(freshCpu);entry=cpu->pc;initial_regs=cpu->r;exe_loaded=true;has_bios=false;game_booted=false;current_disc_path.clear();bios_name=L"Homebrew trực tiếp";disc_name=std::filesystem::path(file).filename().wstring();
  status=L"PS-X EXE loaded. Bam Run. Khong co BIOS services; chi homebrew tu khoi tao.";
  }catch(const std::exception& e){win_diag::write_event("Load PS-X EXE",e.what(),true);std::string text=e.what();status=L"EXE: "+std::wstring(text.begin(),text.end());}
 refresh();
}
bool same_disc_path(const std::filesystem::path& left,const std::filesystem::path& right){
 const auto a=left.wstring(),b=right.wstring();return CompareStringOrdinal(a.c_str(),static_cast<int>(a.size()),b.c_str(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;
}
bool mount_disc_path(HWND w,const std::filesystem::path& path){
 stop(w,true);
 try{
  bus->cd.mount(path);current_disc_path=std::filesystem::absolute(path).lexically_normal();game_booted=false;disc_name=current_disc_path.filename().wstring();
  status=L"Đã mở đĩa. Nạp BIOS nếu chưa có → Boot đĩa → Chạy.";
  if(game_library){std::wstring error;if(!game_library->import_game(current_disc_path,error)||!game_library->update_disc_metadata(current_disc_path,bus->cd.boot_name,bus->cd.effective_region(),error)){
    win_diag::write_event("Game library metadata","Disc mounted, but metadata could not be added to the library.",true);status=L"Đã mount đĩa; không lưu được metadata thư viện.";
   }else persist_library(w);}
  if(library_window)avopsui::refresh_game_library_window(library_window);
 }catch(const std::exception& error){win_diag::write_event("Mount disc",error.what(),true);const std::string message=error.what();status=L"Mount thất bại: "+std::wstring(message.begin(),message.end());refresh();return false;}
 refresh();if(settings_window)avopsui::refresh_settings_window(settings_window,preferences.font_size);return true;
}
void mountIso(HWND w){
 wchar_t path[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=w;dialog.lpstrFile=path;dialog.nMaxFile=static_cast<DWORD>(sizeof(path)/sizeof(path[0]));dialog.lpstrFilter=L"PS1 disc (*.cue;*.bin;*.iso)\0*.cue;*.bin;*.iso\0All files\0*.*\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
 if(!GetOpenFileNameW(&dialog))return;mount_disc_path(w,std::filesystem::path(path));
}
void boot_library_game(HWND w,const std::filesystem::path& path){
 if(!same_disc_path(current_disc_path,path)||!bus->cd.sectors)if(!mount_disc_path(w,path))return;
 stop(w,true);if(!has_bios){status=L"Nạp BIOS hợp lệ trước khi Boot / run từ thư viện.";refresh();MessageBoxW(w,status.c_str(),L"BIOS required",MB_OK|MB_ICONWARNING);return;}
 try{
  ps1::boot_disc(*bus,*cpu,false);game_booted=true;exe_loaded=false;entry=cpu->pc;last_fault.clear();
  if(game_library){const auto* game=game_library->find(path);if(game&&game->has_host_overrides){limit_speed=game->limit_speed;muted=game->muted;if(audio_output.ready())audio_output.set_muted(muted);if(buttons[18])SetWindowTextW(buttons[18],limit_speed?L"Tốc độ: 1x":L"Tốc độ: Max");if(buttons[19])SetWindowTextW(buttons[19],muted?L"Âm thanh: Tắt":L"Âm thanh: Bật");}}
  status=L"Đã reset BIOS với game đã mount; Runner bắt đầu phiên chơi.";start_run(w);
 }catch(const std::exception& error){win_diag::write_event("Library game boot",error.what(),true);const std::string message=error.what();status=L"Boot game thất bại: "+std::wstring(message.begin(),message.end());refresh();}
}
std::filesystem::path default_capture_path(){
 wchar_t local_app_data[32768]{};const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",local_app_data,static_cast<DWORD>(sizeof(local_app_data)/sizeof(local_app_data[0])));
 if(!length||length>=sizeof(local_app_data)/sizeof(local_app_data[0]))throw std::runtime_error("LOCALAPPDATA is unavailable");
 const auto directory=std::filesystem::path(local_app_data)/L"AVOPSEMU"/L"Captures";
 std::error_code error;std::filesystem::create_directories(directory,error);if(error)throw std::runtime_error("Cannot create capture directory: "+error.message());
 SYSTEMTIME time{};GetLocalTime(&time);static std::atomic<uint64_t> sequence{0};
 std::wostringstream name;name<<L"capture_"<<std::setfill(L'0')<<std::setw(4)<<time.wYear<<std::setw(2)<<time.wMonth<<std::setw(2)<<time.wDay<<L"_"<<std::setw(2)<<time.wHour<<std::setw(2)<<time.wMinute<<std::setw(2)<<time.wSecond<<L"_"<<std::setw(3)<<time.wMilliseconds<<L"_"<<sequence++<<L".txt";
 return directory/name.str();
}
void save_diagnostic_capture(HWND w,const std::filesystem::path& output_path){
 try{
  std::ostringstream text;for(wchar_t ch:last_fault)text.put(ch<=127?static_cast<char>(ch):'?');
  text<<"\nHOST_INPUT last_pressed="<<std::hex<<last_host_buttons<<" held="<<host_pad.held_buttons()<<" pending_presses="<<host_pad.pending_presses()<<" events="<<std::dec<<input_events<<" samples="<<input_samples<<" focused_before_export="<<input_focused
      <<"\nMIPS="<<core_mips<<" VideoHz="<<video_hz<<" paced="<<limit_speed<<" audio_underruns="<<audio_output.underruns()<<" audio_tempo="<<audio_output.ratio()<<" audio_host_drops="<<audio_output.dropped()<<" audio_buffered="<<audio_output.buffered_frames()<<" audio_mmcss="<<audio_output.multimedia_scheduled()<<" audio_error="<<audio_output.error()<<"\n"<<cpu->diagnostic();
  const auto content=text.str();const auto text_file=ps1::write_verified_file(output_path,content.data(),content.size());
  const auto images=ps1::save_debug_images_detailed(*bus,output_path);
  const bool complete=text_file.success&&images.success();
  std::ostringstream details;
  auto append_file=[&](const char* name,const ps1::CaptureFileResult& file){details<<name<<" path=\""<<file.path.u8string()<<"\" expected="<<file.expected_bytes<<" written="<<file.bytes_written<<" verified="<<file.bytes_verified<<" error_code="<<file.error_code<<" operation=\""<<file.error<<"\" success="<<file.success<<"; ";};
  append_file("text_log",text_file);for(size_t i=0;i<images.files.size();i++)append_file(i==0?"vram_raw":i==1?"vram_bmp":"frame_bmp",images.files[i]);
  const uint64_t written=text_file.bytes_written+images.total_bytes_written();
  const uint64_t verified=text_file.bytes_verified+images.total_bytes_verified();
  const auto detail_text=details.str();
  if(!win_diag::write_capture_record(output_path,images.framebuffer_width,images.framebuffer_height,images.pixel_format.c_str(),written,verified,complete,detail_text.c_str()))
   win_diag::write_event("Capture","Could not write the capture record to capture.log",true);
  if(!complete)win_diag::write_event("Capture",detail_text.c_str(),true);
  if(complete)status=L"Đã lưu log và ảnh đã ghi/đọc lại đầy đủ: "+output_path.filename().wstring();
  else status=L"Capture chưa đầy đủ; xem capture.log trong %LOCALAPPDATA%\\AVOPSEMU\\Logs.";
  refresh();
 }catch(const std::exception& error){
   if(!win_diag::write_capture_record(output_path,0,0,"unknown",0,0,false,error.what()))
    win_diag::write_event("Capture","Could not write the capture failure record",true);
  win_diag::write_event("Capture",error.what(),true);status=L"Capture thất bại; xem session.log trong %LOCALAPPDATA%\\AVOPSEMU\\Logs.";refresh();
 }catch(...){
   if(!win_diag::write_capture_record(output_path,0,0,"unknown",0,0,false,"Non-standard exception while creating diagnostic capture"))
    win_diag::write_event("Capture","Could not write the capture failure record",true);
  win_diag::write_event("Capture","Non-standard exception while creating diagnostic capture",true);status=L"Capture thất bại do lỗi không xác định; xem session.log.";refresh();
 }
}
void paint_shell(HWND w,HDC dc){
 RECT client{};GetClientRect(w,&client);FillRect(dc,&client,background_brush);
 ps1ui::label(dc,L"AVOPSEMU",RECT{20,12,380,62},title_font,ps1ui::text);
 ps1ui::label(dc,L"PLAYSTATION EMULATION  /  v51",RECT{22,62,400,82},small_font,ps1ui::accent);
  if(fps_hud_visible){std::wostringstream metrics;metrics<<std::fixed<<std::setprecision(1)<<display_frame_summary.fps<<L" FPS GDI   ·   "<<video_hz<<L" Hz mô hình   ·   "<<core_mips<<L" MIPS   ·   "<<(running?L"ĐANG CHẠY":L"TẠM DỪNG");
   ps1ui::label(dc,metrics.str().c_str(),RECT{420,26,client.right-20,62},ui_font,running?ps1ui::accent:ps1ui::muted,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);}
 if(!large_view){
  const auto& p=shell_layout.sidebar;const int y=p.y;
  ps1ui::box(dc,RECT{p.x,y,p.x+p.width,y+p.height},ps1ui::panel,ps1ui::border);
  ps1ui::label(dc,L"PHIÊN CHƠI PS1",RECT{36,y+12,228,y+40},small_font,ps1ui::accent);
  ps1ui::label(dc,L"BIOS  /  512 KiB",RECT{36,y+48,228,y+72},small_font,ps1ui::muted);
  ps1ui::label(dc,bios_name.c_str(),RECT{36,y+76,228,y+102},ui_font,ps1ui::text,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
  ps1ui::label(dc,L"ĐĨA / CHƯƠNG TRÌNH",RECT{36,y+112,228,y+138},small_font,ps1ui::muted);
  ps1ui::label(dc,disc_name.c_str(),RECT{36,y+142,228,y+174},ui_font,ps1ui::text,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
  ps1ui::label(dc,L"DIGITAL PAD 1",RECT{36,y+184,228,y+210},small_font,ps1ui::accent);
  ps1ui::label(dc,wasd_profile?L"WASD + J / K / U / I":L"Mũi tên + Z / X / A / S",RECT{36,y+214,228,y+244},small_font,ps1ui::text);
 }
  if(status_visible){const auto& r=shell_layout.status;
   ps1ui::box(dc,RECT{r.x,r.y,r.x+r.width,r.y+r.height},ps1ui::panel,ps1ui::border,8);
   ps1ui::label(dc,status.c_str(),RECT{r.x+10,r.y,r.x+r.width-10,r.y+r.height},small_font,ps1ui::muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);}
}

LRESULT CALLBACK proc_impl(HWND w,UINT m,WPARAM wp,LPARAM lp){
 switch(m){
 case WM_CREATE:{
  const wchar_t* labels[]={L"Demo CPU",L"1. Nạp BIOS",L"Đặt lại CPU (giữ RAM)",L"Step",L"4. Chạy",L"Tạm dừng",L"Demo GPU",L"Polygon",L"Open EXE",L"Chạy 10.000 lệnh",L"Lưu log + ảnh",L"2. Mở đĩa",L"Homebrew EXE",L"Chế độ rạp",L"Demo texture",L"3. Boot đĩa",L"Region CD",L"Tháo đĩa",L"Tốc độ: 1x",L"Âm thanh: Bật",L"SPU test",L"Bàn phím"};
  for(int i=0;i<22;i++){auto b=CreateWindowW(L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,16+(i%8)*120,16+(i/8)*42,110,36,w,reinterpret_cast<HMENU>(INT_PTR(101+i)),nullptr,nullptr);buttons[i]=b;SendMessageW(b,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);}
   settings_button=CreateWindowW(L"BUTTON",L"Cài đặt",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,16,16,110,36,w,reinterpret_cast<HMENU>(134),nullptr,nullptr);
   SendMessageW(settings_button,WM_SETFONT,reinterpret_cast<WPARAM>(ui_font),TRUE);
    library_button=CreateWindowW(L"BUTTON",L"Thư viện game",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,16,16,110,36,w,reinterpret_cast<HMENU>(135),nullptr,nullptr);
    SendMessageW(library_button,WM_SETFONT,reinterpret_cast<WPARAM>(ui_font),TRUE);
    if(!settings_button||!library_button)return -1;
  pad_hint=CreateWindowW(L"STATIC",L"Mũi tên: di chuyển   Z/X/A/S: Cross/Circle/Square/Triangle\nEnter: Start   Shift: Select   Q/W: L1/R1   E/R: L2/R2",WS_CHILD|WS_VISIBLE,16,90,1300,24,w,nullptr,nullptr,nullptr);
  output=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY,16,108,740,400,w,nullptr,nullptr,nullptr);
  screen=CreateWindowW(L"PS1Vram",L"",WS_CHILD|WS_VISIBLE,780,108,640,480,w,nullptr,GetModuleHandleW(nullptr),nullptr);
  font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
  for(auto button:buttons)SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(ui_font),TRUE);
  if(wasd_profile)SetWindowTextW(pad_hint,L"WASD: di chuyển   J/K/U/I: Cross/Circle/Square/Triangle\nEnter: Start   Tab: Select   Q/E: L1/R1   1/3: L2/R2");
  SendMessageW(pad_hint,WM_SETFONT,reinterpret_cast<WPARAM>(small_font),TRUE);
  HMENU menu=CreateMenu(),file=CreatePopupMenu(),view=CreatePopupMenu(),tools=CreatePopupMenu();
    AppendMenuW(file,MF_STRING,135,L"Game Library...");AppendMenuW(file,MF_STRING,102,L"Nạp BIOS PS1...");AppendMenuW(file,MF_STRING,112,L"Mở đĩa BIN / CUE / ISO...");AppendMenuW(file,MF_STRING,116,L"Boot đĩa qua BIOS");AppendMenuW(file,MF_STRING,109,L"Mở homebrew EXE...");AppendMenuW(file,MF_STRING,111,L"Lưu log + ảnh VRAM...");AppendMenuW(file,MF_STRING,133,L"Chụp tự động vào AppData (không hộp thoại)");AppendMenuW(file,MF_STRING,124,L"Thoát");
   AppendMenuW(view,MF_STRING,123,L"Bật / tắt debugger   F9");AppendMenuW(view,MF_STRING,114,L"Bật / tắt chế độ rạp");AppendMenuW(view,MF_STRING,fullscreen_command,L"Toàn màn hình   F11");AppendMenuW(view,MF_STRING,122,L"Hướng dẫn bàn phím");
  AppendMenuW(view,MF_STRING,129,L"Thử giữ Cross trong 1 giây");
  AppendMenuW(view,MF_STRING,130,L"Thử giữ Start trong 1 giây");
  AppendMenuW(view,MF_STRING|(!wasd_profile?MF_CHECKED:0),126,L"Bàn phím: Mũi tên + Z/X/A/S");
  AppendMenuW(view,MF_STRING|(wasd_profile?MF_CHECKED:0),127,L"Bàn phím: WASD + J/K/U/I");
  AppendMenuW(view,MF_STRING|(keyboard.neutral_opposites?MF_CHECKED:0),128,L"Trung hòa hai hướng đối nghịch");
  const unsigned ids[]={103,104,110,117,118,101,107,108,115,121,113};
  const wchar_t* names[]={L"Đặt lại CPU (giữ RAM)",L"Chạy 1 lệnh",L"Chạy 10.000 lệnh",L"Đổi vùng CD",L"Tháo đĩa",L"Demo CPU",L"Demo GPU",L"Demo đa giác",L"Demo texture",L"Kiểm tra âm thanh",L"Boot homebrew trực tiếp"};
  for(unsigned n=0;n<11;n++)AppendMenuW(tools,MF_STRING,ids[n],names[n]);
  
   AppendMenuW(tools,MF_STRING,134,L"Settings Center...");
    AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(file),L"Tệp");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(view),L"Hiển thị");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(tools),L"Công cụ");AppendMenuW(view,MF_STRING,131,L"Màn hình chào");AppendMenuW(view,MF_STRING,132,L"Về AVOPSEMU");SetMenu(w,menu);main_menu=menu;
    CheckMenuItem(menu,123,MF_BYCOMMAND|(debug_visible?MF_CHECKED:MF_UNCHECKED));CheckMenuItem(menu,114,MF_BYCOMMAND|(large_view?MF_CHECKED:MF_UNCHECKED));CheckMenuItem(menu,fullscreen_command,MF_BYCOMMAND|(fullscreen?MF_CHECKED:MF_UNCHECKED));
   SetWindowTextW(buttons[18],limit_speed?L"Tốc độ: 1x":L"Tốc độ: Max");SetWindowTextW(buttons[19],muted?L"Âm thanh: Tắt":L"Âm thanh: Bật");SetWindowTextW(buttons[13],large_view?L"Hiện thanh bên":L"Chế độ rạp");
    SendMessageW(output,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);SetTimer(w,2,8,nullptr);if(!SetTimer(w,3,5000,nullptr)){const auto message="Periodic log flush timer GetLastError="+std::to_string(GetLastError());win_diag::write_event("Diagnostics",message.c_str(),true);}if(!SetTimer(w,4,30000,nullptr))win_diag::write_event("Game library","Could not create the periodic library checkpoint timer",true);layout(w);refresh();return 0;}
 case WM_ERASEBKGND:return 1;
 case WM_PAINT:{PAINTSTRUCT paint{};auto dc=BeginPaint(w,&paint);RECT r{};GetClientRect(w,&r);
  auto memory=CreateCompatibleDC(dc);auto bitmap=CreateCompatibleBitmap(dc,std::max(1,int(r.right)),std::max(1,int(r.bottom)));
  if(memory&&bitmap){auto old=SelectObject(memory,bitmap);paint_shell(w,memory);BitBlt(dc,0,0,r.right,r.bottom,memory,0,0,SRCCOPY);SelectObject(memory,old);}
  else paint_shell(w,dc);
  if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);EndPaint(w,&paint);return 0;}
 case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:{auto dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,ps1ui::muted);SetBkColor(dc,ps1ui::background);return reinterpret_cast<LRESULT>(background_brush);}
 case WM_DRAWITEM:{auto item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);if(item->CtlType!=ODT_BUTTON)break;
    const bool primary=item->CtlID==105||item->CtlID==116||item->CtlID==134||item->CtlID==135,pressed=item->itemState&ODS_SELECTED;
  RECT r=item->rcItem;ps1ui::box(item->hDC,r,primary?(pressed?RGB(88,80,185):ps1ui::accent):(pressed?RGB(47,45,66):ps1ui::panel),primary?ps1ui::accent:ps1ui::border,10);
  wchar_t text[128]{};GetWindowTextW(item->hwndItem,text,128);ps1ui::label(item->hDC,text,r,ui_font,primary?ps1ui::background:ps1ui::text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
  if(item->itemState&ODS_FOCUS){InflateRect(&r,-4,-4);DrawFocusRect(item->hDC,&r);}return TRUE;}
 case WM_ACTIVATEAPP:if(!wp){keyboard.release();host_pad.clear();mouse_pad_buttons=0xffff;}return 0;
  case WM_SIZE:if(output&&screen)layout(w);return 0;
  case WM_DISPLAYCHANGE:if(fullscreen){std::wstring error;if(!position_fullscreen(w,error))show_fullscreen_error(w,error);}if(output&&screen)layout(w);return 0;
 case WM_GETMINMAXINFO:{auto p=reinterpret_cast<MINMAXINFO*>(lp);p->ptMinTrackSize={1040,780};return 0;}
 case WM_COMMAND:
  switch(LOWORD(wp)){
     case 134:{avopsui::SettingsActions actions;actions.changed=[w]{apply_preferences(w);};actions.choose_bios=[w]{loadBios(w);};actions.snapshot=[&]{avopsui::SettingsSnapshot snapshot;snapshot.bios_path=preferences.bios_path;snapshot.bios_name=bios_name;snapshot.disc_name=disc_name;snapshot.bios_loaded=has_bios;snapshot.disc_mounted=bus&&bus->cd.sectors!=0;snapshot.region=bus?bus->cd.effective_region():4;return snapshot;};settings_window=avopsui::open_settings_window(w,preferences,preferences_path,actions,preferences.font_size);if(!settings_window)MessageBoxW(w,L"Không thể tạo cửa sổ Settings Center.",L"AVOPSEMU",MB_OK|MB_ICONERROR);break;}
    case 135:{if(!game_library){MessageBoxW(w,L"Game Library chưa được khởi tạo.",L"AVOPSEMU",MB_OK|MB_ICONERROR);break;}avopsui::LibraryActions actions;actions.mount_disc=[w](const std::filesystem::path& path){mount_disc_path(w,path);};actions.boot_disc=[w](const std::filesystem::path& path){boot_library_game(w,path);};library_window=avopsui::open_game_library_window(w,*game_library,std::move(actions));if(!library_window)MessageBoxW(w,L"Không thể tạo cửa sổ Game Library.",L"AVOPSEMU",MB_OK|MB_ICONERROR);break;}
   case 129:case 130:
    if(!running)start_run(w);
    if(running){SetFocus(screen);mouse_pad_buttons=uint16_t(0xffffu^(LOWORD(wp)==129?0x4000u:8u));mouse_pad_until=GetTickCount64()+1000;update_pad(w);}
    break;
   case 126:case 127:wasd_profile=LOWORD(wp)==127;keyboard.preset(wasd_profile);host_pad.clear();update_pad(w);save_keyboard(w,true);CheckMenuItem(GetMenu(w),126,MF_BYCOMMAND|(!wasd_profile?MF_CHECKED:MF_UNCHECKED));CheckMenuItem(GetMenu(w),127,MF_BYCOMMAND|(wasd_profile?MF_CHECKED:MF_UNCHECKED));InvalidateRect(w,nullptr,FALSE);
    SetWindowTextW(pad_hint,wasd_profile?L"KEYBOARD   WASD D-pad | J Cross / K Circle / U Square / I Triangle | Enter Start / Tab Select | Q/E L1/R1 | 1/3 L2/R2":L"KEYBOARD   Arrows D-pad | Z Cross / X Circle / A Square / S Triangle | Enter Start / Shift Select | Q/W L1/R1 | E/R L2/R2");SetFocus(screen);break;
   case 128:host_pad.clear();keyboard.release();keyboard.neutral_opposites=!keyboard.neutral_opposites;update_pad(w);save_keyboard(w);CheckMenuItem(GetMenu(w),128,MF_BYCOMMAND|(keyboard.neutral_opposites?MF_CHECKED:MF_UNCHECKED));break;
    case 123:debug_visible=!debug_visible;preferences.show_debugger=debug_visible;CheckMenuItem(GetMenu(w),123,MF_BYCOMMAND|(debug_visible?MF_CHECKED:MF_UNCHECKED));layout(w);persist_preferences(w);refresh();if(settings_window)avopsui::refresh_settings_window(settings_window,preferences.font_size);break;
   case 124:DestroyWindow(w);break;
   case 131:stop(w);show_welcome=true;status=L"Màn hình chào • Phiên chơi được giữ lại. Bấm Chạy để tiếp tục.";refresh();break;
  case 132:MessageBoxW(w,L"AVOPSEMU PS1 v51\nGiả lập PlayStation — C++17 / Win32\n\nPS1: CPU, GPU, CD-ROM, SPU và digital pad.\nPS2 đã được tách thành dự án AVOPSEMU_PS2 riêng.\n\nF5: Chạy / Tạm dừng\nF9: Debugger\nF11: Toàn màn hình\nEscape: Thoát toàn màn hình\n\nKhả năng tương thích game vẫn đang được phát triển.",L"Về AVOPSEMU",MB_OK|MB_ICONINFORMATION);break;
   case 122:{keyboard.release();update_pad(w);MessageBoxW(w,L"Digital Pad 1 - ban phim\n\nHiển thị > Bàn phím để đổi preset. WASD preset: WASD di chuyen; J/K/U/I = Cross/Circle/Square/Triangle; Q/E = L1/R1; 1/3 = L2/R2; Tab = Select.\n\nPreset mac dinh:\nMui ten: D-pad\nZ: Cross (X) - chức năng tùy game\nX: Circle (O)\nA: Square\nS: Triangle\nEnter: Start / Pause trong game\nShift: Select\nQ / W: L1 / R1\nE / R: L2 / R2\n\nBam vao man hinh game truoc khi choi. Giu nhieu phim cung luc duoc.\nAlt-Tab se nha tat ca nut. Day la digital pad, chua co analog/rung.",L"AVOPSEMU — Bàn phím PS1",MB_OK);SetFocus(screen);break;}

   case 115:{stop(w,true);game_booted=false;current_disc_path.clear();exe_loaded=false;entry=0;const auto n=ps1::install_texture_demo(*bus);cpu->reset(0);advance(w,n);status=L"Texture demo: CPU upload -> textured quads, modulation, sprite va blend.";refresh();break;}
     case 114:large_view=!large_view;preferences.show_sidebar=!large_view;SetWindowTextW(buttons[13],large_view?L"Hiện thanh bên":L"Chế độ rạp");layout(w);persist_preferences(w);break;
     case 136:toggle_fullscreen(w);break;
   case 101:stop(w,true);game_booted=false;current_disc_path.clear();exe_loaded=false;entry=0;ps1::install_demo(*bus);cpu->reset(entry);advance(w,4);status=L"Demo hoan tat: 7 + 5 = 12. Xem RAM[0x100].";refresh();break;
   case 112:mountIso(w);break;
    case 113:{stop(w,true);try{ps1::boot_disc(*bus,*cpu,true);game_booted=true;entry=cpu->pc;initial_regs=cpu->r;exe_loaded=true;last_fault.clear();start_perf();status=L"Homebrew EXE loaded. KHONG co kernel BIOS. Game thuong mai: Nap BIOS -> Boot Game CD -> Run.";}catch(const std::exception&e){win_diag::write_event("Direct disc boot",e.what(),true);std::string msg=e.what();status=std::wstring(msg.begin(),msg.end());}refresh();break;}
    case 116:{stop(w,true);try{if(!has_bios)throw std::runtime_error("Nap BIOS truoc, sau do Mount CD");ps1::boot_disc(*bus,*cpu,false);game_booted=true;exe_loaded=false;entry=cpu->pc;last_fault.clear();start_perf();status=L"Đã reset BIOS với đĩa đã mở. Bấm Chạy để khởi động game.";}catch(const std::exception&e){win_diag::write_event("BIOS disc boot",e.what(),true);std::string msg=e.what();status=std::wstring(msg.begin(),msg.end());}refresh();break;}
   case 117:{stop(w);bus->cd.region=(bus->cd.region+1)%5;const wchar_t* names[]={L"Auto (BOOT prefix)",L"USA",L"Europe",L"Japan",L"Unlicensed"};status=L"GetID region emulated: "+std::wstring(names[bus->cd.region]);refresh();break;}
   case 121:stop(w,true);game_booted=false;current_disc_path.clear();exe_loaded=false;entry=0;ps1::install_spu_demo(*bus);cpu->reset(0);start_run(w);status=L"Original SPU test: hai tan so trai/phai; khong can BIOS.";refresh();break;
    case 119:{bool resume=running;stop(w);limit_speed=!limit_speed;preferences.limit_speed=limit_speed;SetWindowTextW(buttons[18],limit_speed?L"Tốc độ: 1x":L"Tốc độ: Max");persist_preferences(w);if(resume)start_run(w);else refresh();break;}
    case 120:muted=!muted;preferences.muted=muted;audio_output.set_muted(muted);SetWindowTextW(buttons[19],muted?L"Âm thanh: Tắt":L"Âm thanh: Bật");persist_preferences(w);refresh();break;
   case 118:stop(w,true);bus->cd.eject();current_disc_path.clear();game_booted=false;disc_name=L"Chưa mở đĩa";status=L"Đã tháo đĩa.";refresh();if(library_window)avopsui::refresh_game_library_window(library_window);break;
    case 111:{
     stop(w);wchar_t path[32768]=L"avopsemu_ps1_log.txt";OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.hwndOwner=w;o.lpstrFile=path;o.nMaxFile=32768;o.lpstrFilter=L"Text log\0*.txt\0";o.lpstrDefExt=L"txt";o.Flags=OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR;
     if(GetSaveFileNameW(&o))save_diagnostic_capture(w,std::filesystem::path(path));
     else{const DWORD dialog_error=CommDlgExtendedError();if(dialog_error){const auto message="GetSaveFileNameW failed, CommDlgExtendedError="+std::to_string(dialog_error);win_diag::write_event("Save dialog",message.c_str(),true);status=L"Save dialog gặp lỗi; xem session.log.";refresh();}}
     break;}
     case 133:{
      stop(w);
      try{save_diagnostic_capture(w,default_capture_path());}
      catch(const std::exception& error){
       if(!win_diag::write_capture_record(std::filesystem::path{},0,0,"unknown",0,0,false,error.what()))
        win_diag::write_event("Capture","Could not write the capture failure record",true);
       win_diag::write_event("Automatic capture path",error.what(),true);status=L"Không tạo được thư mục capture trong AppData.";refresh();
      }
      catch(...){
       if(!win_diag::write_capture_record(std::filesystem::path{},0,0,"unknown",0,0,false,"Non-standard exception creating output path"))
        win_diag::write_event("Capture","Could not write the capture failure record",true);
       win_diag::write_event("Automatic capture path","Non-standard exception creating output path",true);status=L"Không tạo được đường dẫn capture.";refresh();
      }
      break;
     }
   case 109:openExe(w);break;
   case 110:stop(w);advance(w,10000);break;
    case 108:{stop(w,true);game_booted=false;current_disc_path.clear();exe_loaded=false;entry=0;unsigned n=ps1::install_polygon_demo(*bus);cpu->reset(0);advance(w,n);status=L"Polygon demo: tam giac va tu giac mau phang qua CPU -> GP0.";refresh();break;}
    case 107:{stop(w,true);game_booted=false;current_disc_path.clear();exe_loaded=false;entry=0;unsigned n=ps1::install_gpu_demo(*bus);cpu->reset(0);advance(w,n);status=L"GPU demo: CPU -> SW -> GP0 -> VRAM. Khung xem 320x240.";refresh();break;}
   case 102:loadBios(w);break;
   case 103:stop(w);cpu->reset(entry);if(exe_loaded)cpu->r=initial_regs;status=L"Đã đặt lại CPU; RAM được giữ lại.";refresh();break;
   case 104:stop(w);advance(w,1);break;
   case 105:if(!running)start_run(w);break;
   case 106:stop(w);status=L"Đã tạm dừng. Bấm Chạy hoặc F5 để tiếp tục.";refresh();break;
  }return 0;
  case WM_TIMER:if(wp==2){update_pad(w,true);return 0;}if(wp==3){win_diag::flush();return 0;}if(wp==4){persist_library(w);if(library_window)avopsui::refresh_game_library_window_live(library_window);return 0;}if(wp==1&&running){
  sample_perf();
   if(library_window&&GetTickCount64()-last_library_refresh>=1000){avopsui::refresh_game_library_window_live(library_window);last_library_refresh=GetTickCount64();}
  if(GetTickCount64()-last_refresh>=250){std::wstring hint=wasd_profile?L"WASD: di chuyển   J/K/U/I: Cross/Circle/Square/Triangle":L"Mũi tên: di chuyển   Z/X/A/S: Cross/Circle/Square/Triangle";
   hint+=L"\nEnter: Start   ·   F5: Chạy / Tạm dừng   ·   ";hint+=input_focused?L"Bàn phím đang hoạt động":L"Bấm vào màn hình game để nhận phím";
   SetWindowTextW(pad_hint,hint.c_str());}

   if(!runner->active()){
    const auto error=runner->error();stop(w,true);status=L"Dừng: "+std::wstring(error.begin(),error.end());last_fault=status;refresh();return 0;
  }
  if(audio_output.error()!=MMSYSERR_NOERROR&&audio_output.ready()==false){audio_output.close();status=L"Loi phat am thanh Windows; CPU van chay.";}
  bool changed=false;
  const auto wait_start=PerfClock::now();
  {std::lock_guard<std::mutex> lock(machine_mutex);changed=(bus->gpu.words!=painted_words&&bus->gpu.video_frames!=painted_video)||bus->gpu.display_revision!=painted_display;}
  ui_mutex_wait_ns+=duration_ns(PerfClock::now()-wait_start);
  if(changed&&!IsIconic(w))InvalidateRect(screen,nullptr,FALSE);
  if(GetTickCount64()-last_refresh>=250){refresh();last_refresh=GetTickCount64();}
 }return 0;
  case WM_DESTROY:{
    KillTimer(w,2);KillTimer(w,3);KillTimer(w,4);if(library_window&&IsWindow(library_window))DestroyWindow(library_window);if(settings_window&&IsWindow(settings_window))DestroyWindow(settings_window);stop(w,true);persist_library(w);
    if(main_menu){if(GetMenu(w)==main_menu)SetMenu(w,nullptr);DestroyMenu(main_menu);main_menu=nullptr;}
   if(!fullscreen){WINDOWPLACEMENT placement{};placement.length=sizeof(placement);if(GetWindowPlacement(w,&placement))save_windowed_size(placement);else win_diag::write_event("Window placement","Could not save normal window placement",true);}
   persist_preferences(w);win_diag::flush();PostQuitMessage(0);return 0;}
 }return DefWindowProcW(w,m,wp,lp);
 }
}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM wp,LPARAM lp) noexcept{
 try{return proc_impl(w,m,wp,lp);}
 catch(const std::exception& error){win_diag::write_event("Main window procedure",error.what(),true);}
 catch(...){win_diag::write_event("Main window procedure","Non-standard exception in Win32 callback",true);}
 if(!w||!PostMessageW(w,WM_CLOSE,0,0))PostQuitMessage(1);
 return 0;
}
LRESULT CALLBACK vramProc_impl(HWND w,UINT m,WPARAM wp,LPARAM lp){
 if(m==WM_LBUTTONDOWN){SetFocus(w);return 0;}
 if(m==WM_KEYDOWN&&wp==VK_ESCAPE){SendMessageW(GetParent(w),WM_COMMAND,fullscreen?fullscreen_command:106,0);return 0;}
 if(m==WM_PAINT){
   const auto paint_start=PerfClock::now();
  PAINTSTRUCT paint{};HDC dc=BeginPaint(w,&paint);
  if(show_welcome){
   RECT area{};GetClientRect(w,&area);FillRect(dc,&area,background_brush);
   const int cx=area.right/2,cy=area.bottom/2;
   ps1ui::box(dc,RECT{cx-88,cy-138,cx+88,cy-42},ps1ui::panel,ps1ui::accent,28);
   ps1ui::label(dc,L"AV",RECT{cx-88,cy-138,cx+88,cy-42},title_font,ps1ui::accent,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
   ps1ui::label(dc,L"AVOPSEMU",RECT{20,cy-24,area.right-20,cy+36},title_font,ps1ui::text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
   ps1ui::label(dc,L"Trở lại thế giới PlayStation.",RECT{20,cy+40,area.right-20,cy+72},ui_font,ps1ui::muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
   ps1ui::label(dc,L"Nạp BIOS → Mở đĩa → Boot đĩa → Chạy",RECT{12,area.bottom-58,area.right-12,area.bottom-20},small_font,ps1ui::accent,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
   EndPaint(w,&paint);return 0;
  }
   const auto wait_start=PerfClock::now();
   std::unique_lock<std::mutex> lock(machine_mutex);
   ui_mutex_wait_ns+=duration_ns(PerfClock::now()-wait_start);
  const auto view=ps1::display_view(bus->gpu);
   const uint32_t display_mode=bus->gpu.display[3];
   ps1::snapshot_display_vram(bus->gpu,presentation,view);
  painted_words=bus->gpu.words;painted_display=bus->gpu.display_revision;painted_video=bus->gpu.video_frames;lock.unlock();
  presentation_pixels.resize(view.width*view.height);
  auto& pixels=presentation_pixels;
  for(unsigned y=0;y<view.height;y++)for(unsigned x=0;x<view.width;x++)pixels[y*view.width+x]=ps1::display_pixel(presentation,view,x,y);
  BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(view.width);info.bmiHeader.biHeight=-static_cast<LONG>(view.height);
  info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
  RECT r{};GetClientRect(w,&r);SetStretchBltMode(dc,COLORONCOLOR);
  FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
   const auto dest=ps1::fit_display(r.right,r.bottom,preferences.stretch_screen);
   bool submitted=false;
   if(dest.width>0&&dest.height>0)submitted=StretchDIBits(dc,dest.x,dest.y,dest.width,dest.height,0,0,static_cast<int>(view.width),static_cast<int>(view.height),pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY)!=GDI_ERROR;
   EndPaint(w,&paint);
   const auto paint_end=PerfClock::now();
   if(submitted&&running&&IsWindowVisible(w)&&!IsIconic(GetParent(w))){
    const auto timestamp=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(paint_end.time_since_epoch()).count());
    display_frame_metrics.record_submission(timestamp,(display_mode&8)?20000000u:16666667u);
    paint_work_ns+=duration_ns(paint_end-paint_start);
   }
   return 0;
 }
 return DefWindowProcW(w,m,wp,lp);
}
LRESULT CALLBACK vramProc(HWND w,UINT m,WPARAM wp,LPARAM lp) noexcept{
 try{return vramProc_impl(w,m,wp,lp);}
 catch(const std::exception& error){win_diag::write_event("VRAM window procedure",error.what(),true);}
 catch(...){win_diag::write_event("VRAM window procedure","Non-standard exception in Win32 callback",true);}
 HWND parent=GetParent(w);if(!parent||!PostMessageW(parent,WM_CLOSE,0,0))PostQuitMessage(1);
 return 0;
}
int run_gui(HINSTANCE h,int show){
 wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
 const auto executable_directory=std::filesystem::path(executable).parent_path();
 preferences_path=avopsui::user_preferences_path();
 auto legacy_config=executable_directory/L"avopsemu.ini";
 if(GetFileAttributesW(legacy_config.c_str())==INVALID_FILE_ATTRIBUTES)legacy_config=executable_directory/L"pslab.ini";
 const auto loaded_preferences=avopsui::read_preferences(preferences_path,legacy_config);
 if(!loaded_preferences.error.empty()){
  win_diag::write_event("Settings","Invalid settings profile; defaults are active",true);
  MessageBoxW(nullptr,loaded_preferences.error.c_str(),L"AVOPSEMU settings",MB_OK|MB_ICONWARNING);
  preferences=avopsui::Preferences{};
 }else{
  preferences=loaded_preferences.value;
  if(!loaded_preferences.found||loaded_preferences.migrated){std::wstring error;if(!avopsui::write_preferences(preferences_path,preferences,error)){win_diag::write_event("Settings","Could not initialize or migrate the settings profile",true);MessageBoxW(nullptr,error.c_str(),L"AVOPSEMU settings",MB_OK|MB_ICONWARNING);}}
 }
  game_library_path=preferences_path.parent_path()/L"library.tsv";game_library=std::make_unique<avopsui::GameLibrary>(game_library_path);std::wstring library_error;
  if(!game_library->load(library_error)){
   win_diag::write_event("Game library","Existing library could not be loaded; preserving it and using a recovery database",true);MessageBoxW(nullptr,(library_error+L"\n\nThe original file is preserved. A new library-recovery.tsv will be used.").c_str(),L"Game Library",MB_OK|MB_ICONWARNING);
   game_library_path=preferences_path.parent_path()/L"library-recovery.tsv";game_library=std::make_unique<avopsui::GameLibrary>(game_library_path);if(!game_library->load(library_error))throw std::runtime_error("Could not initialize recovery game library.");
  }
 limit_speed=preferences.limit_speed;muted=preferences.muted;wasd_profile=preferences.wasd_profile;
 keyboard.keys=preferences.keyboard_keys;keyboard.neutral_opposites=preferences.neutral_opposites;
 large_view=!preferences.show_sidebar;debug_visible=preferences.show_debugger;toolbar_visible=preferences.show_toolbar;status_visible=preferences.show_status;fps_hud_visible=preferences.show_fps;
  avopsui::apply_palette(preferences.theme,preferences.accent);avopsui::set_game_library_font_size(preferences.font_size);rebuild_ui_resources();
 bus=std::make_unique<ps1::Bus>();cpu=std::make_unique<ps1::Cpu>(*bus);ps1::install_demo(*bus);cpu->reset(0);
 WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=h;c.lpszClassName=L"AVOPSEMUWindow";c.hIcon=LoadIconW(h,MAKEINTRESOURCEW(1));c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=background_brush;
 WNDCLASSW v{};v.lpfnWndProc=vramProc;v.hInstance=h;v.lpszClassName=L"PS1Vram";v.hCursor=LoadCursor(nullptr,IDC_ARROW);
 if(!RegisterClassW(&v))return 1;
 if(!RegisterClassW(&c))return 1;
  HWND w=CreateWindowW(c.lpszClassName,L"AVOPSEMU PS1 v51 — PlayStation / PS1",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,static_cast<int>(preferences.window_width),static_cast<int>(preferences.window_height),nullptr,nullptr,h,nullptr);
   if(!w)return 1;ShowWindow(w,preferences.window_maximized?SW_SHOWMAXIMIZED:show);
   if(preferences.fullscreen)toggle_fullscreen(w);
   MSG msg{};int result;
 while((result=GetMessageW(&msg,nullptr,0,0))>0){
   const HWND key_root=GetAncestor(msg.hwnd,GA_ROOT);
   if(msg.message==WM_KEYDOWN&&(key_root==w||key_root==settings_window)&&!(msg.lParam&(LPARAM(1)<<30))&&(msg.wParam==VK_F11||(msg.wParam==VK_ESCAPE&&fullscreen))){SendMessageW(w,WM_COMMAND,fullscreen_command,0);continue;}
  if((msg.message==WM_KEYDOWN||msg.message==WM_KEYUP)&&GetAncestor(msg.hwnd,GA_ROOT)==w){
   if(keyboard.event(unsigned(msg.wParam),msg.message==WM_KEYDOWN)){input_events++;update_pad(w);continue;}
  }
  if(msg.message==WM_KEYDOWN&&GetAncestor(msg.hwnd,GA_ROOT)==w&&!(msg.lParam&(LPARAM(1)<<30))){
    const unsigned command=msg.wParam==VK_F5?(running?106:105):msg.wParam==VK_F9?123:0;
   if(command){SendMessageW(w,WM_COMMAND,command,0);continue;}
  }
  TranslateMessage(&msg);DispatchMessageW(&msg);
 }
  DeleteObject(ui_font);DeleteObject(title_font);DeleteObject(small_font);DeleteObject(background_brush);DeleteObject(panel_brush);ps1::win_diag::flush();
 return result==-1?1:static_cast<int>(msg.wParam);
}
int WINAPI wWinMain(HINSTANCE h,HINSTANCE,PWSTR,int show){
 ps1::win_diag::install_handlers();
 const bool diagnostics_ready=ps1::win_diag::initialize_default();
 if(!diagnostics_ready)MessageBoxW(nullptr,L"Không mở được log crash trong %LOCALAPPDATA%\\AVOPSEMU\\Logs. WER vẫn có thể tạo dump nếu được bật.",L"AVOPSEMU diagnostics",MB_OK|MB_ICONWARNING);
 try{return run_gui(h,show);}
 catch(const std::exception& error){ps1::win_diag::write_event("wWinMain",error.what(),true);MessageBoxW(nullptr,L"AVOPSEMU gặp lỗi khi khởi động. Xem session.log hoặc WER dump.",L"AVOPSEMU",MB_OK|MB_ICONERROR);return 1;}
 catch(...){ps1::win_diag::write_event("wWinMain","Non-standard exception at GUI entry point",true);MessageBoxW(nullptr,L"AVOPSEMU gặp lỗi không xác định khi khởi động. Xem crash.log hoặc WER dump.",L"AVOPSEMU",MB_OK|MB_ICONERROR);return 1;}
}
