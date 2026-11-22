#include "frontend_library.hpp"
#include "ui_theme.hpp"
#include <commdlg.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace avopsui {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint64_t maximum_library_bytes=32ull*1024*1024;
constexpr unsigned maximum_games=4096;
constexpr int search_id=2001,sort_id=2002,list_id=2003,mount_id=2004,boot_id=2005,import_id=2006,remove_id=2007,cover_id=2008;
constexpr int overrides_id=2010,limit_id=2011,mute_id=2012,cover_draw_id=2013;
constexpr int window_width=1080,window_height=760;
constexpr const wchar_t* window_class_name=L"AVOPSEMUGameLibraryWindow";

uint64_t utc_seconds(){
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::wstring windows_error(const wchar_t* operation,DWORD code){
	return std::wstring(operation)+L" failed (Windows error "+std::to_wstring(code)+L").";
}

bool same_path(const std::filesystem::path& left,const std::filesystem::path& right){
	const auto a=left.wstring(),b=right.wstring();
	return CompareStringOrdinal(a.c_str(),static_cast<int>(a.size()),b.c_str(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;
}

std::filesystem::path absolute_path(const std::filesystem::path& path,std::wstring& error){
	std::error_code filesystem_error;
	auto result=std::filesystem::absolute(path,filesystem_error);
	if(filesystem_error){const auto text=filesystem_error.message();error=L"Could not resolve game path: "+std::wstring(text.begin(),text.end());return {};}
	return result.lexically_normal();
}

bool utf8_from_wide(const std::wstring& value,std::string& result){
	if(value.empty()){result.clear();return true;}
	const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
	if(size<=0)return false;
	result.resize(static_cast<size_t>(size));
	return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr)==size;
}

bool wide_from_utf8(const std::string& value,std::wstring& result){
	if(value.empty()){result.clear();return true;}
	const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
	if(size<=0)return false;
	result.resize(static_cast<size_t>(size));
	return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size)==size;
}

std::string encode_field(const std::wstring& value){
	std::string utf8;
	if(!utf8_from_wide(value,utf8))throw std::runtime_error("A library field contains invalid UTF-16.");
	static constexpr char hex[]="0123456789ABCDEF";
	std::string encoded;encoded.reserve(utf8.size());
	for(unsigned char byte:utf8){
		if((byte>='a'&&byte<='z')||(byte>='A'&&byte<='Z')||(byte>='0'&&byte<='9')||byte=='-'||byte=='_'||byte=='.'||byte=='~')encoded.push_back(static_cast<char>(byte));
		else{encoded.push_back('%');encoded.push_back(hex[byte>>4]);encoded.push_back(hex[byte&15]);}
	}
	return encoded;
}

unsigned hex_value(char value){
	if(value>='0'&&value<='9')return static_cast<unsigned>(value-'0');
	if(value>='A'&&value<='F')return static_cast<unsigned>(value-'A'+10);
	if(value>='a'&&value<='f')return static_cast<unsigned>(value-'a'+10);
	return 16;
}

std::wstring decode_field(const std::string& value){
	std::string utf8;utf8.reserve(value.size());
	for(size_t i=0;i<value.size();i++){
		if(value[i]!='%'){utf8.push_back(value[i]);continue;}
		if(i+2>=value.size())throw std::runtime_error("Truncated percent escape in game library.");
		const unsigned high=hex_value(value[i+1]),low=hex_value(value[i+2]);
		if(high>15||low>15)throw std::runtime_error("Invalid percent escape in game library.");
		utf8.push_back(static_cast<char>((high<<4)|low));i+=2;
	}
	std::wstring result;if(!wide_from_utf8(utf8,result))throw std::runtime_error("Game library contains invalid UTF-8.");
	return result;
}

std::vector<std::string> split_fields(const std::string& line){
	std::vector<std::string> fields;size_t first=0;
	for(;;){const size_t end=line.find('\t',first);fields.push_back(line.substr(first,end==std::string::npos?end:end-first));if(end==std::string::npos)break;first=end+1;}
	return fields;
}

uint64_t parse_number(const std::string& value,uint64_t maximum){
	uint64_t result=0;
	const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result,10);
	if(value.empty()||parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||result>maximum)throw std::runtime_error("Invalid numeric value in game library.");
	return result;
}

bool write_atomic(const std::filesystem::path& path,const std::string& contents,std::wstring& error){
	if(contents.size()>maximum_library_bytes){error=L"Game library exceeded the 32 MiB safety limit.";return false;}
	std::error_code filesystem_error;
	if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path(),filesystem_error);
	if(filesystem_error){const auto text=filesystem_error.message();error=L"Could not create game-library directory: "+std::wstring(text.begin(),text.end());return false;}
	auto temporary=path;temporary+=L".tmp";
	HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
	if(file==INVALID_HANDLE_VALUE){error=windows_error(L"Create game-library temporary",GetLastError());return false;}
	DWORD written=0;const DWORD requested=static_cast<DWORD>(contents.size());
	const BOOL write_ok=WriteFile(file,contents.data(),requested,&written,nullptr);
	const DWORD write_error=write_ok?ERROR_SUCCESS:GetLastError();
	if(!write_ok||written!=requested){CloseHandle(file);DeleteFileW(temporary.c_str());error=windows_error(L"Write game library",write_ok?ERROR_WRITE_FAULT:write_error);return false;}
	if(!FlushFileBuffers(file)){const DWORD code=GetLastError();CloseHandle(file);DeleteFileW(temporary.c_str());error=windows_error(L"Flush game library",code);return false;}
	if(!CloseHandle(file)){const DWORD code=GetLastError();DeleteFileW(temporary.c_str());error=windows_error(L"Close game library",code);return false;}
	if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){const DWORD code=GetLastError();DeleteFileW(temporary.c_str());error=windows_error(L"Replace game library",code);return false;}
	return true;
}

