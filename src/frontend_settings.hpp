#pragma once
#include "keyboard_pad.hpp"
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <windows.h>

namespace avopsui {

enum class Theme : unsigned { Dark, Light, Amoled };

struct Preferences {
	Theme theme=Theme::Dark;
	unsigned accent=0;
	unsigned font_size=16;
	bool show_toolbar=true,show_status=true,show_fps=true,show_sidebar=true,show_debugger=false;
	bool limit_speed=true,muted=false,wasd_profile=false,neutral_opposites=false;
	unsigned window_width=1440,window_height=900;
	bool window_maximized=false;
	std::wstring bios_path;
	std::array<unsigned,16> keyboard_keys=ps1::KeyboardPad::defaults;
};

struct PreferenceLoadResult {
	Preferences value{};
	bool found=false,migrated=false;
	std::wstring error;
};

struct SettingsSnapshot {
	std::wstring bios_path,bios_name,disc_name;
	bool bios_loaded=false,disc_mounted=false;
	unsigned region=4;
};

struct SettingsActions {
	std::function<void()> changed;
	std::function<void()> choose_bios;
	std::function<SettingsSnapshot()> snapshot;
};

std::filesystem::path user_preferences_path();
PreferenceLoadResult read_preferences(const std::filesystem::path& path,const std::filesystem::path& legacy_keyboard_path={});
bool write_preferences(const std::filesystem::path& path,const Preferences& value,std::wstring& error);
bool validate_preferences(const Preferences& value,std::wstring& error);
void apply_palette(Theme theme,unsigned accent);
HWND open_settings_window(HWND owner,Preferences& value,const std::filesystem::path& path,SettingsActions actions,unsigned font_size=16);
void refresh_settings_window(HWND window,unsigned font_size=16);

}