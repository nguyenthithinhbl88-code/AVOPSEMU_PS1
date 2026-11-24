#include "cpu_crash_context.hpp"
#include "win_crash_diagnostics.hpp"
#include <windows.h>
#include <filesystem>

int wmain(int argc, wchar_t* argv[]) {
	if (argc != 2) return 2;
	ps1::win_diag::install_handlers();
	if (!ps1::win_diag::initialize(std::filesystem::path(argv[1]))) return 3;
	const auto capture_path = std::filesystem::path(argv[1]) / L"probe_capture.bmp";
	if (!ps1::win_diag::write_capture_record(capture_path, 640, 478, "RGB555/BMP", 1200, 1200,
											 true, "probe capture verified")) return 5;
	if (!ps1::win_diag::write_capture_record(capture_path, 640, 478, "RGB555/BMP", 600, 0,
											 false, "probe \"write\" failure")) return 6;
	ps1::record_cpu_crash_context(0x80059ef8, 0x03e00008);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	RaiseException(0xE042A001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	return 4;
}
