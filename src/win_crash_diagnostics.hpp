#pragma once
#include <cstdint>
#include <filesystem>

namespace ps1::win_diag {
bool initialize(const std::filesystem::path& log_directory) noexcept;
bool initialize_default() noexcept;
void install_handlers() noexcept;
bool write_event(const char* component, const char* message, bool flush_now = true) noexcept;
bool write_capture_record(const std::filesystem::path& output_path,
						  unsigned width,
						  unsigned height,
						  const char* pixel_format,
						  uint64_t bytes_written,
						  uint64_t bytes_verified,
						  bool success,
						  const char* details) noexcept;
void flush() noexcept;
}
