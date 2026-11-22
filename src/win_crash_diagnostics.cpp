#include "win_crash_diagnostics.hpp"
#include "cpu_crash_context.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <array>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

namespace ps1::win_diag {
namespace {
constexpr size_t max_modules = 128;
const HANDLE invalid_file = INVALID_HANDLE_VALUE;

struct ModuleRange {
	uintptr_t base = 0;
	size_t size = 0;
	char name[64]{};
};

struct FixedBuffer {
	char data[1024]{};
	size_t size = 0;

	void append(const char* value) noexcept {
		if (!value) return;
		while (*value && size < sizeof(data) - 1) data[size++] = *value++;
	}

	void append_hex(uint64_t value, unsigned digits) noexcept {
		static constexpr char alphabet[] = "0123456789ABCDEF";
		append("0x");
		for (unsigned i = digits; i > 0 && size < sizeof(data) - 1; --i)
			data[size++] = alphabet[(value >> ((i - 1) * 4)) & 15];
	}

	void append_decimal(uint64_t value) noexcept {
		char digits[24]{};
		unsigned count = 0;
		do {
			digits[count++] = char('0' + value % 10);
			value /= 10;
		} while (value && count < sizeof(digits));
		while (count && size < sizeof(data) - 1) data[size++] = digits[--count];
	}

	void finish_line() noexcept {
		if (size < sizeof(data) - 1) data[size++] = '\n';
	}
};

HANDLE session_log = invalid_file;
HANDLE capture_log = invalid_file;
HANDLE crash_log = invalid_file;
std::mutex initialization_mutex;
std::mutex log_mutex;
bool initialized = false;
char build_sha256[65] = "unavailable";
std::array<ModuleRange, max_modules> modules{};
size_t module_count = 0;

bool write_all(HANDLE file, const char* data, size_t size) noexcept {
	while (size) {
		const DWORD chunk = static_cast<DWORD>(size > 0x7fffffff ? 0x7fffffff : size);
		DWORD written = 0;
		if (!WriteFile(file, data, chunk, &written, nullptr) || written == 0) return false;
		data += written;
		size -= written;
	}
	return true;
}

std::string timestamp() {
	SYSTEMTIME time{};
	GetLocalTime(&time);
	std::ostringstream out;
	out << std::setfill('0') << std::setw(4) << time.wYear << '-'
		<< std::setw(2) << time.wMonth << '-' << std::setw(2) << time.wDay << 'T'
		<< std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute << ':'
		<< std::setw(2) << time.wSecond << '.' << std::setw(3) << time.wMilliseconds;
	return out.str();
}

std::string clean_field(const char* value) {
	std::string result = value ? value : "";
	for (char& ch : result) if (ch == '\r' || ch == '\n' || ch == '\0') ch = ' ';
	return result;
}

std::string escape_quoted_field(const char* value) {
	std::string result;
	if (!value) return result;
	for (const char* current = value; *current; ++current) {
		if (*current == '\r' || *current == '\n') result.push_back(' ');
		else {
			if (*current == '\\' || *current == '"') result.push_back('\\');
			result.push_back(*current);
		}
	}
	return result;
}

bool append_record(HANDLE file, const std::string& record, bool flush_now) noexcept {
	try {
		std::lock_guard<std::mutex> lock(log_mutex);
		if (file == invalid_file) return false;
		if (!write_all(file, record.data(), record.size()) || !write_all(file, "\n", 1)) {
			OutputDebugStringA("AVOPSEMU diagnostics: log write failed.\n");
			return false;
		}
		if (flush_now && !FlushFileBuffers(file)) {
			OutputDebugStringA("AVOPSEMU diagnostics: log flush failed.\n");
			return false;
		}
		return true;
	} catch (...) {
		OutputDebugStringA("AVOPSEMU diagnostics: log formatting failed.\n");
		return false;
	}
}

bool hash_executable(const wchar_t* path, char output[65]) {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	HANDLE file = invalid_file;
	std::array<UCHAR, 4096> hash_object{};
	std::array<UCHAR, 64> digest{};
	std::array<UCHAR, 65536> chunk{};
	ULONG object_length = 0, digest_length = 0, property_bytes = 0;
	bool success = false;

	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == invalid_file) return false;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto cleanup;
	if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length),
						  sizeof(object_length), &property_bytes, 0) < 0 || object_length > hash_object.size()) goto cleanup;
	if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digest_length),
						  sizeof(digest_length), &property_bytes, 0) < 0 || digest_length != 32) goto cleanup;
	if (BCryptCreateHash(algorithm, &hash, hash_object.data(), object_length, nullptr, 0, 0) < 0) goto cleanup;
	for (;;) {
		DWORD read = 0;
		if (!ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) goto cleanup;
		if (!read) break;
		if (BCryptHashData(hash, chunk.data(), read, 0) < 0) goto cleanup;
	}
	if (BCryptFinishHash(hash, digest.data(), digest_length, 0) < 0) goto cleanup;
	for (ULONG i = 0; i < digest_length; ++i) {
		static constexpr char alphabet[] = "0123456789abcdef";
		output[i * 2] = alphabet[digest[i] >> 4];
		output[i * 2 + 1] = alphabet[digest[i] & 15];
	}
	output[64] = '\0';
	success = true;