std::wstring region_title(unsigned region){
	switch(region){case 1:return L"USA (SYSTEM.CNF metadata)";case 2:return L"Europe (SYSTEM.CNF metadata)";case 3:return L"Japan (SYSTEM.CNF metadata)";default:return L"Unknown / unlicensed";}
}

std::wstring format_duration(uint64_t seconds){
	const uint64_t hours=seconds/3600,minutes=(seconds/60)%60,remainder=seconds%60;
	std::wostringstream value;value<<hours<<L":"<<std::setfill(L'0')<<std::setw(2)<<minutes<<L":"<<std::setw(2)<<remainder;return value.str();
}

std::wstring format_date(uint64_t seconds){
	if(!seconds)return L"Never";
	const std::time_t timestamp=static_cast<std::time_t>(seconds);std::tm local{};
	if(localtime_s(&local,&timestamp)!=0)return L"Unknown";
	std::wostringstream value;value<<std::setfill(L'0')<<std::setw(4)<<local.tm_year+1900<<L"-"<<std::setw(2)<<local.tm_mon+1<<L"-"<<std::setw(2)<<local.tm_mday<<L" "<<std::setw(2)<<local.tm_hour<<L":"<<std::setw(2)<<local.tm_min;return value.str();
}

std::wstring lower(std::wstring text){for(auto& ch:text)ch=static_cast<wchar_t>(std::towlower(ch));return text;}

struct WindowState {
	HWND window=nullptr,search=nullptr,sort=nullptr,list=nullptr,title=nullptr,path=nullptr,metadata=nullptr,playtime=nullptr,state_label=nullptr,cover=nullptr;
	HWND import=nullptr,mount=nullptr,boot=nullptr,remove=nullptr,choose_cover=nullptr,overrides=nullptr,limit=nullptr,mute=nullptr;
	GameLibrary* library=nullptr;
	LibraryActions actions;
	std::vector<std::filesystem::path> visible_paths;
	std::filesystem::path selected_path;
	HFONT font=nullptr,title_font=nullptr;
	HBRUSH background=nullptr,panel=nullptr;
	HBITMAP cover_bitmap=nullptr;
	unsigned font_size=16;
};

HWND current_window=nullptr;
WindowState* pending_state=nullptr;
unsigned configured_font_size=16;

void rebuild_fonts(WindowState& state){
	if(state.font)DeleteObject(state.font);if(state.title_font)DeleteObject(state.title_font);
	state.font=CreateFontW(-static_cast<int>(state.font_size),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
	state.title_font=CreateFontW(-static_cast<int>(state.font_size*3/2),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
}

void layout_window(WindowState& state){
	RECT client{};GetClientRect(state.window,&client);const int width=client.right,height=client.bottom;
	const int left_width=std::max(340,width*42/100),right_x=left_width+32,right_width=std::max(400,width-right_x-20);
	MoveWindow(state.search,20,68,left_width-180,30,TRUE);MoveWindow(state.sort,left_width-150,68,130,180,TRUE);
	MoveWindow(state.import,right_x+20,68,160,30,TRUE);
	MoveWindow(state.list,20,108,left_width,height-162,TRUE);
	MoveWindow(state.title,right_x+20,112,right_width-40,46,TRUE);
	MoveWindow(state.cover,right_x+20,164,190,250,TRUE);
	MoveWindow(state.path,right_x+230,164,right_width-250,58,TRUE);
	MoveWindow(state.metadata,right_x+230,226,right_width-250,86,TRUE);
	MoveWindow(state.playtime,right_x+230,320,right_width-250,40,TRUE);
	MoveWindow(state.state_label,right_x+230,360,right_width-250,40,TRUE);
	const int button_gap=8,button_width=(right_width-40-3*button_gap)/4;
	MoveWindow(state.mount,right_x+20,430,button_width,34,TRUE);MoveWindow(state.boot,right_x+20+button_width+button_gap,430,button_width,34,TRUE);
	MoveWindow(state.choose_cover,right_x+20+2*(button_width+button_gap),430,button_width,34,TRUE);MoveWindow(state.remove,right_x+20+3*(button_width+button_gap),430,button_width,34,TRUE);
	MoveWindow(state.overrides,right_x+20,484,right_width-40,28,TRUE);
	MoveWindow(state.limit,right_x+20,520,right_width-40,28,TRUE);MoveWindow(state.mute,right_x+20,552,right_width-40,28,TRUE);
}

void update_cover(WindowState& state){
	if(state.cover_bitmap){DeleteObject(state.cover_bitmap);state.cover_bitmap=nullptr;}
	const auto* entry=state.library?state.library->find(state.selected_path):nullptr;
	if(!entry||entry->cover_path.empty())return;
	HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
	const bool uninitialize=SUCCEEDED(initialized);
	if(FAILED(initialized)&&initialized!=RPC_E_CHANGED_MODE)return;
	ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
	HRESULT result=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf()));
	if(SUCCEEDED(result))result=factory->CreateDecoderFromFilename(entry->cover_path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,decoder.GetAddressOf());
	if(SUCCEEDED(result))result=decoder->GetFrame(0,frame.GetAddressOf());
	UINT width=0,height=0;
	if(SUCCEEDED(result))result=frame->GetSize(&width,&height);
	if(SUCCEEDED(result)&&(width==0||height==0||width>4096||height>4096||uint64_t(width)*height>16ull*1024*1024))result=E_INVALIDARG;
	if(SUCCEEDED(result))result=factory->CreateFormatConverter(converter.GetAddressOf());
	if(SUCCEEDED(result))result=converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
	if(SUCCEEDED(result)){
		const UINT stride=width*4,bytes=stride*height;std::vector<uint8_t> pixels(bytes);
		result=converter->CopyPixels(nullptr,stride,bytes,pixels.data());
		if(SUCCEEDED(result)){
			BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(width);info.bmiHeader.biHeight=-static_cast<LONG>(height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
			void* destination=nullptr;state.cover_bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&destination,nullptr,0);
			if(!state.cover_bitmap)result=HRESULT_FROM_WIN32(GetLastError());
			else std::memcpy(destination,pixels.data(),bytes);
		}
	}
	converter.Reset();frame.Reset();decoder.Reset();factory.Reset();
	if(uninitialize)CoUninitialize();
	if(FAILED(result)&&state.cover_bitmap){DeleteObject(state.cover_bitmap);state.cover_bitmap=nullptr;}
}

