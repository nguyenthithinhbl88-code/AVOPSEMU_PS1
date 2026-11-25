#include "frontend_settings.hpp"
#include "ui_theme.hpp"
#include <commdlg.h>
#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace avopsui {
namespace {
constexpr unsigned current_schema=2;
constexpr wchar_t missing_value[]={1,0};
constexpr const wchar_t* categories[]={L"CPU",L"GPU",L"CD-ROM",L"Audio",L"BIOS",L"Timing",L"Controller",L"Display / Hiển thị",L"Giao diện"};
constexpr unsigned category_count=sizeof(categories)/sizeof(categories[0]);
constexpr int category_id=1000,theme_id=1100,accent_id=1101,font_id=1102;
constexpr int toolbar_id=1110,status_id=1111,fps_id=1112,sidebar_id=1113,debugger_id=1114;
constexpr int reset_ui_id=1120,export_id=1121,import_id=1122;
constexpr int mute_id=1200,pace_id=1201,bios_id=1202;
constexpr int default_keys_id=1300,wasd_keys_id=1301,neutral_id=1302,key_base_id=1320;
constexpr int stretch_screen_id=1400,fullscreen_id=1401;
constexpr int cpu_profiler_id=1500,cpu_profile_interval_id=1501,cpu_trace_id=1502,cpu_trace_size_id=1503,cpu_trace_export_id=1504;
constexpr int cpu_break_reserved_id=1510,cpu_break_exception_id=1511,cpu_pc_breakpoint_id=1512,cpu_pc_address_id=1513,cpu_pc_set_id=1514,cpu_pc_clear_id=1515;
constexpr int cpu_diagnostics_id=1520,cpu_diagnostics_export_id=1521,cpu_defaults_id=1522;
constexpr UINT_PTR cpu_stats_timer_id=1;
constexpr unsigned cpu_profile_intervals[]={64,256,1024,4096,16384,65536};
constexpr unsigned cpu_trace_sizes[]={128,512,2048,8192,16384};
constexpr int width=960,height=700;

template<std::size_t N> bool includes(const unsigned (&options)[N],unsigned value){
	for(unsigned option:options)if(option==value)return true;
	return false;
}

template<std::size_t N> unsigned selection_for(const unsigned (&options)[N],unsigned value){
	for(unsigned i=0;i<N;i++)if(options[i]==value)return i;
	return 0;
}

bool parse_pc_address(const std::wstring& text,uint32_t& address){
	size_t first=0;if(text.size()>=2&&text[0]==L'0'&&(text[1]==L'x'||text[1]==L'X'))first=2;
	if(first==text.size())return false;
	uint64_t value=0;
	for(size_t i=first;i<text.size();i++){
		const wchar_t ch=text[i];unsigned digit=0;
		if(ch>=L'0'&&ch<=L'9')digit=unsigned(ch-L'0');
		else if(ch>=L'a'&&ch<=L'f')digit=unsigned(ch-L'a'+10);
		else if(ch>=L'A'&&ch<=L'F')digit=unsigned(ch-L'A'+10);
		else return false;
		if(value>0x0fffffffu)return false;
		value=(value<<4)|digit;
	}
	if(value&3)return false;
	address=static_cast<uint32_t>(value);return true;
}

std::wstring format_pc_address(uint32_t address){
	std::wostringstream text;text<<L"0x"<<std::hex<<std::uppercase<<std::setfill(L'0')<<std::setw(8)<<address;return text.str();
}

std::wstring win_error(const wchar_t* operation,DWORD code){
	return std::wstring(operation)+L" failed (Windows error "+std::to_wstring(code)+L").";
}

bool file_exists(const std::filesystem::path& path){
	const DWORD attributes=GetFileAttributesW(path.c_str());
	return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_DIRECTORY);
}

bool read_text(const std::filesystem::path& path,const wchar_t* section,const wchar_t* key,std::wstring& value,bool& present,std::wstring& error){
	std::vector<wchar_t> buffer(32768);
	const DWORD length=GetPrivateProfileStringW(section,key,missing_value,buffer.data(),static_cast<DWORD>(buffer.size()),path.c_str());
	if(length>=buffer.size()-1){error=std::wstring(L"Configuration value is too long: ")+section+L"/"+key;return false;}
	present=!(buffer[0]==missing_value[0]&&buffer[1]==L'\0');
	value.assign(buffer.data(),length);
	return true;
}

bool read_unsigned(const std::filesystem::path& path,const wchar_t* section,const wchar_t* key,unsigned fallback,unsigned minimum,unsigned maximum,unsigned& value,std::wstring& error){
	std::wstring text;bool present=false;
	if(!read_text(path,section,key,text,present,error))return false;
	if(!present){value=fallback;return true;}
	if(text.empty()){error=std::wstring(L"Empty numeric setting: ")+section+L"/"+key;return false;}
	if(!std::all_of(text.begin(),text.end(),[](wchar_t ch){return ch>=L'0'&&ch<=L'9';})){error=std::wstring(L"Invalid setting value: ")+section+L"/"+key;return false;}
	wchar_t* end=nullptr;errno=0;const unsigned long parsed=std::wcstoul(text.c_str(),&end,10);
	if(errno||end==text.c_str()||*end||parsed<minimum||parsed>maximum){error=std::wstring(L"Invalid setting value: ")+section+L"/"+key;return false;}
	value=static_cast<unsigned>(parsed);
	return true;
}

bool read_boolean(const std::filesystem::path& path,const wchar_t* section,const wchar_t* key,bool fallback,bool& value,std::wstring& error){
	unsigned parsed=0;
	if(!read_unsigned(path,section,key,fallback?1u:0u,0,1,parsed,error))return false;
	value=parsed!=0;
	return true;
}

bool read_keyboard(const std::filesystem::path& path,Preferences& value,std::wstring& error){
	if(!read_boolean(path,L"Keyboard",L"Wasd",false,value.wasd_profile,error)||
	   !read_boolean(path,L"Keyboard",L"NeutralOpposites",false,value.neutral_opposites,error))return false;
	for(unsigned key=0;key<value.keyboard_keys.size();key++){
		const auto name=L"Key"+std::to_wstring(key);
		if(!read_unsigned(path,L"Keyboard",name.c_str(),value.keyboard_keys[key],0,255,value.keyboard_keys[key],error))return false;
	}
	return true;
}