cleanup:
	if (hash) BCryptDestroyHash(hash);
	if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
	CloseHandle(file);
	return success;
}

void cache_modules() noexcept {
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
	if (snapshot == INVALID_HANDLE_VALUE) return;
	MODULEENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	if (Module32FirstW(snapshot, &entry)) {
		do {
			if (module_count == modules.size()) break;
			auto& target = modules[module_count++];
			target.base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
			target.size = entry.modBaseSize;
			if (!WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, target.name,
									 static_cast<int>(sizeof(target.name)), nullptr, nullptr)) {
				const char unknown[] = "unknown";
				for (size_t i = 0; i < sizeof(unknown); ++i) target.name[i] = unknown[i];
			}
		} while (Module32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
}

const char* module_for(uintptr_t address) noexcept {
	for (size_t i = 0; i < module_count; ++i) {
		const uintptr_t base = modules[i].base;
		if (address >= base && address - base < modules[i].size) return modules[i].name;
	}
	return "unknown";
}

uintptr_t native_instruction_pointer(const EXCEPTION_POINTERS* pointers) noexcept {
	if (!pointers || !pointers->ContextRecord) return 0;
#if defined(_M_X64) || defined(__x86_64__)
	return static_cast<uintptr_t>(pointers->ContextRecord->Rip);
#elif defined(_M_IX86) || defined(__i386__)
	return static_cast<uintptr_t>(pointers->ContextRecord->Eip);
#elif defined(_M_ARM64) || defined(__aarch64__)
	return static_cast<uintptr_t>(pointers->ContextRecord->Pc);
#else
	return 0;
#endif
}

void write_crash_record(const char* kind, EXCEPTION_POINTERS* pointers) noexcept {
	if (crash_log == invalid_file) return;
	FixedBuffer line;
	FILETIME now{};
	GetSystemTimeAsFileTime(&now);
	const uint64_t file_time = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
	const bool has_exception = pointers && pointers->ExceptionRecord;
	const uintptr_t address = has_exception ? reinterpret_cast<uintptr_t>(pointers->ExceptionRecord->ExceptionAddress) : 0;
	const auto cpu = current_cpu_crash_context();

	line.append("timestamp_filetime="); line.append_hex(file_time, 16);
	line.append(" kind="); line.append(kind);
	line.append(" exception_code=");
	if (has_exception) line.append_hex(pointers->ExceptionRecord->ExceptionCode, 8);
	else line.append("CPP_TERMINATE");
	line.append(" exception_address=");
	if (has_exception) line.append_hex(address, sizeof(uintptr_t) * 2);
	else line.append("unavailable");
	line.append(" module="); line.append(has_exception ? module_for(address) : "unavailable");
	line.append(" thread_id="); line.append_decimal(GetCurrentThreadId());
	line.append(" host_pc=");
	const uintptr_t host_pc = native_instruction_pointer(pointers);
	if (host_pc) line.append_hex(host_pc, sizeof(uintptr_t) * 2);
	else line.append("unavailable");
	line.append(" build_sha256="); line.append(build_sha256);
	line.append(" psx_pc=");
	if (cpu.valid) line.append_hex(cpu.pc, 8);
	else line.append("unavailable");
	line.append(" psx_opcode=");
	if (cpu.valid) line.append_hex(cpu.opcode, 8);
	else line.append("unavailable");
	line.finish_line();

	if (line.size && !write_all(crash_log, line.data, line.size))
		OutputDebugStringA("AVOPSEMU diagnostics: crash record write failed.\n");
	else if (line.size && !FlushFileBuffers(crash_log))
		OutputDebugStringA("AVOPSEMU diagnostics: crash record flush failed.\n");
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* pointers) noexcept {
	write_crash_record("unhandled_seh", pointers);
	return EXCEPTION_CONTINUE_SEARCH;
}

void terminate_handler() noexcept {
	write_crash_record("cpp_terminate", nullptr);
	std::abort();
}

HANDLE open_append_log(const std::filesystem::path& path) noexcept {
	return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
}

bool initialize(const std::filesystem::path& log_directory) noexcept {
	try {
		std::lock_guard<std::mutex> lock(initialization_mutex);
		if (initialized) return true;
		if (log_directory.empty()) return false;
		std::error_code error;
		std::filesystem::create_directories(log_directory, error);
		if (error) return false;

		session_log = open_append_log(log_directory / L"session.log");
		if (session_log == invalid_file) return false;
		capture_log = open_append_log(log_directory / L"capture.log");
		if (capture_log == invalid_file) {
			CloseHandle(session_log); session_log = invalid_file;
			return false;
		}
		crash_log = open_append_log(log_directory / L"crash.log");
		if (crash_log == invalid_file) {
			CloseHandle(capture_log); capture_log = invalid_file;
			CloseHandle(session_log); session_log = invalid_file;
			return false;
		}

		wchar_t executable[32768]{};
		const DWORD path_length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
		if (path_length && path_length < std::size(executable)) hash_executable(executable, build_sha256);
		cache_modules();
		initialized = true;
		std::string image = path_length && path_length < std::size(executable)
			? std::filesystem::path(executable).u8string() : "unknown";
		return write_event("startup", ("image=" + clean_field(image.c_str()) + " build_sha256=" + build_sha256).c_str(), true);
	} catch (...) {
		return false;
	}
}

bool initialize_default() noexcept {
	wchar_t local_app_data[32768]{};
	const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data,
												   static_cast<DWORD>(std::size(local_app_data)));
	if (!length || length >= std::size(local_app_data)) return false;
	try {
		return initialize(std::filesystem::path(local_app_data) / L"AVOPSEMU" / L"Logs");
	} catch (...) {
		return false;
	}
}

