#pragma once
#include "keyboard_pad.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
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
	bool stretch_screen=false,fullscreen=false;
	bool cpu_profiler_enabled=false,cpu_trace_enabled=false;
	unsigned cpu_profile_sample_interval=4096,cpu_trace_buffer_size=128;
	bool cpu_break_on_reserved=false,cpu_break_on_exception=false,cpu_pc_breakpoint_enabled=false,cpu_diagnostic_logging=false;
	uint32_t cpu_pc_breakpoint_address=0;
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
	uint64_t cpu_instructions=0,cpu_profile_samples=0,cpu_profile_fetch_ns=0,cpu_profile_trace_ns=0,cpu_profile_decode_ns=0,cpu_profile_execute_ns=0;
	uint64_t cpu_trace_records=0,cpu_trace_capacity=0,cpu_diagnostic_events=0;
	double cpu_mips=0;
	bool cpu_mips_valid=false;
};

struct SettingsActions {
	std::function<void()> changed;
	std::function<void()> choose_bios;
	std::function<void()> export_cpu_trace;
	std::function<void()> export_cpu_diagnostics;
	std::function<SettingsSnapshot()> snapshot;
};

std::filesystem::path user_preferences_path();
PreferenceLoadResult read_preferences(const std::filesystem::path& path,const std::filesystem::path& legacy_keyboard_path={});
bool write_preferences(const std::filesystem::path& path,const Preferences& value,std::wstring& error);
bool validate_preferences(const Preferences& value,std::wstring& error);
void restore_cpu_defaults(Preferences& value);
void apply_palette(Theme theme,unsigned accent);
HWND open_settings_window(HWND owner,Preferences& value,const std::filesystem::path& path,SettingsActions actions,unsigned font_size=16);
void refresh_settings_window(HWND window,unsigned font_size=16);

}