bool write_profile_value(const std::filesystem::path& path,const wchar_t* section,const wchar_t* key,const std::wstring& value,std::wstring& error){
	if(WritePrivateProfileStringW(section,key,value.c_str(),path.c_str()))return true;
	error=win_error(L"WritePrivateProfileStringW",GetLastError());
	return false;
}

std::wstring key_name(unsigned key){
	if(!key)return L"Not bound";
	const UINT scan=MapVirtualKeyW(key,MAPVK_VK_TO_VSC);
	wchar_t name[64]{};
	if(scan&&GetKeyNameTextW(static_cast<LONG>(scan<<16),name,static_cast<int>(sizeof(name)/sizeof(name[0]))))return name;
	std::wostringstream text;text<<L"VK 0x"<<std::hex<<key;return text.str();
}

struct WindowState {
	HWND window=nullptr,owner=nullptr,categories_list=nullptr;
	HWND cpu_statistics_control=nullptr;
	Preferences* value=nullptr;
	std::filesystem::path path;
	SettingsActions actions;
	std::vector<HWND> page_controls;
	HFONT font=nullptr,title_font=nullptr;
	HBRUSH background=nullptr,panel=nullptr;
	unsigned page=0;
	unsigned font_size=16;
	int listening_key=-1;
};

HWND current_window=nullptr;
WindowState* pending_state=nullptr;
void build_page(WindowState& state);

void rebuild_brushes(WindowState& state){
	if(state.background)DeleteObject(state.background);
	if(state.panel)DeleteObject(state.panel);
	state.background=CreateSolidBrush(ps1ui::background);
	state.panel=CreateSolidBrush(ps1ui::panel);
}