void update_details(WindowState& state){
	const LRESULT selected=SendMessageW(state.list,LB_GETCURSEL,0,0);
	if(selected<0||static_cast<size_t>(selected)>=state.visible_paths.size()){
		state.selected_path.clear();SetWindowTextW(state.title,L"Select a game");SetWindowTextW(state.path,L"");SetWindowTextW(state.metadata,L"");SetWindowTextW(state.playtime,L"");SetWindowTextW(state.state_label,L"");
		for(HWND control:{state.mount,state.boot,state.remove,state.choose_cover,state.overrides,state.limit,state.mute})EnableWindow(control,FALSE);
		if(state.cover_bitmap){DeleteObject(state.cover_bitmap);state.cover_bitmap=nullptr;}InvalidateRect(state.cover,nullptr,TRUE);return;
	}
	state.selected_path=state.visible_paths[static_cast<size_t>(selected)];
	const auto* entry=state.library->find(state.selected_path);if(!entry)return;
	SetWindowTextW(state.title,entry->title.c_str());const auto path_text=entry->path.wstring();SetWindowTextW(state.path,path_text.c_str());
	const std::wstring code=entry->serial.empty()?L"Not detected":entry->serial;
	const std::wstring region=entry->region.empty()?L"Unknown":entry->region;
	const std::wstring meta=L"Game code: "+code+L"\r\nRelease region: "+region+L"\r\nLast played: "+format_date(entry->last_played_utc);
	SetWindowTextW(state.metadata,meta.c_str());SetWindowTextW(state.playtime,(L"Total playtime: "+format_duration(state.library->playtime_seconds(*entry))).c_str());
	const auto play_state=state.library->state(entry->path);const wchar_t* state_text=play_state==GamePlayState::Running?L"State: Running":play_state==GamePlayState::Paused?L"State: Paused":L"State: Stopped";SetWindowTextW(state.state_label,state_text);
	SendMessageW(state.overrides,BM_SETCHECK,entry->has_host_overrides?BST_CHECKED:BST_UNCHECKED,0);
	SendMessageW(state.limit,BM_SETCHECK,entry->limit_speed?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(state.mute,BM_SETCHECK,entry->muted?BST_CHECKED:BST_UNCHECKED,0);
	EnableWindow(state.mount,TRUE);EnableWindow(state.boot,TRUE);EnableWindow(state.remove,TRUE);EnableWindow(state.choose_cover,TRUE);EnableWindow(state.overrides,TRUE);
	EnableWindow(state.limit,entry->has_host_overrides);EnableWindow(state.mute,entry->has_host_overrides);
	update_cover(state);InvalidateRect(state.cover,nullptr,TRUE);
}

void rebuild_list(WindowState& state){
	std::wstring query(32768,L'\0');const int length=GetWindowTextW(state.search,query.data(),static_cast<int>(query.size()));query.resize(length);query=lower(std::move(query));
	std::vector<const GameEntry*> matches;
	for(const auto& entry:state.library->entries()){
		const auto searchable=lower(entry.title+L" "+entry.serial+L" "+entry.path.wstring());
		if(query.empty()||searchable.find(query)!=std::wstring::npos)matches.push_back(&entry);
	}
	const int sort=static_cast<int>(SendMessageW(state.sort,CB_GETCURSEL,0,0));
	if(sort==1)std::stable_sort(matches.begin(),matches.end(),[](const auto* a,const auto* b){return a->last_played_utc>b->last_played_utc;});
	else if(sort==2)std::stable_sort(matches.begin(),matches.end(),[&state](const auto* a,const auto* b){return state.library->playtime_seconds(*a)>state.library->playtime_seconds(*b);});
	else std::stable_sort(matches.begin(),matches.end(),[](const auto* a,const auto* b){return _wcsicmp(a->title.c_str(),b->title.c_str())<0;});
	const auto previous=state.selected_path;state.visible_paths.clear();SendMessageW(state.list,LB_RESETCONTENT,0,0);
	for(const auto* entry:matches){const std::wstring line=entry->title+L"   "+format_duration(state.library->playtime_seconds(*entry));SendMessageW(state.list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(line.c_str()));state.visible_paths.push_back(entry->path);}
	LRESULT selection=LB_ERR;
	for(size_t i=0;i<state.visible_paths.size();i++)if(!previous.empty()&&same_path(state.visible_paths[i],previous)){selection=static_cast<LRESULT>(i);break;}
	if(selection==LB_ERR&&!state.visible_paths.empty())selection=0;
	if(selection!=LB_ERR)SendMessageW(state.list,LB_SETCURSEL,selection,0);
	update_details(state);
}

void refresh_controls(WindowState& state){
	if(!state.window)return;
		for(HWND control:{state.search,state.sort,state.list,state.title,state.path,state.metadata,state.playtime,state.state_label,state.import,state.mount,state.boot,state.remove,state.choose_cover,state.overrides,state.limit,state.mute})if(control)SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(state.font),TRUE);
	SendMessageW(state.title,WM_SETFONT,reinterpret_cast<WPARAM>(state.title_font),TRUE);
	rebuild_list(state);layout_window(state);InvalidateRect(state.window,nullptr,TRUE);
}

