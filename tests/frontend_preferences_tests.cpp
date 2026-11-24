#include "frontend_settings.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
}

int main(){
	std::filesystem::path directory;
	try{
		directory=std::filesystem::temp_directory_path()/(L"avopsemu-frontend-settings-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
		std::filesystem::create_directories(directory);
		const auto path=directory/L"settings.ini";
		avopsui::Preferences source;source.theme=avopsui::Theme::Light;source.accent=2;source.font_size=20;source.show_status=false;source.muted=true;source.limit_speed=false;source.stretch_screen=true;source.fullscreen=true;source.window_width=1600;source.window_height=1000;source.bios_path=L"C:\\BIOS\\test.bin";source.keyboard_keys[14]='C';
		std::wstring error;if(!avopsui::write_preferences(path,source,error)){std::wcerr<<error<<L'\n';throw std::runtime_error("atomic settings write");}
		const auto loaded=avopsui::read_preferences(path);check(loaded.error.empty()&&loaded.found&&!loaded.migrated,"settings reload result");
		check(loaded.value.theme==source.theme&&loaded.value.accent==source.accent&&loaded.value.font_size==source.font_size,"theme/accent/font round trip");
		check(!loaded.value.show_status&&loaded.value.muted&&!loaded.value.limit_speed,"boolean settings round trip");
		check(loaded.value.stretch_screen&&loaded.value.fullscreen,"display settings round trip");
		check(loaded.value.window_width==source.window_width&&loaded.value.window_height==source.window_height&&loaded.value.bios_path==source.bios_path,"window and BIOS path round trip");
		check(loaded.value.keyboard_keys==source.keyboard_keys,"keyboard mapping round trip");

		auto invalid=source;invalid.keyboard_keys[15]=invalid.keyboard_keys[14];error.clear();check(!avopsui::write_preferences(path,invalid,error)&&!error.empty(),"duplicate keyboard mappings rejected");
		const auto preserved=avopsui::read_preferences(path);check(preserved.error.empty()&&preserved.value.keyboard_keys==source.keyboard_keys,"invalid write preserves previous settings");
		check(WritePrivateProfileStringW(L"UI",L"StretchScreen",nullptr,path.c_str())!=FALSE,"remove optional stretch setting");
		check(WritePrivateProfileStringW(L"Window",L"Fullscreen",nullptr,path.c_str())!=FALSE,"remove optional fullscreen setting");
		const auto old_settings=avopsui::read_preferences(path);check(old_settings.error.empty()&&!old_settings.value.stretch_screen&&!old_settings.value.fullscreen,"older settings default display options off");

		const auto legacy=directory/L"legacy.ini";check(WritePrivateProfileStringW(L"Keyboard",L"Wasd",L"1",legacy.c_str())!=FALSE,"create legacy keyboard profile");
		check(WritePrivateProfileStringW(L"Keyboard",L"NeutralOpposites",L"1",legacy.c_str())!=FALSE,"write legacy neutral setting");
		check(WritePrivateProfileStringW(L"Keyboard",L"Key14",L"74",legacy.c_str())!=FALSE,"write legacy key override");
		const auto migrated=avopsui::read_preferences(directory/L"new-settings.ini",legacy);
		check(migrated.error.empty()&&migrated.migrated&&migrated.value.wasd_profile&&migrated.value.neutral_opposites,"legacy keyboard migration");
		check(migrated.value.keyboard_keys[14]==74,"legacy keyboard override migration");

		const auto future=directory/L"future.ini";check(WritePrivateProfileStringW(L"Meta",L"SchemaVersion",L"99",future.c_str())!=FALSE,"create future profile");
		const auto rejected=avopsui::read_preferences(future);check(!rejected.error.empty(),"future schema rejected explicitly");
		WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());WritePrivateProfileStringW(nullptr,nullptr,nullptr,legacy.c_str());WritePrivateProfileStringW(nullptr,nullptr,nullptr,future.c_str());
		std::filesystem::remove_all(directory);
		std::cout<<"PASS: atomic persistence, schema validation, migration, and keyboard mapping round trip\n";
		return 0;
	}catch(const std::exception& error){
		if(!directory.empty()){std::error_code cleanup;std::filesystem::remove_all(directory,cleanup);}
		std::cerr<<error.what()<<'\n';return 1;
	}
}