void rebuild_fonts(WindowState& state){
	if(state.font)DeleteObject(state.font);if(state.title_font)DeleteObject(state.title_font);
	state.font=CreateFontW(-static_cast<int>(state.font_size),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
	state.title_font=CreateFontW(-static_cast<int>(state.font_size*11/8),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
}

void clear_page(WindowState& state){
	for(HWND control:state.page_controls)if(IsWindow(control))DestroyWindow(control);
	state.page_controls.clear();state.listening_key=-1;state.cpu_statistics_control=nullptr;
}

HWND add_control(WindowState& state,const wchar_t* class_name,const wchar_t* text,DWORD style,int id,int x,int y,int w,int h){
	HWND control=CreateWindowExW(0,class_name,text,style|WS_CHILD|WS_VISIBLE,x,y,w,h,state.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
	if(control){SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(state.font),TRUE);state.page_controls.push_back(control);}
	return control;
}

HWND add_label_at(WindowState& state,const std::wstring& text,int x,int y,int w,int h=26){
	return add_control(state,L"STATIC",text.c_str(),SS_LEFT,0,x,y,w,h);
}

HWND add_label(WindowState& state,const std::wstring& text,int y,int h=26){
	return add_label_at(state,text,238,y,680,h);
}

HWND add_button(WindowState& state,const wchar_t* text,int id,int x,int y,int w=260){
	return add_control(state,L"BUTTON",text,BS_PUSHBUTTON|WS_TABSTOP,id,x,y,w,30);
}

HWND add_checkbox(WindowState& state,const wchar_t* text,int id,int y,bool checked){
	HWND control=add_control(state,L"BUTTON",text,BS_AUTOCHECKBOX|WS_TABSTOP,id,238,y,660,28);
	if(control)SendMessageW(control,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);
	return control;
}

HWND add_checkbox_at(WindowState& state,const wchar_t* text,int id,int x,int y,int w,bool checked){
	HWND control=add_control(state,L"BUTTON",text,BS_AUTOCHECKBOX|WS_TABSTOP,id,x,y,w,28);
	if(control)SendMessageW(control,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);
	return control;
}

HWND add_combo(WindowState& state,int id,int y,const std::vector<const wchar_t*>& options,unsigned selection){
	HWND control=add_control(state,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,id,238,y,360,260);
	if(!control)return nullptr;
	for(const wchar_t* option:options)SendMessageW(control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(option));
	SendMessageW(control,CB_SETCURSEL,selection,0);
	return control;
}

HWND add_combo_at(WindowState& state,int id,int x,int y,int w,const std::vector<const wchar_t*>& options,unsigned selection){
	HWND control=add_control(state,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,id,x,y,w,260);
	if(!control)return nullptr;
	for(const wchar_t* option:options)SendMessageW(control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(option));
	SendMessageW(control,CB_SETCURSEL,selection,0);return control;
}

HWND add_edit(WindowState& state,const std::wstring& text,int id,int x,int y,int w){
	return add_control(state,L"EDIT",text.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,id,x,y,w,26);
}

std::wstring cpu_statistics_text(const Preferences& preferences,const SettingsSnapshot& snapshot){
	std::wostringstream text;text<<L"Instructions: "<<snapshot.cpu_instructions<<L" | measured MIPS (Runner 1-s interval): ";
	if(snapshot.cpu_mips_valid)text<<std::fixed<<std::setprecision(2)<<snapshot.cpu_mips;
	else text<<L"n/a (Runner idle / warming up)";
	text<<L"\r\nProfiler: "<<(preferences.cpu_profiler_enabled?L"Enabled":L"Disabled")<<L"; samples="<<snapshot.cpu_profile_samples<<L"; interval="<<preferences.cpu_profile_sample_interval
		<<L"\r\nSampled host ns (fetch/trace/decode/execute): "<<snapshot.cpu_profile_fetch_ns<<L"/"<<snapshot.cpu_profile_trace_ns<<L"/"<<snapshot.cpu_profile_decode_ns<<L"/"<<snapshot.cpu_profile_execute_ns
		<<L"\r\nTrace: "<<(preferences.cpu_trace_enabled?L"Enabled":L"Disabled")<<L" "<<snapshot.cpu_trace_records<<L"/"<<(snapshot.cpu_trace_capacity?snapshot.cpu_trace_capacity:preferences.cpu_trace_buffer_size)
		<<L"; diagnostics: "<<(preferences.cpu_diagnostic_logging?L"Enabled":L"Disabled")<<L" "<<snapshot.cpu_diagnostic_events<<L"/64; times are host ns, not emulated cycles.";
	return text.str();
}

void update_cpu_statistics(WindowState& state){
	if(!state.cpu_statistics_control||!state.actions.snapshot||!state.value)return;
	const auto snapshot=state.actions.snapshot();const auto text=cpu_statistics_text(*state.value,snapshot);
	SetWindowTextW(state.cpu_statistics_control,text.c_str());
}

std::wstring region_name(unsigned region){
	switch(region){case 1:return L"USA (GetID metadata)";case 2:return L"Europe (GetID metadata)";case 3:return L"Japan (GetID metadata)";default:return L"Unknown / unlicensed";}
}

void apply_changes(WindowState& state){
	if(state.actions.changed)state.actions.changed();
	InvalidateRect(state.window,nullptr,TRUE);
}

void show_message(WindowState& state,const std::wstring& message,const wchar_t* title=L"AVOPSEMU Settings"){
	MessageBoxW(state.window,message.c_str(),title,MB_OK|MB_ICONINFORMATION);
}

void import_profile(WindowState& state){
	wchar_t file[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=state.window;dialog.lpstrFile=file;dialog.nMaxFile=static_cast<DWORD>(sizeof(file)/sizeof(file[0]));dialog.lpstrFilter=L"AVOPSEMU settings (*.ini)\0*.ini\0All files\0*.*\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
	if(!GetOpenFileNameW(&dialog))return;
	const auto loaded=read_preferences(std::filesystem::path(file));
	if(!loaded.error.empty()){MessageBoxW(state.window,loaded.error.c_str(),L"Invalid settings profile",MB_OK|MB_ICONERROR);return;}
	if(!loaded.found){show_message(state,L"Selected file contains no AVOPSEMU settings.",L"Invalid settings profile");return;}
	*state.value=loaded.value;apply_changes(state);build_page(state);
}

void export_profile(WindowState& state){
	wchar_t file[32768]=L"avopsemu-settings.ini";OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=state.window;dialog.lpstrFile=file;dialog.nMaxFile=static_cast<DWORD>(sizeof(file)/sizeof(file[0]));dialog.lpstrFilter=L"AVOPSEMU settings (*.ini)\0*.ini\0";dialog.lpstrDefExt=L"ini";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
	if(!GetSaveFileNameW(&dialog))return;
	std::wstring error;if(!write_preferences(std::filesystem::path(file),*state.value,error)){MessageBoxW(state.window,error.c_str(),L"Could not export settings",MB_OK|MB_ICONERROR);return;}
	show_message(state,L"Settings profile exported and verified by the configuration writer.");
}

void build_page(WindowState& state){
	clear_page(state);
	if(!state.value)return;
	const Preferences& p=*state.value;
	switch(state.page){
	case 0:{
		add_label_at(state,L"Live CPU Performance Statistics",238,82,680);
		state.cpu_statistics_control=add_label_at(state,L"",238,108,680,92);

		add_label_at(state,L"Performance",238,204,320);
		add_checkbox_at(state,L"Enable CPU profiler",cpu_profiler_id,238,230,320,p.cpu_profiler_enabled);
		add_label_at(state,L"Measures host time; emulated cycles stay unchanged.",238,258,330,26);
		add_label_at(state,L"Sample interval (instructions)",238,286,320,22);
		add_combo_at(state,cpu_profile_interval_id,238,309,320,{L"64",L"256",L"1,024",L"4,096",L"16,384",L"65,536"},selection_for(cpu_profile_intervals,p.cpu_profile_sample_interval));

		add_label_at(state,L"Instruction Trace",238,348,320);
		add_checkbox_at(state,L"Enable instruction trace",cpu_trace_id,238,374,320,p.cpu_trace_enabled);
		add_label_at(state,L"Separate from the 128-entry crash trace.",238,402,330,26);
		add_label_at(state,L"Trace buffer size (records)",238,432,190,22);
		add_combo_at(state,cpu_trace_size_id,238,455,190,{L"128",L"512",L"2,048",L"8,192",L"16,384"},selection_for(cpu_trace_sizes,p.cpu_trace_buffer_size));
		add_button(state,L"Export trace...",cpu_trace_export_id,436,455,122);
		add_label_at(state,L"Maximum 16,384 records; resized on apply.",238,490,330,26);
		add_button(state,L"Restore CPU Defaults",cpu_defaults_id,238,532,190);

		add_label_at(state,L"Debugging",574,204,354);
		add_checkbox_at(state,L"Break on reserved instruction",cpu_break_reserved_id,574,230,354,p.cpu_break_on_reserved);
		add_label_at(state,L"RI stop; Cause/EPC/Status stay intact.",574,258,370,26);
		add_checkbox_at(state,L"Break on CPU exception",cpu_break_exception_id,574,288,370,p.cpu_break_on_exception);
		add_label_at(state,L"Stops on CPU exceptions and interrupts.",574,316,370,26);
		add_checkbox_at(state,L"Enable PC breakpoint",cpu_pc_breakpoint_id,574,346,370,p.cpu_pc_breakpoint_enabled);
		add_label_at(state,L"Software stop; COP0 debug registers untouched.",574,374,370,26);
		add_label_at(state,L"Address (hex; 32-bit, 4-byte aligned)",574,404,354,22);
		add_edit(state,format_pc_address(p.cpu_pc_breakpoint_address),cpu_pc_address_id,574,428,164);
		add_button(state,L"Set",cpu_pc_set_id,744,426,72);
		add_button(state,L"Clear",cpu_pc_clear_id,820,426,92);

		add_label_at(state,L"Diagnostics",574,464,354);
		add_checkbox_at(state,L"Enable CPU diagnostic logging",cpu_diagnostics_id,574,490,354,p.cpu_diagnostic_logging);
		add_label_at(state,L"64-event ring; exceptions/host errors only.",574,518,370,26);
		add_button(state,L"Export CPU diagnostics...",cpu_diagnostics_export_id,574,558,190);
		add_label_at(state,L"Not supported: JIT or CPU timing overrides.",238,604,680,26);
		update_cpu_statistics(state);
		break;}
	case 1:
		add_label(state,L"Renderer: deterministic software rasterizer; VRAM is 1024 x 512 pixels.",82);
		add_label(state,L"Internal resolution and texture filtering overrides are not supported.",116);
		add_label(state,L"Dithering, blending, mask and texture-page behavior follow GP0 commands; no global override is exposed.",150,46);
		add_label(state,L"Presentation uses the current GDI display path. Hardware acceleration is not implemented.",204,42);
		break;
	case 2:
		add_label(state,L"Disc image input: CUE/BIN and ISO subset. Drive timing is modeled by the core.",82,42);
		add_label(state,L"Read speed, drive seek model and compatibility overrides are not configurable.",132,42);
		add_label(state,L"Region metadata: derived from SYSTEM.CNF boot executable when available; not SCEx validation.",182,46);
		break;
	case 3:
		add_label(state,L"Output: 44.1 kHz, stereo, 16-bit PCM through Windows waveOut.",82);
		add_label(state,L"Host queue: 8 blocks x 512 frames; these values are fixed in the current backend.",116);
		add_label(state,L"Resampling/time-stretching is core-managed; sample-rate and latency controls are not supported.",150,42);
		add_checkbox(state,L"Mute Windows audio output",mute_id,214,p.muted);
		break;
	case 4:{
		const auto snapshot=state.actions.snapshot?state.actions.snapshot():SettingsSnapshot{};
		add_label(state,L"Selected BIOS: "+(snapshot.bios_path.empty()?L"(none)":snapshot.bios_path),82,44);
		add_label(state,snapshot.bios_loaded?L"Status: loaded and validated for this session.":L"Status: not loaded in this session.",134);
		add_label(state,L"Validation requires exactly 512 KiB; the BIOS is not bundled with AVOPSEMU.",168,40);
		add_button(state,L"Browse and load BIOS...",bios_id,238,226,250);
		if(snapshot.disc_mounted){add_label(state,L"Mounted disc: "+snapshot.disc_name,278,36);add_label(state,L"Detected region: "+region_name(snapshot.region),322,28);}
		break;}
	case 5:
		add_checkbox(state,L"Limit execution to modeled 1x speed",pace_id,92,p.limit_speed);
		add_label(state,L"This option is connected to Runner pacing. Turning it off runs as fast as the host allows.",136,44);
		add_label(state,L"VSync is not available in the GDI presentation path; modeled VBlank is not display FPS.",190,44);
		break;
	case 6:{
		add_label(state,L"Keyboard profile: choose a preset, then select a mapping and press a key.",78,30);
		add_button(state,L"Default arrows + Z/X/A/S",default_keys_id,238,112,260);
		add_button(state,L"WASD + J/K/U/I",wasd_keys_id,510,112,230);
		add_checkbox(state,L"Neutralize opposite directions",neutral_id,150,p.neutral_opposites);
		static const wchar_t* names[]={L"Select",L"L3",L"R3",L"Start",L"Up",L"Right",L"Down",L"Left",L"L2",L"R2",L"L1",L"R1",L"Triangle",L"Circle",L"Cross",L"Square"};
		for(unsigned n=0;n<16;n++){
			const int column=n<8?0:1;const int row=int(n%8);const int x=238+column*330;const int y=190+row*36;
			const std::wstring text=std::wstring(names[n])+L": "+(n==1||n==2?L"not supported":key_name(p.keyboard_keys[n]));
			HWND control=add_button(state,text.c_str(),key_base_id+int(n),x,y,310);
			if(control&&(n==1||n==2))EnableWindow(control,FALSE);
		}
		add_label(state,L"Gamepad/analog input and per-game mapping profiles are not implemented.",490,40);
		break;}
	case 7:
		add_label(state,L"Presentation settings affect only the host display; PS1 VRAM and framebuffer resolution stay unchanged.",82,42);
		add_checkbox(state,L"Stretch Screen to fill the display area",stretch_screen_id,140,p.stretch_screen);
		add_label(state,L"Off keeps the current 4:3 fit. On stretches the image to the full game-view area.",174,42);
		add_checkbox(state,L"Fullscreen (borderless window)",fullscreen_id,238,p.fullscreen);
		add_label(state,L"F11 toggles fullscreen; Escape exits fullscreen. Window size and maximized state are restored.",272,50);
		break;
	case 8:
	default:
		add_label(state,L"Theme",82);add_combo(state,theme_id,106,{L"Dark",L"Light",L"AMOLED"},static_cast<unsigned>(p.theme));
		add_label(state,L"Accent color",150);add_combo(state,accent_id,174,{L"Violet",L"Blue",L"Green",L"Orange"},p.accent);
		add_label(state,L"Font size",218);add_combo(state,font_id,242,{L"12",L"14",L"16",L"18",L"20",L"22",L"24"},(p.font_size-12)/2);
		add_checkbox(state,L"Show toolbar",toolbar_id,286,p.show_toolbar);
		add_checkbox(state,L"Show status bar",status_id,316,p.show_status);
		add_checkbox(state,L"Show FPS HUD (GDI submissions; not monitor refresh rate)",fps_id,346,p.show_fps);
		add_checkbox(state,L"Show sidebar",sidebar_id,376,p.show_sidebar);
		add_checkbox(state,L"Show debugger panel",debugger_id,406,p.show_debugger);
		add_button(state,L"Restore UI defaults",reset_ui_id,238,450,190);
		add_button(state,L"Import...",import_id,438,450,130);
		add_button(state,L"Export...",export_id,580,450,130);
		add_label(state,L"Dockable panels, animation controls and language packs are not implemented; no controls are exposed.",502,48);
		add_label(state,L"Icon-size scaling is not implemented; native controls follow the system metrics.",552,36);
		break;
	}
}

void set_preset(Preferences& p,bool wasd){
	p.wasd_profile=wasd;
	if(wasd)p.keyboard_keys={0x09,0,0,0x0d,'W','D','S','A','1','3','Q','E','I','K','J','U'};
	else p.keyboard_keys=ps1::KeyboardPad::defaults;
}

LRESULT CALLBACK settings_proc(HWND window,UINT message,WPARAM wp,LPARAM lp){
	WindowState* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
	if(message==WM_NCCREATE){auto* create=reinterpret_cast<CREATESTRUCTW*>(lp);state=static_cast<WindowState*>(create->lpCreateParams);state->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}
	if(!state)return DefWindowProcW(window,message,wp,lp);
	switch(message){
	case WM_CREATE:{
		rebuild_brushes(*state);rebuild_fonts(*state);
		state->categories_list=CreateWindowExW(0,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,20,68,190,560,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(category_id)),GetModuleHandleW(nullptr),nullptr);
		if(!state->font||!state->title_font||!state->background||!state->panel||!state->categories_list)return -1;
		SendMessageW(state->categories_list,WM_SETFONT,reinterpret_cast<WPARAM>(state->font),TRUE);for(unsigned n=0;n<category_count;n++)SendMessageW(state->categories_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(categories[n]));SendMessageW(state->categories_list,LB_SETCURSEL,state->page,0);
		if(!SetTimer(window,cpu_stats_timer_id,1000,nullptr))return -1;
		build_page(*state);return 0;}
	case WM_PAINT:{PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);RECT area{};GetClientRect(window,&area);FillRect(dc,&area,state->background);ps1ui::label(dc,L"Settings Center",RECT{22,20,430,54},state->title_font,ps1ui::text);ps1ui::label(dc,L"Hardware capabilities and persisted frontend preferences",RECT{438,24,928,50},state->font,ps1ui::muted,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);ps1ui::box(dc,RECT{16,60,216,638},ps1ui::panel,ps1ui::border,8);ps1ui::box(dc,RECT{226,60,944,638},ps1ui::panel,ps1ui::border,8);EndPaint(window,&paint);return 0;}
	case WM_ERASEBKGND:return 1;
	case WM_CTLCOLORSTATIC:case WM_CTLCOLORLISTBOX:{HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,ps1ui::text);SetBkColor(dc,ps1ui::panel);return reinterpret_cast<LRESULT>(state->panel);}
	case WM_CTLCOLORBTN:{HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,ps1ui::text);SetBkColor(dc,ps1ui::panel);return reinterpret_cast<LRESULT>(state->panel);}
	case WM_COMMAND:{
		const int id=LOWORD(wp),code=HIWORD(wp);Preferences& p=*state->value;
		if(id==category_id&&code==LBN_SELCHANGE){const LRESULT selected=SendMessageW(state->categories_list,LB_GETCURSEL,0,0);if(selected>=0){state->page=static_cast<unsigned>(selected);build_page(*state);}return 0;}
		if(code==CBN_SELCHANGE){const LRESULT selected=SendMessageW(reinterpret_cast<HWND>(lp),CB_GETCURSEL,0,0);if(selected<0)return 0;
			if(id==theme_id)p.theme=static_cast<Theme>(selected);
			else if(id==accent_id)p.accent=static_cast<unsigned>(selected);
			else if(id==font_id)p.font_size=12+2*static_cast<unsigned>(selected);
			else if(id==cpu_profile_interval_id){if(static_cast<size_t>(selected)>=std::size(cpu_profile_intervals))return 0;p.cpu_profile_sample_interval=cpu_profile_intervals[selected];}
			else if(id==cpu_trace_size_id){if(static_cast<size_t>(selected)>=std::size(cpu_trace_sizes))return 0;p.cpu_trace_buffer_size=cpu_trace_sizes[selected];}
			else return 0;
			apply_changes(*state);return 0;
		}
		if(code!=BN_CLICKED)return 0;
		if(id==cpu_trace_export_id){if(state->actions.export_cpu_trace)state->actions.export_cpu_trace();else show_message(*state,L"CPU trace export is unavailable.");return 0;}
		if(id==cpu_diagnostics_export_id){if(state->actions.export_cpu_diagnostics)state->actions.export_cpu_diagnostics();else show_message(*state,L"CPU diagnostic export is unavailable.");return 0;}
		if(id==cpu_pc_set_id){
			wchar_t text[64]{};GetWindowTextW(GetDlgItem(window,cpu_pc_address_id),text,static_cast<int>(std::size(text)));uint32_t address=0;
			if(!parse_pc_address(text,address)){show_message(*state,L"Enter a 32-bit hexadecimal PC address aligned to 4 bytes (for example 0x80010000).",L"Invalid CPU PC breakpoint");return 0;}
			p.cpu_pc_breakpoint_address=address;p.cpu_pc_breakpoint_enabled=true;apply_changes(*state);return 0;
		}
		if(id==cpu_pc_clear_id){p.cpu_pc_breakpoint_enabled=false;p.cpu_pc_breakpoint_address=0;apply_changes(*state);return 0;}
		if(id==cpu_defaults_id){restore_cpu_defaults(p);apply_changes(*state);return 0;}
		if(id==toolbar_id)p.show_toolbar=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==status_id)p.show_status=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==fps_id)p.show_fps=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==sidebar_id)p.show_sidebar=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==debugger_id)p.show_debugger=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==stretch_screen_id)p.stretch_screen=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==fullscreen_id)p.fullscreen=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==cpu_profiler_id)p.cpu_profiler_enabled=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==cpu_trace_id)p.cpu_trace_enabled=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==cpu_break_reserved_id)p.cpu_break_on_reserved=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==cpu_break_exception_id)p.cpu_break_on_exception=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==cpu_pc_breakpoint_id)p.cpu_pc_breakpoint_enabled=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==cpu_diagnostics_id)p.cpu_diagnostic_logging=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==mute_id)p.muted=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==pace_id)p.limit_speed=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==bios_id){if(state->actions.choose_bios)state->actions.choose_bios();build_page(*state);return 0;}
		else if(id==default_keys_id){set_preset(p,false);apply_changes(*state);build_page(*state);return 0;}
		else if(id==wasd_keys_id){set_preset(p,true);apply_changes(*state);build_page(*state);return 0;}
		else if(id==neutral_id)p.neutral_opposites=SendMessageW(reinterpret_cast<HWND>(lp),BM_GETCHECK,0,0)==BST_CHECKED;
		else if(id==reset_ui_id){const Preferences defaults{};p.theme=defaults.theme;p.accent=defaults.accent;p.font_size=defaults.font_size;p.show_toolbar=defaults.show_toolbar;p.show_status=defaults.show_status;p.show_fps=defaults.show_fps;p.show_sidebar=defaults.show_sidebar;p.show_debugger=defaults.show_debugger;p.stretch_screen=defaults.stretch_screen;p.fullscreen=defaults.fullscreen;p.window_width=defaults.window_width;p.window_height=defaults.window_height;p.window_maximized=defaults.window_maximized;apply_changes(*state);SetWindowPos(state->owner,nullptr,0,0,static_cast<int>(p.window_width),static_cast<int>(p.window_height),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(state->owner,p.window_maximized?SW_SHOWMAXIMIZED:SW_RESTORE);build_page(*state);return 0;}
		else if(id==export_id){export_profile(*state);return 0;}
		else if(id==import_id){import_profile(*state);return 0;}
		else if(id>=key_base_id&&id<key_base_id+16){const unsigned key=static_cast<unsigned>(id-key_base_id);if(key==1||key==2)return 0;state->listening_key=static_cast<int>(key);SetWindowTextW(reinterpret_cast<HWND>(lp),L"Press a key...");SetFocus(window);return 0;}
		else return 0;
		apply_changes(*state);return 0;}
	case WM_KEYDOWN:
		if(state->listening_key>=0){
			const unsigned key=static_cast<unsigned>(wp);const unsigned target=static_cast<unsigned>(state->listening_key);
			if(key==VK_ESCAPE){build_page(*state);return 0;}
			bool valid=key<256&&key!=VK_ESCAPE&&key!=VK_MENU;
			for(unsigned n=0;n<state->value->keyboard_keys.size()&&valid;n++)if(n!=target&&state->value->keyboard_keys[n]==key)valid=false;
			if(!valid){MessageBeep(MB_ICONWARNING);return 0;}
			state->value->keyboard_keys[target]=key;state->value->wasd_profile=false;state->listening_key=-1;apply_changes(*state);build_page(*state);return 0;
		}
		break;
	case WM_TIMER:
		if(wp==cpu_stats_timer_id&&state->page==0){update_cpu_statistics(*state);return 0;}
		break;
	case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize={900,650};return 0;}
	case WM_CLOSE:DestroyWindow(window);return 0;
	case WM_NCDESTROY:
		if(current_window==window)current_window=nullptr;
		if(pending_state==state)pending_state=nullptr;
		KillTimer(window,cpu_stats_timer_id);
		clear_page(*state);if(state->font)DeleteObject(state->font);if(state->title_font)DeleteObject(state->title_font);if(state->background)DeleteObject(state->background);if(state->panel)DeleteObject(state->panel);
		SetWindowLongPtrW(window,GWLP_USERDATA,0);delete state;return DefWindowProcW(window,message,wp,lp);
	}
	return DefWindowProcW(window,message,wp,lp);
}
}