bool choose_game_file(HWND owner,std::filesystem::path& path){
	wchar_t file[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=owner;dialog.lpstrFile=file;dialog.nMaxFile=static_cast<DWORD>(sizeof(file)/sizeof(file[0]));dialog.lpstrFilter=L"PS1 discs (*.cue;*.bin;*.iso)\0*.cue;*.bin;*.iso\0All files\0*.*\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
	if(!GetOpenFileNameW(&dialog))return false;
	path=file;return true;
}

void show_error(HWND owner,const std::wstring& error,const wchar_t* title){
	MessageBoxW(owner,error.c_str(),title,MB_OK|MB_ICONERROR);
}

void set_override(WindowState& state,int changed){
	if(state.selected_path.empty())return;
	const bool enabled=SendMessageW(state.overrides,BM_GETCHECK,0,0)==BST_CHECKED;
	const bool limit=SendMessageW(state.limit,BM_GETCHECK,0,0)==BST_CHECKED;
	const bool muted=SendMessageW(state.mute,BM_GETCHECK,0,0)==BST_CHECKED;
	std::wstring error;
	if(!state.library->set_host_overrides(state.selected_path,enabled,limit,muted,error)){show_error(state.window,error,L"Game profile could not be saved");return;}
	if(!state.library->save(error)){show_error(state.window,error,L"Game library could not be saved");return;}
	EnableWindow(state.limit,enabled);EnableWindow(state.mute,enabled);
	if(state.actions.profile_changed&&changed!=overrides_id)state.actions.profile_changed(state.selected_path);
	update_details(state);
}

LRESULT CALLBACK library_proc(HWND window,UINT message,WPARAM wp,LPARAM lp){
	WindowState* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
	if(message==WM_NCCREATE){auto* create=reinterpret_cast<CREATESTRUCTW*>(lp);state=static_cast<WindowState*>(create->lpCreateParams);state->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}
	if(!state)return DefWindowProcW(window,message,wp,lp);
	switch(message){
	case WM_CREATE:{
		state->background=CreateSolidBrush(ps1ui::background);state->panel=CreateSolidBrush(ps1ui::panel);
		state->font_size=configured_font_size;rebuild_fonts(*state);
		state->search=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(search_id)),GetModuleHandleW(nullptr),nullptr);
		state->sort=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(sort_id)),GetModuleHandleW(nullptr),nullptr);
		state->list=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(list_id)),GetModuleHandleW(nullptr),nullptr);
		state->title=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
		state->path=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
		state->metadata=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
		state->playtime=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
		state->state_label=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
		state->cover=CreateWindowExW(WS_EX_CLIENTEDGE,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(cover_draw_id)),GetModuleHandleW(nullptr),nullptr);
		state->import=CreateWindowW(L"BUTTON",L"Import disc image...",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(import_id)),GetModuleHandleW(nullptr),nullptr);
		state->mount=CreateWindowW(L"BUTTON",L"Mount disc",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(mount_id)),GetModuleHandleW(nullptr),nullptr);
		state->boot=CreateWindowW(L"BUTTON",L"Boot / run",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(boot_id)),GetModuleHandleW(nullptr),nullptr);
		state->choose_cover=CreateWindowW(L"BUTTON",L"Choose cover...",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(cover_id)),GetModuleHandleW(nullptr),nullptr);
		state->remove=CreateWindowW(L"BUTTON",L"Remove",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(remove_id)),GetModuleHandleW(nullptr),nullptr);
		state->overrides=CreateWindowW(L"BUTTON",L"Use per-game host settings when booting from this library",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(overrides_id)),GetModuleHandleW(nullptr),nullptr);
		state->limit=CreateWindowW(L"BUTTON",L"Limit execution to modeled 1x speed",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(limit_id)),GetModuleHandleW(nullptr),nullptr);
		state->mute=CreateWindowW(L"BUTTON",L"Mute Windows audio output",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(mute_id)),GetModuleHandleW(nullptr),nullptr);
		if(!state->background||!state->panel||!state->font||!state->title_font||!state->search||!state->sort||!state->list||!state->title||!state->path||!state->metadata||!state->playtime||!state->state_label||!state->cover||!state->import||!state->mount||!state->boot||!state->choose_cover||!state->remove||!state->overrides||!state->limit||!state->mute)return -1;
		SendMessageW(state->sort,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Name"));SendMessageW(state->sort,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Last played"));SendMessageW(state->sort,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Playtime"));SendMessageW(state->sort,CB_SETCURSEL,0,0);
		SetWindowTextW(state->search,L"");refresh_controls(*state);return 0;}
	case WM_PAINT:{PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);RECT area{};GetClientRect(window,&area);FillRect(dc,&area,state->background);const int client_width=static_cast<int>(area.right),client_height=static_cast<int>(area.bottom);ps1ui::label(dc,L"Game Library",RECT{20,16,450,54},state->title_font,ps1ui::text);ps1ui::label(dc,L"Imported disc images · play sessions are counted only while the Runner is active",RECT{450,24,client_width-20,48},state->font,ps1ui::muted,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);ps1ui::label(dc,L"Search",RECT{20,48,120,68},state->font,ps1ui::muted);ps1ui::label(dc,L"Sort",RECT{client_width*42/100-150,48,client_width*42/100-40,68},state->font,ps1ui::muted);const int left=std::max(340,client_width*42/100);ps1ui::box(dc,RECT{12,102,left+8,client_height-42},ps1ui::panel,ps1ui::border,8);ps1ui::box(dc,RECT{left+20,102,client_width-12,client_height-42},ps1ui::panel,ps1ui::border,8);ps1ui::label(dc,L"Cover art: PNG, JPEG, or BMP. Game code and region are detected from SYSTEM.CNF when available.",RECT{left+40,client_height-34,client_width-20,client_height-12},state->font,ps1ui::muted);EndPaint(window,&paint);return 0;}
	case WM_ERASEBKGND:return 1;
	case WM_CTLCOLORSTATIC:case WM_CTLCOLORLISTBOX:{HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,ps1ui::text);SetBkColor(dc,ps1ui::panel);return reinterpret_cast<LRESULT>(state->panel);}
	case WM_CTLCOLORBTN:{HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,ps1ui::text);SetBkColor(dc,ps1ui::panel);return reinterpret_cast<LRESULT>(state->panel);}
	case WM_CTLCOLOREDIT:{HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,ps1ui::text);SetBkColor(dc,ps1ui::panel);return reinterpret_cast<LRESULT>(state->panel);}
	case WM_DRAWITEM:{
		auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
		if(item->CtlID==cover_draw_id){FillRect(item->hDC,&item->rcItem,state->panel);if(state->cover_bitmap){BITMAP image{};if(GetObjectW(state->cover_bitmap,sizeof(image),&image)){HDC source=CreateCompatibleDC(item->hDC);if(source){auto old=SelectObject(source,state->cover_bitmap);SetStretchBltMode(item->hDC,HALFTONE);StretchBlt(item->hDC,item->rcItem.left+4,item->rcItem.top+4,item->rcItem.right-item->rcItem.left-8,item->rcItem.bottom-item->rcItem.top-8,source,0,0,image.bmWidth,image.bmHeight,SRCCOPY);SelectObject(source,old);DeleteDC(source);}}}else ps1ui::label(item->hDC,L"No cover selected",item->rcItem,state->font,ps1ui::muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);return TRUE;}
		if(item->CtlType!=ODT_BUTTON)return FALSE;
		const bool primary=item->CtlID==boot_id||item->CtlID==import_id,pressed=(item->itemState&ODS_SELECTED)!=0,disabled=(item->itemState&ODS_DISABLED)!=0;
		const COLORREF fill=disabled?RGB(35,36,47):primary?(pressed?RGB(118,106,220):ps1ui::accent):(pressed?RGB(47,45,66):ps1ui::panel);
		ps1ui::box(item->hDC,item->rcItem,fill,primary?ps1ui::accent:ps1ui::border,8);
		wchar_t text[128]{};GetWindowTextW(item->hwndItem,text,128);ps1ui::label(item->hDC,text,item->rcItem,state->font,disabled?ps1ui::muted:primary?ps1ui::background:ps1ui::text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
		if(item->itemState&ODS_FOCUS){RECT focus=item->rcItem;InflateRect(&focus,-4,-4);DrawFocusRect(item->hDC,&focus);}return TRUE;}
	case WM_COMMAND:{const int id=LOWORD(wp),code=HIWORD(wp);
		if(id==search_id&&code==EN_CHANGE){rebuild_list(*state);return 0;}
		if(id==sort_id&&code==CBN_SELCHANGE){rebuild_list(*state);return 0;}
		if(id==list_id&&code==LBN_SELCHANGE){update_details(*state);return 0;}
		if(code==BN_CLICKED&&(id==overrides_id||id==limit_id||id==mute_id)){set_override(*state,id);return 0;}
		if(code!=BN_CLICKED)return 0;
		if(id==import_id){std::filesystem::path path;if(!choose_game_file(window,path))return 0;std::wstring error;if(!state->library->import_game(path,error)||!state->library->save(error)){show_error(window,error,L"Could not import game");return 0;}rebuild_list(*state);return 0;}
		if(state->selected_path.empty())return 0;
		if(id==mount_id){if(state->actions.mount_disc)state->actions.mount_disc(state->selected_path);refresh_game_library_window(window);return 0;}
		if(id==boot_id){if(state->actions.boot_disc)state->actions.boot_disc(state->selected_path);refresh_game_library_window(window);return 0;}
		if(id==remove_id){if(MessageBoxW(window,L"Remove this entry from the library? The disc image will not be deleted.",L"Remove game",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;std::wstring error;if(!state->library->remove_game(state->selected_path,error)||!state->library->save(error)){show_error(window,error,L"Could not remove game");return 0;}state->selected_path.clear();rebuild_list(*state);return 0;}
		if(id==cover_id){wchar_t file[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrFile=file;dialog.nMaxFile=static_cast<DWORD>(sizeof(file)/sizeof(file[0]));dialog.lpstrFilter=L"Image files (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All files\0*.*\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(!GetOpenFileNameW(&dialog))return 0;const auto* entry=state->library->find(state->selected_path);if(!entry)return 0;const auto old_cover=entry->cover_path;std::wstring error;if(!state->library->set_cover(state->selected_path,std::filesystem::path(file),error)){show_error(window,error,L"Invalid cover image");return 0;}update_cover(*state);if(!state->cover_bitmap){state->library->set_cover(state->selected_path,old_cover,error);update_cover(*state);show_error(window,L"The selected cover image could not be decoded by Windows Imaging Component.",L"Invalid cover image");return 0;}if(!state->library->save(error)){state->library->set_cover(state->selected_path,old_cover,error);update_cover(*state);show_error(window,error,L"Could not save cover path");return 0;}update_details(*state);return 0;}
		return 0;}
	case WM_SIZE:layout_window(*state);return 0;
	case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize={980,700};return 0;}
	case WM_CLOSE:DestroyWindow(window);return 0;
	case WM_NCDESTROY:
		if(current_window==window)current_window=nullptr;if(pending_state==state)pending_state=nullptr;
		if(state->cover_bitmap)DeleteObject(state->cover_bitmap);if(state->font)DeleteObject(state->font);if(state->title_font)DeleteObject(state->title_font);if(state->background)DeleteObject(state->background);if(state->panel)DeleteObject(state->panel);
		SetWindowLongPtrW(window,GWLP_USERDATA,0);delete state;return DefWindowProcW(window,message,wp,lp);
	}
	return DefWindowProcW(window,message,wp,lp);
}

}

void set_game_library_font_size(unsigned font_size){
	configured_font_size=std::clamp(font_size,12u,24u);
	if(!current_window||!IsWindow(current_window))return;auto* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(current_window,GWLP_USERDATA));if(!state)return;
	if(state->font_size!=configured_font_size){state->font_size=configured_font_size;rebuild_fonts(*state);refresh_controls(*state);}
}