void install_handlers() noexcept {
	SetUnhandledExceptionFilter(unhandled_exception_filter);
	std::set_terminate(terminate_handler);
}

bool write_event(const char* component, const char* message, bool flush_now) noexcept {
	if (!initialized || session_log == invalid_file) return false;
	try {
		const auto cpu = current_cpu_crash_context();
		std::ostringstream line;
		line << "timestamp=" << timestamp() << " component=" << clean_field(component)
			 << " message=" << clean_field(message);
		if (cpu.valid) line << " psx_pc=0x" << std::hex << std::uppercase << cpu.pc
						   << " psx_opcode=0x" << cpu.opcode;
		return append_record(session_log, line.str(), flush_now);
	} catch (...) {
		OutputDebugStringA("AVOPSEMU diagnostics: event formatting failed.\n");
		return false;
	}
}

bool write_capture_record(const std::filesystem::path& output_path,
						  unsigned width,
						  unsigned height,
						  const char* pixel_format,
						  uint64_t bytes_written,
						  uint64_t bytes_verified,
						  bool success,
						  const char* details) noexcept {
	if (!initialized || capture_log == invalid_file) return false;
	try {
		const auto path_text = output_path.u8string();
		std::ostringstream line;
		line << "timestamp=" << timestamp() << " type=framebuffer_capture"
			 << " dimensions=" << width << 'x' << height
			 << " pixel_format=\"" << escape_quoted_field(pixel_format) << '\"'
			 << " output_path=\"" << escape_quoted_field(path_text.c_str()) << '\"'
			 << " bytes_written=" << bytes_written << " bytes_verified=" << bytes_verified
			 << " result=" << (success ? "success" : "failure")
			 << " details=\"" << escape_quoted_field(details) << '\"';
		return append_record(capture_log, line.str(), true);
	} catch (...) {
		OutputDebugStringA("AVOPSEMU diagnostics: capture record formatting failed.\n");
		return false;
	}
}

void flush() noexcept {
	if (!initialized) return;
	try {
		std::lock_guard<std::mutex> lock(log_mutex);
		if (session_log != invalid_file && !FlushFileBuffers(session_log))
			OutputDebugStringA("AVOPSEMU diagnostics: session log flush failed.\n");
		if (capture_log != invalid_file && !FlushFileBuffers(capture_log))
			OutputDebugStringA("AVOPSEMU diagnostics: capture log flush failed.\n");
	} catch (...) {
		OutputDebugStringA("AVOPSEMU diagnostics: periodic flush failed.\n");
	}
}
}