std::filesystem::path user_preferences_path(){
	wchar_t directory[32768]{};const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",directory,static_cast<DWORD>(sizeof(directory)/sizeof(directory[0])));
	if(!length||length>=sizeof(directory)/sizeof(directory[0]))throw std::runtime_error("LOCALAPPDATA is unavailable or too long");
	const auto root=std::filesystem::path(directory)/L"AVOPSEMU";std::error_code error;std::filesystem::create_directories(root,error);
	if(error)throw std::runtime_error("Cannot create AVOPSEMU user-data directory: "+error.message());
	return root/L"settings.ini";
}

bool validate_preferences(const Preferences& value,std::wstring& error){
	error.clear();
	if(static_cast<unsigned>(value.theme)>static_cast<unsigned>(Theme::Amoled)){error=L"Theme value is outside the supported range.";return false;}
	if(value.accent>3){error=L"Accent color is outside the supported range.";return false;}
	if(value.font_size<12||value.font_size>24||(value.font_size&1)){error=L"Font size must be an even value from 12 to 24.";return false;}
	if(value.window_width<1040||value.window_width>8192||value.window_height<780||value.window_height>8192){error=L"Saved window dimensions are outside the supported range.";return false;}
	if(!includes(cpu_profile_intervals,value.cpu_profile_sample_interval)){error=L"CPU profile sample interval is outside the supported choices.";return false;}
	if(!includes(cpu_trace_sizes,value.cpu_trace_buffer_size)){error=L"CPU trace buffer size is outside the supported choices.";return false;}
	if(value.cpu_pc_breakpoint_address&3){error=L"CPU PC breakpoint address must be 4-byte aligned.";return false;}
	if(value.bios_path.find_first_of(L"\r\n")!=std::wstring::npos){error=L"BIOS path contains an invalid line break.";return false;}
	for(unsigned i=0;i<value.keyboard_keys.size();i++){
		const unsigned key=value.keyboard_keys[i];
		if(key>255||key==0x1b||key==0x12||(i==1||i==2)&&key){error=L"Keyboard mapping contains an unsupported virtual key.";return false;}
		if(key)for(unsigned j=0;j<i;j++)if(value.keyboard_keys[j]==key){error=L"Keyboard mapping contains duplicate keys.";return false;}
	}
	return true;
}