GameLibrary::GameLibrary(std::filesystem::path database_path):database_path_(std::move(database_path)){}

bool GameLibrary::load(std::wstring& error){
	error.clear();const DWORD attributes=GetFileAttributesW(database_path_.c_str());
	if(attributes==INVALID_FILE_ATTRIBUTES){const DWORD code=GetLastError();if(code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND){entries_.clear();active_path_.clear();state_=GamePlayState::Stopped;return true;}error=windows_error(L"Inspect game library",code);return false;}
	if(attributes&FILE_ATTRIBUTE_DIRECTORY){error=L"Game-library path is a directory.";return false;}
	std::error_code filesystem_error;const auto size=std::filesystem::file_size(database_path_,filesystem_error);
	if(filesystem_error||size>maximum_library_bytes){error=filesystem_error?L"Could not inspect game-library size.":L"Game library exceeds the 32 MiB safety limit.";return false;}
	std::ifstream input(database_path_,std::ios::binary);if(!input){error=L"Could not open game library for reading.";return false;}
	std::string contents(static_cast<size_t>(size),'\0');if(size&&!input.read(contents.data(),static_cast<std::streamsize>(size))){error=L"Could not read the complete game library.";return false;}
	std::istringstream lines(contents);std::string line;if(!std::getline(lines,line)){error=L"Game library is empty.";return false;}if(!line.empty()&&line.back()=='\r')line.pop_back();
	if(line!="AVOPSEMU_GAME_LIBRARY\t1"){error=L"Unsupported game-library schema.";return false;}
	std::vector<GameEntry> loaded;GameEntry current;bool in_game=false;unsigned line_number=1;uint64_t expected_sessions=0;
	try{
		while(std::getline(lines,line)){
			++line_number;if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;
			const auto fields=split_fields(line);
			if(fields[0]=="GAME"){
				if(in_game||fields.size()!=12||loaded.size()>=maximum_games)throw std::runtime_error("Malformed or excessive GAME record.");
				current=GameEntry{};current.path=decode_field(fields[1]);current.title=decode_field(fields[2]);current.serial=decode_field(fields[3]);current.region=decode_field(fields[4]);current.cover_path=decode_field(fields[5]);
				current.play_seconds=parse_number(fields[6],std::numeric_limits<uint64_t>::max());current.last_played_utc=parse_number(fields[7],std::numeric_limits<uint64_t>::max());
				const auto overrides=parse_number(fields[8],1),limit=parse_number(fields[9],1),muted=parse_number(fields[10],1);
				current.has_host_overrides=overrides!=0;current.limit_speed=limit!=0;current.muted=muted!=0;
				if(!current.path.is_absolute()||current.title.empty())throw std::runtime_error("GAME record has an invalid path or empty title.");
				expected_sessions=parse_number(fields[11],100000);current.sessions.reserve(static_cast<size_t>(expected_sessions));in_game=true;
			}else if(fields[0]=="SESSION"){
				if(!in_game||fields.size()!=3||current.sessions.size()>=expected_sessions)throw std::runtime_error("Malformed SESSION record.");
				current.sessions.push_back({parse_number(fields[1],std::numeric_limits<uint64_t>::max()),parse_number(fields[2],std::numeric_limits<uint64_t>::max())});
			}else if(fields[0]=="ENDGAME"){
				if(!in_game||fields.size()!=1)throw std::runtime_error("Unexpected ENDGAME record.");
				if(current.sessions.size()!=expected_sessions)throw std::runtime_error("Game session count does not match its GAME record.");
				loaded.push_back(std::move(current));current=GameEntry{};in_game=false;
			}else throw std::runtime_error("Unknown record in game library.");
		}
		if(in_game)throw std::runtime_error("Missing ENDGAME record.");
		if(loaded.size()>maximum_games)throw std::runtime_error("Game library has too many entries.");
		for(size_t i=0;i<loaded.size();i++)for(size_t j=0;j<i;j++)if(same_path(loaded[i].path,loaded[j].path))throw std::runtime_error("Duplicate path in game library.");
	}catch(const std::exception& parse_error){error=L"Game library line "+std::to_wstring(line_number)+L": "+std::wstring(parse_error.what(),parse_error.what()+std::strlen(parse_error.what()));return false;}
	entries_=std::move(loaded);active_path_.clear();state_=GamePlayState::Stopped;return true;
}

