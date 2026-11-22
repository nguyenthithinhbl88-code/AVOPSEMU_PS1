#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace avopsui {

struct GameSession {
	uint64_t started_utc=0;
	uint64_t duration_seconds=0;
};

struct GameEntry {
	std::filesystem::path path;
	std::wstring title,serial,region,cover_path;
	uint64_t play_seconds=0,last_played_utc=0;
	bool has_host_overrides=false,limit_speed=true,muted=false;
	std::vector<GameSession> sessions;
};

enum class GamePlayState { Stopped, Paused, Running };

class GameLibrary {
public:
	explicit GameLibrary(std::filesystem::path database_path);
	bool load(std::wstring& error);
	bool save(std::wstring& error) const;
	bool import_game(const std::filesystem::path& path,std::wstring& error);
	bool remove_game(const std::filesystem::path& path,std::wstring& error);
	bool update_disc_metadata(const std::filesystem::path& path,const std::string& boot_name,unsigned region,std::wstring& error);
	bool set_cover(const std::filesystem::path& path,const std::filesystem::path& cover,std::wstring& error);
	bool set_host_overrides(const std::filesystem::path& path,bool enabled,bool limit_speed,bool muted,std::wstring& error);
	void begin_session(const std::filesystem::path& path);
	void pause_session();
	void stop_session();
	const std::vector<GameEntry>& entries() const{return entries_;}
	const GameEntry* find(const std::filesystem::path& path) const;
	GamePlayState state(const std::filesystem::path& path) const;
	uint64_t playtime_seconds(const GameEntry& entry) const;
	const std::filesystem::path& active_path() const{return active_path_;}
private:
	void finish_session();
	GameEntry* find_mutable(const std::filesystem::path& path);
	std::filesystem::path database_path_;
	std::vector<GameEntry> entries_;
	std::filesystem::path active_path_;
	GamePlayState state_=GamePlayState::Stopped;
	std::chrono::steady_clock::time_point active_since_{};
	uint64_t active_started_utc_=0;
};

struct LibraryActions {
	std::function<void(const std::filesystem::path&)> mount_disc;
	std::function<void(const std::filesystem::path&)> boot_disc;
	std::function<void(const std::filesystem::path&)> profile_changed;
};

HWND open_game_library_window(HWND owner,GameLibrary& library,LibraryActions actions);
void set_game_library_font_size(unsigned font_size);
void refresh_game_library_window(HWND window);
void refresh_game_library_window_live(HWND window);

}