void restore_cpu_defaults(Preferences& value){
	const Preferences defaults{};
	value.cpu_profiler_enabled=defaults.cpu_profiler_enabled;
	value.cpu_profile_sample_interval=defaults.cpu_profile_sample_interval;
	value.cpu_trace_enabled=defaults.cpu_trace_enabled;
	value.cpu_trace_buffer_size=defaults.cpu_trace_buffer_size;
	value.cpu_break_on_reserved=defaults.cpu_break_on_reserved;
	value.cpu_break_on_exception=defaults.cpu_break_on_exception;
	value.cpu_pc_breakpoint_enabled=defaults.cpu_pc_breakpoint_enabled;
	value.cpu_pc_breakpoint_address=defaults.cpu_pc_breakpoint_address;
	value.cpu_diagnostic_logging=defaults.cpu_diagnostic_logging;
}

PreferenceLoadResult read_preferences(const std::filesystem::path& path,const std::filesystem::path& legacy_keyboard_path){
	PreferenceLoadResult result;
	std::filesystem::path source=path;
	if(!file_exists(path)){
		if(legacy_keyboard_path.empty()||!file_exists(legacy_keyboard_path))return result;
		source=legacy_keyboard_path;result.migrated=true;
		if(!read_keyboard(source,result.value,result.error))return result;
		std::wstring validation;if(!validate_preferences(result.value,validation))result.error=validation;
		return result;
	}
	result.found=true;unsigned version=0;
	if(!read_unsigned(source,L"Meta",L"SchemaVersion",0,0,100,version,result.error))return result;
	if(version>current_schema){result.error=L"Settings schema is newer than this AVOPSEMU build.";return result;}
	if(version<current_schema)result.migrated=true;
	Preferences candidate{};
	if(version>=1){
		unsigned value=0;
		if(!read_unsigned(source,L"UI",L"Theme",static_cast<unsigned>(candidate.theme),0,2,value,result.error))return result;candidate.theme=static_cast<Theme>(value);
		if(!read_unsigned(source,L"UI",L"Accent",candidate.accent,0,3,candidate.accent,result.error)||
		   !read_unsigned(source,L"UI",L"FontSize",candidate.font_size,12,24,candidate.font_size,result.error)||
		   !read_boolean(source,L"UI",L"ShowToolbar",candidate.show_toolbar,candidate.show_toolbar,result.error)||
		   !read_boolean(source,L"UI",L"ShowStatus",candidate.show_status,candidate.show_status,result.error)||
		   !read_boolean(source,L"UI",L"ShowFps",candidate.show_fps,candidate.show_fps,result.error)||
		   !read_boolean(source,L"UI",L"ShowSidebar",candidate.show_sidebar,candidate.show_sidebar,result.error)||
		   !read_boolean(source,L"UI",L"ShowDebugger",candidate.show_debugger,candidate.show_debugger,result.error)||
		   !read_boolean(source,L"UI",L"StretchScreen",candidate.stretch_screen,candidate.stretch_screen,result.error)||
		   !read_boolean(source,L"Hardware",L"LimitSpeed",candidate.limit_speed,candidate.limit_speed,result.error)||
		   !read_boolean(source,L"Hardware",L"Muted",candidate.muted,candidate.muted,result.error)||
		   !read_unsigned(source,L"Window",L"Width",candidate.window_width,1040,8192,candidate.window_width,result.error)||
		   !read_unsigned(source,L"Window",L"Height",candidate.window_height,780,8192,candidate.window_height,result.error)||
		   !read_boolean(source,L"Window",L"Maximized",candidate.window_maximized,candidate.window_maximized,result.error)||
		   !read_boolean(source,L"Window",L"Fullscreen",candidate.fullscreen,candidate.fullscreen,result.error)||
		   !read_keyboard(source,candidate,result.error))return result;
		bool present=false;if(!read_text(source,L"Hardware",L"BiosPath",candidate.bios_path,present,result.error))return result;
		if(version>=2){
			unsigned breakpoint_address=0;
			if(!read_boolean(source,L"CPU",L"ProfilerEnabled",candidate.cpu_profiler_enabled,candidate.cpu_profiler_enabled,result.error)||
			   !read_unsigned(source,L"CPU",L"ProfileSampleInterval",candidate.cpu_profile_sample_interval,64,65536,candidate.cpu_profile_sample_interval,result.error)||
			   !read_boolean(source,L"CPU",L"InstructionTraceEnabled",candidate.cpu_trace_enabled,candidate.cpu_trace_enabled,result.error)||
			   !read_unsigned(source,L"CPU",L"TraceBufferSize",candidate.cpu_trace_buffer_size,128,16384,candidate.cpu_trace_buffer_size,result.error)||
			   !read_boolean(source,L"CPU",L"BreakOnReservedInstruction",candidate.cpu_break_on_reserved,candidate.cpu_break_on_reserved,result.error)||
			   !read_boolean(source,L"CPU",L"BreakOnException",candidate.cpu_break_on_exception,candidate.cpu_break_on_exception,result.error)||
			   !read_boolean(source,L"CPU",L"PCBreakpointEnabled",candidate.cpu_pc_breakpoint_enabled,candidate.cpu_pc_breakpoint_enabled,result.error)||
			   !read_unsigned(source,L"CPU",L"PCBreakpointAddress",0,0,0xffffffffu,breakpoint_address,result.error)||
			   !read_boolean(source,L"CPU",L"DiagnosticLogging",candidate.cpu_diagnostic_logging,candidate.cpu_diagnostic_logging,result.error))return result;
			candidate.cpu_pc_breakpoint_address=breakpoint_address;
		}
	}else if(!read_keyboard(source,candidate,result.error))return result;
	std::wstring validation;if(!validate_preferences(candidate,validation)){result.error=validation;return result;}
	result.value=std::move(candidate);return result;
}