bool GameLibrary::save(std::wstring& error) const{
	error.clear();std::ostringstream output;output<<"AVOPSEMU_GAME_LIBRARY\t1\n";
	try{
		for(const auto& source:entries_){
			GameEntry entry=source;uint64_t elapsed=0;
			if(state_==GamePlayState::Running&&same_path(active_path_,source.path)){
				const auto duration=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-active_since_).count();if(duration>0)elapsed=static_cast<uint64_t>(duration);
			}
			if(elapsed>std::numeric_limits<uint64_t>::max()-entry.play_seconds)entry.play_seconds=std::numeric_limits<uint64_t>::max();else entry.play_seconds+=elapsed;
			output<<"GAME\t"<<encode_field(entry.path.wstring())<<'\t'<<encode_field(entry.title)<<'\t'<<encode_field(entry.serial)<<'\t'<<encode_field(entry.region)<<'\t'<<encode_field(entry.cover_path)<<'\t'<<entry.play_seconds<<'\t'<<entry.last_played_utc<<'\t'<<entry.has_host_overrides<<'\t'<<entry.limit_speed<<'\t'<<entry.muted<<'\t'<<entry.sessions.size()+(elapsed?1:0)<<'\n';
			for(const auto& session:entry.sessions)output<<"SESSION\t"<<session.started_utc<<'\t'<<session.duration_seconds<<'\n';
			if(elapsed)output<<"SESSION\t"<<active_started_utc_<<'\t'<<elapsed<<'\n';
			output<<"ENDGAME\n";
		}
	}catch(const std::exception& serialization_error){error=L"Could not serialize game library: "+std::wstring(serialization_error.what(),serialization_error.what()+std::strlen(serialization_error.what()));return false;}
	return write_atomic(database_path_,output.str(),error);
}

