#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

std::wstring quote_argument(const std::wstring& value) {
	std::wstring result = L"\"";
	size_t backslashes = 0;
	for (wchar_t ch : value) {
		if (ch == L'\\') {
			++backslashes;
		} else if (ch == L'\"') {
			result.append(backslashes * 2 + 1, L'\\');
			result += ch;
			backslashes = 0;
		} else {
			result.append(backslashes, L'\\');
			backslashes = 0;
			result += ch;
		}
	}
	result.append(backslashes * 2, L'\\');
	result += L"\"";
	return result;
}

std::string read_all(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) throw std::runtime_error("Crash probe did not create crash.log");
	std::ostringstream content;
	content << file.rdbuf();
	return content.str();
}
}

int wmain(int argc, wchar_t* argv[]) {
	try {
		check(argc == 2, "crash probe executable path is required");
		const auto nonce = std::to_wstring(GetCurrentProcessId()) + L"_" +
			std::to_wstring(GetTickCount64());
		const auto test_root = std::filesystem::temp_directory_path() /
			(L"avopsemu_crash_test_" + nonce);
		const auto log_directory = test_root / L"logs";
		std::filesystem::create_directories(test_root);

		std::wstring command = quote_argument(argv[1]) + L" " + quote_argument(log_directory.wstring());
		std::vector<wchar_t> mutable_command(command.begin(), command.end());
		mutable_command.push_back(L'\0');
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		check(CreateProcessW(argv[1], mutable_command.data(), nullptr, nullptr, FALSE,
							 CREATE_NO_WINDOW, nullptr, test_root.c_str(), &startup, &process) != FALSE,
			  "could not start isolated crash probe");

		const DWORD wait_result = WaitForSingleObject(process.hProcess, 15000);
		if (wait_result != WAIT_OBJECT_0) {
			TerminateProcess(process.hProcess, 0xdead);
			WaitForSingleObject(process.hProcess, 5000);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			throw std::runtime_error("isolated crash probe did not terminate");
		}
		DWORD exit_code = 0;
		const bool got_exit_code = GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		check(got_exit_code && exit_code != 0 && exit_code != 4,
			  "probe should terminate from the deliberate exception");

		const auto log = read_all(log_directory / L"crash.log");
		check(log.find("kind=unhandled_seh") != std::string::npos, "unhandled exception record missing");
		check(log.find("exception_code=0xE042A001") != std::string::npos, "exception code missing");
		check(log.find("exception_address=0x") != std::string::npos, "fault address missing");
		const auto module_start = log.find("module=");
		check(module_start != std::string::npos, "fault module field missing");
		const auto module_end = log.find(' ', module_start);
		check(module_end != std::string::npos && log.substr(module_start + 7, module_end - module_start - 7) != "unknown",
			  "fault module could not be resolved");
		check(log.find("thread_id=") != std::string::npos, "thread ID missing");
		const auto hash_start = log.find("build_sha256=");
		check(hash_start != std::string::npos, "binary SHA-256 field missing");
		const auto hash_begin = hash_start + std::string("build_sha256=").size();
		check(hash_begin + 64 <= log.size() && std::all_of(log.begin() + hash_begin, log.begin() + hash_begin + 64,
			  [](unsigned char ch) { return std::isxdigit(ch) != 0; }), "binary SHA-256 is not 64 hexadecimal digits");
		check(log.find("psx_pc=0x80059EF8") != std::string::npos, "CPU PC breadcrumb missing");
		check(log.find("psx_opcode=0x03E00008") != std::string::npos, "CPU opcode breadcrumb missing");
		const auto capture_log = read_all(log_directory / L"capture.log");
		check(capture_log.find("dimensions=640x478") != std::string::npos, "capture dimensions missing");
		check(capture_log.find("pixel_format=\"RGB555/BMP\"") != std::string::npos, "capture pixel format missing");
		check(capture_log.find("bytes_written=1200 bytes_verified=1200 result=success") != std::string::npos,
			  "verified capture result missing");
		check(capture_log.find("bytes_written=600 bytes_verified=0 result=failure") != std::string::npos,
			  "capture failure result missing");
		check(capture_log.find("details=\"probe \\\"write\\\" failure\"") != std::string::npos,
			  "capture details quotes were not escaped");

		std::error_code cleanup_error;
		std::filesystem::remove_all(test_root, cleanup_error);
		check(!cleanup_error, "could not remove isolated crash-test files");
		std::cout << "PASS: isolated unhandled-exception record, build hash, module/thread metadata, and CPU breadcrumb\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
