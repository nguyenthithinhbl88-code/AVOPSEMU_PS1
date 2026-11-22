#pragma once
#include "core.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace ps1 {
// Caller pauses/joins the worker before taking the snapshot.
struct CaptureFileResult {
 std::filesystem::path path;
 uint64_t expected_bytes=0,bytes_written=0,bytes_verified=0;
 uint32_t error_code=0;
 std::string error;
 bool success=false;
};
struct CaptureResult {
 unsigned framebuffer_width=0,framebuffer_height=0;
 std::string pixel_format;
 std::vector<CaptureFileResult> files;
 bool success()const noexcept;
 uint64_t total_bytes_written()const noexcept;
 uint64_t total_bytes_verified()const noexcept;
};
CaptureFileResult write_verified_file(const std::filesystem::path& path,const void* data,size_t size);
CaptureResult save_debug_images_detailed(const Bus& bus,const std::filesystem::path& log_path);
bool save_debug_images(const Bus& bus,const std::filesystem::path& log_path);
}