GameEntry* GameLibrary::find_mutable(const std::filesystem::path& path){
	for(auto& entry:entries_)if(same_path(entry.path,path))return &entry;return nullptr;
}

const GameEntry* GameLibrary::find(const std::filesystem::path& path) const{
	for(const auto& entry:entries_)if(same_path(entry.path,path))return &entry;return nullptr;
}

bool GameLibrary::import_game(const std::filesystem::path& path,std::wstring& error){
	error.clear();const auto absolute=absolute_path(path,error);if(absolute.empty())return false;
	const DWORD attributes=GetFileAttributesW(absolute.c_str());if(attributes==INVALID_FILE_ATTRIBUTES){error=windows_error(L"Inspect disc image",GetLastError());return false;}if(attributes&FILE_ATTRIBUTE_DIRECTORY){error=L"Selected game path is a directory.";return false;}
	auto extension=absolute.extension().wstring();std::transform(extension.begin(),extension.end(),extension.begin(),[](wchar_t ch){return static_cast<wchar_t>(std::towupper(ch));});
	if(extension!=L".CUE"&&extension!=L".BIN"&&extension!=L".ISO"){error=L"Game library accepts CUE, BIN, or ISO images only.";return false;}
	if(find(absolute))return true;
	if(entries_.size()>=maximum_games){error=L"Game library reached the 4096-entry limit.";return false;}
	GameEntry entry;entry.path=absolute;entry.title=absolute.stem().wstring();entry.region=L"Not detected";entries_.push_back(std::move(entry));return true;
}

bool GameLibrary::remove_game(const std::filesystem::path& path,std::wstring& error){
	error.clear();auto* entry=find_mutable(path);if(!entry){error=L"Selected game is no longer in the library.";return false;}
	if(state_==GamePlayState::Running&&same_path(active_path_,entry->path)){error=L"Pause or stop this game before removing it from the library.";return false;}
	const auto removed=entry->path;entries_.erase(std::remove_if(entries_.begin(),entries_.end(),[&](const GameEntry& value){return same_path(value.path,removed);}),entries_.end());
	if(same_path(active_path_,removed)){active_path_.clear();state_=GamePlayState::Stopped;}
	return true;
}

bool GameLibrary::update_disc_metadata(const std::filesystem::path& path,const std::string& boot_name,unsigned region,std::wstring& error){
	error.clear();auto absolute=absolute_path(path,error);if(absolute.empty())return false;auto* entry=find_mutable(absolute);if(!entry){if(!import_game(absolute,error))return false;entry=find_mutable(absolute);}if(!entry){error=L"Could not create game-library metadata record.";return false;}
	const size_t separator=boot_name.find_last_of("/\\:");std::string executable=boot_name.substr(separator==std::string::npos?0:separator+1);const size_t version=executable.find(';');if(version!=std::string::npos)executable.resize(version);
	std::transform(executable.begin(),executable.end(),executable.begin(),[](unsigned char ch){return static_cast<char>(std::toupper(ch));});
	if(executable.rfind("SLUS",0)==0||executable.rfind("SCUS",0)==0||executable.rfind("SLES",0)==0||executable.rfind("SCES",0)==0||executable.rfind("SLED",0)==0||executable.rfind("SCED",0)==0||executable.rfind("SLPS",0)==0||executable.rfind("SLPM",0)==0||executable.rfind("SCPS",0)==0){if(!wide_from_utf8(executable,entry->serial)){error=L"Could not decode game code from SYSTEM.CNF.";return false;}}
	else entry->serial.clear();
	entry->region=region_title(region);return true;
}