bool write_preferences(const std::filesystem::path& path,const Preferences& value,std::wstring& error){
	error.clear();if(path.empty()){error=L"Settings path is empty.";return false;}
	if(!validate_preferences(value,error))return false;
	std::error_code filesystem_error;if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path(),filesystem_error);
	if(filesystem_error){const auto message=filesystem_error.message();error=L"Cannot create settings directory: "+std::wstring(message.begin(),message.end());return false;}
	auto temporary=path;temporary+=L".tmp";
	if(!DeleteFileW(temporary.c_str())){const DWORD code=GetLastError();if(code!=ERROR_FILE_NOT_FOUND){error=win_error(L"Delete stale settings temporary",code);return false;}}
	const auto number=[](unsigned n){return std::to_wstring(n);};
	const auto boolean=[](bool b){return std::wstring(b?L"1":L"0");};
	auto put=[&](const wchar_t* section,const wchar_t* key,const std::wstring& text){return write_profile_value(temporary,section,key,text,error);};
	if(!put(L"Meta",L"SchemaVersion",number(current_schema))||
	   !put(L"UI",L"Theme",number(static_cast<unsigned>(value.theme)))||!put(L"UI",L"Accent",number(value.accent))||!put(L"UI",L"FontSize",number(value.font_size))||
	   !put(L"UI",L"ShowToolbar",boolean(value.show_toolbar))||!put(L"UI",L"ShowStatus",boolean(value.show_status))||!put(L"UI",L"ShowFps",boolean(value.show_fps))||!put(L"UI",L"ShowSidebar",boolean(value.show_sidebar))||!put(L"UI",L"ShowDebugger",boolean(value.show_debugger))||!put(L"UI",L"StretchScreen",boolean(value.stretch_screen))||
	   !put(L"CPU",L"ProfilerEnabled",boolean(value.cpu_profiler_enabled))||!put(L"CPU",L"ProfileSampleInterval",number(value.cpu_profile_sample_interval))||!put(L"CPU",L"InstructionTraceEnabled",boolean(value.cpu_trace_enabled))||!put(L"CPU",L"TraceBufferSize",number(value.cpu_trace_buffer_size))||
	   !put(L"CPU",L"BreakOnReservedInstruction",boolean(value.cpu_break_on_reserved))||!put(L"CPU",L"BreakOnException",boolean(value.cpu_break_on_exception))||!put(L"CPU",L"PCBreakpointEnabled",boolean(value.cpu_pc_breakpoint_enabled))||!put(L"CPU",L"PCBreakpointAddress",number(value.cpu_pc_breakpoint_address))||!put(L"CPU",L"DiagnosticLogging",boolean(value.cpu_diagnostic_logging))||
	   !put(L"Hardware",L"LimitSpeed",boolean(value.limit_speed))||!put(L"Hardware",L"Muted",boolean(value.muted))||!put(L"Hardware",L"BiosPath",value.bios_path)||
	   !put(L"Keyboard",L"Wasd",boolean(value.wasd_profile))||!put(L"Keyboard",L"NeutralOpposites",boolean(value.neutral_opposites))||
	   !put(L"Window",L"Width",number(value.window_width))||!put(L"Window",L"Height",number(value.window_height))||!put(L"Window",L"Maximized",boolean(value.window_maximized))||!put(L"Window",L"Fullscreen",boolean(value.fullscreen)))return false;
	for(unsigned i=0;i<value.keyboard_keys.size();i++)if(!put(L"Keyboard",(L"Key"+std::to_wstring(i)).c_str(),number(value.keyboard_keys[i])))return false;
	if(!WritePrivateProfileStringW(nullptr,nullptr,nullptr,temporary.c_str())){
		const DWORD code=GetLastError();
		if(!file_exists(temporary)){error=win_error(L"Flush settings profile",code);return false;}
	}
	HANDLE file=CreateFileW(temporary.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
	if(file==INVALID_HANDLE_VALUE){error=win_error(L"Open settings temporary for flush",GetLastError());return false;}
	const BOOL flushed=FlushFileBuffers(file);const DWORD flush_error=flushed?ERROR_SUCCESS:GetLastError();CloseHandle(file);
	if(!flushed){error=win_error(L"FlushFileBuffers settings",flush_error);return false;}
	if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){error=win_error(L"Atomically replace settings",GetLastError());return false;}
	return true;
}

