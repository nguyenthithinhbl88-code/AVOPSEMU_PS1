#include "frontend_library.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
}

int main(){
	std::filesystem::path directory;
	try{
		directory=std::filesystem::temp_directory_path()/(L"avopsemu-library-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
		std::filesystem::create_directories(directory);
		const auto database=directory/L"library.tsv",game_path=directory/L"Test Game.cue",cover_path=directory/L"cover.png";
		{std::ofstream game(game_path,std::ios::binary);game<<"FILE \"game.bin\" BINARY\n";}
		{std::ofstream cover(cover_path,std::ios::binary);cover<<"test";}
		avopsui::GameLibrary library(database);std::wstring error;
		check(library.load(error)&&library.entries().empty(),"missing library loads as empty");
		check(library.import_game(game_path,error),"import supported disc image");
		check(library.import_game(game_path,error)&&library.entries().size()==1,"duplicate import is idempotent");
		check(!library.import_game(directory/L"notes.txt",error)&&!error.empty(),"unsupported extension rejected");
		check(library.update_disc_metadata(game_path,"cdrom:\\SLUS_123.45;1",1,error),"SYSTEM.CNF metadata update");
		const auto* entry=library.find(game_path);check(entry&&entry->serial==L"SLUS_123.45"&&entry->region.find(L"USA")!=std::wstring::npos,"game code and region recorded");
		check(library.set_cover(game_path,cover_path,error),"cover path recorded");
		check(library.set_host_overrides(game_path,true,false,true,error),"per-game host profile recorded");
		library.begin_session(game_path);std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		entry=library.find(game_path);check(entry&&library.state(game_path)==avopsui::GamePlayState::Running&&library.playtime_seconds(*entry)>=1,"running playtime is measured");
		check(library.save(error),"running playtime snapshot is saved");
		avopsui::GameLibrary loaded(database);check(loaded.load(error),"library reload");
		entry=loaded.find(game_path);check(entry&&entry->play_seconds>=1&&entry->sessions.size()==1,"playtime and active session survive reload");
		check(entry->cover_path==cover_path.wstring()&&entry->has_host_overrides&&!entry->limit_speed&&entry->muted,"cover and per-game settings round trip");
		library.pause_session();check(library.state(game_path)==avopsui::GamePlayState::Paused,"pause state recorded");
		library.begin_session(game_path);std::this_thread::sleep_for(std::chrono::milliseconds(1100));library.stop_session();
		check(library.state(game_path)==avopsui::GamePlayState::Stopped,"stop state recorded");
		entry=library.find(game_path);check(entry&&entry->sessions.size()==2&&entry->play_seconds>=2,"paused and stopped sessions accumulate once");
		check(library.save(error),"final library save");avopsui::GameLibrary final_load(database);check(final_load.load(error),"final library reload");
		entry=final_load.find(game_path);check(entry&&entry->sessions.size()==2&&entry->play_seconds>=2,"session history persists");
		check(final_load.remove_game(game_path,error)&&final_load.save(error),"remove library entry without deleting image");
		check(std::filesystem::exists(game_path)&&final_load.entries().empty(),"remove leaves source image intact");
		const auto corrupt=directory/L"corrupt.tsv";{std::ofstream file(corrupt,std::ios::binary);file<<"not a library\n";}
		avopsui::GameLibrary invalid(corrupt);check(!invalid.load(error)&&!error.empty(),"unsupported database rejected explicitly");
		std::filesystem::remove_all(directory);
		std::cout<<"PASS: library persistence, metadata, cover path, per-game options, play sessions, searchable records, and safe removal\n";
		return 0;
	}catch(const std::exception& error){
		if(!directory.empty()){std::error_code cleanup;std::filesystem::remove_all(directory,cleanup);}
		std::cerr<<error.what()<<'\n';return 1;
	}
}