bool GameLibrary::set_cover(const std::filesystem::path& path,const std::filesystem::path& cover,std::wstring& error){
	error.clear();auto* entry=find_mutable(path);if(!entry){error=L"Selected game is no longer in the library.";return false;}
	if(cover.empty()){entry->cover_path.clear();return true;}
	const auto absolute=absolute_path(cover,error);if(absolute.empty())return false;const DWORD attributes=GetFileAttributesW(absolute.c_str());if(attributes==INVALID_FILE_ATTRIBUTES){error=windows_error(L"Inspect cover image",GetLastError());return false;}if(attributes&FILE_ATTRIBUTE_DIRECTORY){error=L"Cover path is a directory.";return false;}
	entry->cover_path=absolute.wstring();return true;
}

bool GameLibrary::set_host_overrides(const std::filesystem::path& path,bool enabled,bool limit_speed,bool muted,std::wstring& error){
	error.clear();auto* entry=find_mutable(path);if(!entry){error=L"Selected game is no longer in the library.";return false;}entry->has_host_overrides=enabled;entry->limit_speed=limit_speed;entry->muted=muted;return true;
}

void GameLibrary::finish_session(){
	if(state_!=GamePlayState::Running)return;
	const auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-active_since_).count();
	if(elapsed>0){if(auto* entry=find_mutable(active_path_)){const auto duration=static_cast<uint64_t>(elapsed);entry->play_seconds=duration>std::numeric_limits<uint64_t>::max()-entry->play_seconds?std::numeric_limits<uint64_t>::max():entry->play_seconds+duration;entry->sessions.push_back({active_started_utc_,duration});}}
	active_since_={};active_started_utc_=0;
}

void GameLibrary::begin_session(const std::filesystem::path& path){
	auto absolute=path;
	for(const auto& entry:entries_)if(same_path(entry.path,path)){absolute=entry.path;break;}
	if(state_==GamePlayState::Running&&same_path(active_path_,absolute))return;
	if(state_==GamePlayState::Running)finish_session();
	active_path_=absolute;active_since_=std::chrono::steady_clock::now();active_started_utc_=utc_seconds();state_=GamePlayState::Running;
	if(auto* entry=find_mutable(active_path_))entry->last_played_utc=active_started_utc_;
}

void GameLibrary::pause_session(){
	if(state_==GamePlayState::Running)finish_session();if(!active_path_.empty())state_=GamePlayState::Paused;
}

void GameLibrary::stop_session(){
	if(state_==GamePlayState::Running)finish_session();state_=GamePlayState::Stopped;
}

GamePlayState GameLibrary::state(const std::filesystem::path& path) const{
	return !active_path_.empty()&&same_path(active_path_,path)?state_:GamePlayState::Stopped;
}

uint64_t GameLibrary::playtime_seconds(const GameEntry& entry) const{
	if(state_!=GamePlayState::Running||!same_path(active_path_,entry.path))return entry.play_seconds;
	const auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-active_since_).count();
	const auto duration=elapsed>0?static_cast<uint64_t>(elapsed):0;return duration>std::numeric_limits<uint64_t>::max()-entry.play_seconds?std::numeric_limits<uint64_t>::max():entry.play_seconds+duration;
}

HWND open_game_library_window(HWND owner,GameLibrary& library,LibraryActions actions){
	if(current_window&&IsWindow(current_window)){SetForegroundWindow(current_window);return current_window;}
	static bool registered=false;const HINSTANCE instance=GetModuleHandleW(nullptr);
	if(!registered){WNDCLASSW window_class{};window_class.lpfnWndProc=library_proc;window_class.hInstance=instance;window_class.hCursor=LoadCursorW(nullptr,IDC_ARROW);window_class.hbrBackground=nullptr;window_class.lpszClassName=window_class_name;if(!RegisterClassW(&window_class)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return nullptr;registered=true;}
	auto* state=new WindowState;state->library=&library;state->actions=std::move(actions);pending_state=state;
	HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME,window_class_name,L"AVOPSEMU — Game Library",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,window_width,window_height,owner,nullptr,instance,state);
	if(!window){if(pending_state==state){pending_state=nullptr;delete state;}return nullptr;}
	pending_state=nullptr;current_window=window;ShowWindow(window,SW_SHOW);UpdateWindow(window);return window;
}

void refresh_game_library_window(HWND window){
	if(!window||!IsWindow(window))return;auto* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(state)refresh_controls(*state);
}

void refresh_game_library_window_live(HWND window){
	if(!window||!IsWindow(window))return;auto* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(!state||state->selected_path.empty())return;
	const auto* entry=state->library->find(state->selected_path);if(!entry)return;
	SetWindowTextW(state->playtime,(L"Total playtime: "+format_duration(state->library->playtime_seconds(*entry))).c_str());
	const auto play_state=state->library->state(entry->path);const wchar_t* text=play_state==GamePlayState::Running?L"State: Running":play_state==GamePlayState::Paused?L"State: Paused":L"State: Stopped";SetWindowTextW(state->state_label,text);
}

}