void apply_palette(Theme theme,unsigned accent){
	static const COLORREF accents[]={RGB(169,157,255),RGB(86,156,255),RGB(58,190,132),RGB(247,153,80)};
	ps1ui::accent=accents[accent<4?accent:0];
	if(theme==Theme::Light){ps1ui::background=RGB(245,246,250);ps1ui::panel=RGB(255,255,255);ps1ui::border=RGB(211,214,224);ps1ui::text=RGB(29,31,42);ps1ui::muted=RGB(94,99,115);}
	else if(theme==Theme::Amoled){ps1ui::background=RGB(0,0,0);ps1ui::panel=RGB(12,12,14);ps1ui::border=RGB(44,44,48);ps1ui::text=RGB(245,245,248);ps1ui::muted=RGB(157,157,166);}
	else{ps1ui::background=RGB(15,16,24);ps1ui::panel=RGB(24,25,38);ps1ui::border=RGB(48,49,68);ps1ui::text=RGB(239,239,249);ps1ui::muted=RGB(162,164,187);}
}

HWND open_settings_window(HWND owner,Preferences& value,const std::filesystem::path& path,SettingsActions actions,unsigned font_size){
	if(current_window&&IsWindow(current_window)){SetForegroundWindow(current_window);return current_window;}
	static bool registered=false;
	HINSTANCE instance=GetModuleHandleW(nullptr);
	if(!registered){WNDCLASSW window_class{};window_class.lpfnWndProc=settings_proc;window_class.hInstance=instance;window_class.hCursor=LoadCursorW(nullptr,IDC_ARROW);window_class.hbrBackground=nullptr;window_class.lpszClassName=L"AVOPSEMUSettingsWindow";if(!RegisterClassW(&window_class)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return nullptr;registered=true;}
	auto* state=new WindowState;state->owner=owner;state->value=&value;state->path=path;state->actions=std::move(actions);state->font_size=std::clamp(font_size,12u,24u);pending_state=state;
	HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME,L"AVOPSEMUSettingsWindow",L"AVOPSEMU — Hardware and UI Settings",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,width,height,owner,nullptr,instance,state);
	if(!window){if(pending_state==state){pending_state=nullptr;delete state;}return nullptr;}
	pending_state=nullptr;
	current_window=window;ShowWindow(window,SW_SHOW);UpdateWindow(window);return window;
}

void refresh_settings_window(HWND window,unsigned font_size){
	if(!window||!IsWindow(window))return;
	auto* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(!state)return;
	rebuild_brushes(*state);const unsigned adjusted=std::clamp(font_size,12u,24u);if(state->font_size!=adjusted){state->font_size=adjusted;rebuild_fonts(*state);}if(state->categories_list)SendMessageW(state->categories_list,WM_SETFONT,reinterpret_cast<WPARAM>(state->font),TRUE);build_page(*state);InvalidateRect(window,nullptr,TRUE);
}

}