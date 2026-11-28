# AVOPSEMU PS1 v51

### An Experimental Sony PlayStation 1 Emulator Written in C++17

**AVOPSEMU PS1** is an experimental Sony PlayStation 1 (PS1 / PSX) emulator developed in C++17, featuring a native Windows Win32 frontend for Visual Studio 2022.

The project focuses on recreating the original PlayStation hardware environment through software emulation, with ongoing development in CPU execution, GPU rendering, CD-ROM processing, DMA transfers, SPU audio, SIO communication, hardware timing, and game compatibility.

Formerly known as **PSLab**, the project has evolved into AVOPSEMU PS1. The PlayStation 2 branch has been separated into an independent project named AVOPSEMU_PS2. This repository exclusively contains the PlayStation 1 emulator and does not build or link the PS2 project.

| Project Information | Details |
|---|---|
| Project | AVOPSEMU PS1 |
| Version | v51 |
| Platform | Windows |
| Programming Language | C++17 |
| Frontend | Win32 / GDI |
| Development Environment | Visual Studio 2022 |
| Build System | CMake |
| Architecture | x64 |
| Project Type | Experimental PlayStation Emulator |

---

# 🎮 FIFA FOOTBALL 2005 – RUNNING ON AVOPSEMU PS1

One of the most significant milestones in AVOPSEMU PS1 development is successfully booting and running **FIFA Football 2005 for Sony PlayStation 1**.

AVOPSEMU PS1 has progressed beyond BIOS-only execution and demonstrated the ability to load a commercial PlayStation game, reach its main menu, and successfully enter an actual football match.

This achievement represents an important step forward in the development of an independently developed PS1 emulator.

## FIFA Football 2005 – Actual Emulator Screenshot

<img width="1915" height="974" alt="image" src="https://github.com/user-attachments/assets/a90992ab-ee34-46b1-92cf-8ef6fc295e4f" />

*An actual screenshot captured directly from AVOPSEMU PS1.*

## FIFA 2005 Compatibility Status

| Test | Status |
|---|---|
| PlayStation BIOS initialization | Working |
| FIFA 2005 boot process | Successful |
| FIFA 2005 main menu | Reached |
| Match loading | Completed |
| In-game football match | Reached |
| Graphics rendering | Working |
| Performance optimization | In progress |
| Full game compatibility | Not yet confirmed |

### Performance and Known Limitations

FIFA Football 2005 is currently a demanding test case for AVOPSEMU PS1.

During the recorded test:

- Reaching the FIFA 2005 main menu took approximately 19 minutes.
- Loading an actual football match took approximately 67 minutes.
- CPU performance fluctuated during the loading process.
- Video timing and frame submission performance still require investigation.

These results show that reaching an actual match is possible, but performance remains a major development challenge.

The current focus is investigating CPU execution overhead, CD-ROM data processing, DMA transfers, audio synchronization, GPU rendering, and frontend performance.

**This is a compatibility milestone, not a claim of full-speed gameplay or perfect emulation.**

---

# Project Goals

AVOPSEMU PS1 is being developed with the following long-term objectives:

- Improve PlayStation 1 hardware emulation accuracy.
- Increase CPU emulation performance.
- Optimize GPU rendering and VRAM operations.
- Improve CD-ROM command and sector processing.
- Investigate DMA transfer behavior and synchronization.
- Improve SPU audio playback and timing.
- Expand commercial game compatibility.
- Improve Windows frontend performance and usability.
- Maintain reliable diagnostics and reproducible testing.

# Core Emulation Systems

## CPU – MIPS R3000A

The CPU subsystem implements the PlayStation's MIPS R3000A instruction execution environment.

Development areas include:

- Instruction fetching, decoding, and execution.
- CPU register and memory operations.
- COP0 system control.
- Exception and interrupt handling.
- Debug registers and breakpoints.
- CPU performance profiling and optimization.

## GPU – Graphics Processing

The GPU subsystem processes PlayStation graphics commands and renders the console's framebuffer.

Development areas include:

- GP0 and GP1 command processing.
- Polygon and rectangle rendering.
- Texture and CLUT operations.
- Gouraud shading.
- VRAM access and framebuffer handling.
- Rendering performance optimization.

## CD-ROM

The CD-ROM subsystem handles disc commands and sector data processing.

Development areas include:

- CD-ROM command execution.
- Sector reading.
- Data FIFO management.
- Disc image support.
- CD-ROM interrupt handling.
- Integration with DMA transfers.

## DMA – Direct Memory Access

The DMA subsystem handles data transfers between system memory and PlayStation hardware devices.

Development areas include:

- DMA channel management.
- Transfer synchronization.
- CD-ROM data transfers.
- GPU data transfers.
- Memory access diagnostics.

## SPU – Sound Processing Unit

The SPU subsystem handles PlayStation audio processing and playback.

Development areas include:

- Voice processing.
- PCM audio generation.
- Audio buffering.
- Playback synchronization.
- WSOLA-based audio processing.
- Windows audio backend integration.

## SIO – Serial Input/Output

The SIO subsystem provides serial communication and controller protocol handling.

Current functionality includes:

- Digital controller communication.
- Serial data transmission.
- RX FIFO handling.
- ACK and interrupt behavior.
- Keyboard-based controller input.

---

# v51 Core Update

### CPU Improvements

- Completed COP0 TAR handling.
- Implemented CAUSE.BT behavior.
- Added DCIC mask handling.
- Implemented BPC/BPCM breakpoints with a dedicated debug vector.
- Reduced redundant interrupt sampling when device state has not changed.

### GPU Improvements

- Reduced repeated texture and CLUT address calculations in large sprites.
- Preserved direct VRAM reads to maintain correct ordering when textures and framebuffer regions overlap.

### Audio Improvements

- Optimized WSOLA overlap search using cumulative energy calculations.
- Preserved PCM samples and SPU timing behavior.

### Performance and Validation

Detailed development information is available in:

- `PS1_PERFORMANCE_CPU_UPDATE_V51.md`
- `BENCHMARK_V51.txt`
- `VALIDATION_V51.txt`

The PS2 v50 branch remains unchanged.

Real-game FPS and Windows audio device quality have not yet been fully validated for this release.

---

# Windows Frontend

AVOPSEMU PS1 includes a native Windows Win32/GDI frontend.

### Main Features

- Settings Center.
- Game Library.
- 4:3 game display area.
- Dark, Light, and AMOLED themes.
- Accent color and font settings.
- Toolbar and status bar configuration.
- FPS HUD.
- Sidebar and debugger visibility settings.
- Window size configuration.
- Keyboard mapping.
- Audio mute controls.
- Frame pacing configuration.
- BIOS path configuration.

### Keyboard Shortcuts

| Key | Function |
|---|---|
| F5 | Run / Pause |
| F9 | Toggle existing debugger |
| F11 | Toggle theater mode |

### Settings Storage

Settings are stored in:

`%LOCALAPPDATA%\AVOPSEMU\settings.ini`

The settings system includes schema validation and migration from older keyboard configurations.

### Game Library

The Game Library stores its data in:

`%LOCALAPPDATA%\AVOPSEMU\library.tsv`

Supported functionality includes:

- Importing CUE, BIN, and ISO disc images.
- Searching and sorting games.
- Reading `SYSTEM.CNF` metadata.
- Loading PNG, JPEG, and BMP covers.
- Mounting discs.
- Booting games.
- Modeled per-game host settings limited to 1x and mute.

Playtime is recorded per session and only accumulates when a game disc has booted and the Runner is actually running.

BIOS-only execution and homebrew EXE execution are not counted as game sessions.

### Frontend Limitations

The following features have not been implemented:

- Gamepad and analog controller support.
- Controller vibration.
- Per-game controller profiles.
- Dockable debugger panels.
- Animation and language settings.
- Full debugger studio functionality.
- Memory hex editor.
- Execution trace viewer.
- GPU command monitor.

The CPU, GPU, CD-ROM, Audio, and Timing settings pages display actual implemented capabilities. Unsupported features are not presented as functional options.

The Windows GUI executable is named **AVOPSEMU.exe**. The internal CMake target remains `pslab` for build configuration compatibility.

---

# Modern UI – WinUI 3

The project also contains an independent WinUI 3/C++/WinRT desktop application.

Project location:

`modern_ui/AVOPSEMU.ModernUI.vcxproj`

The Modern UI application does not replace the Win32/GDI frontend and does not link against the emulator core.

### Current Interface

- Dashboard.
- Game Library.
- Game Details.
- Settings.
- About.

The library reads the existing `library.tsv` data and supports searching, filtering, sorting, metadata viewing, and cover display.

### Prototype Limitations

The following features are not connected to the emulator core:

- Game execution.
- Disc mounting and booting.
- Live FPS and runtime status.
- Play history.
- Core configuration.

These capabilities are explicitly marked as Prototype or Not Connected rather than displaying simulated emulator states.

Build independently using:

`modern_ui/AVOPSEMU.ModernUI.vcxproj`

Configuration: `Release | x64`.

---

# Project Separation – AVOPSEMU_PS2

Starting with the v46 project separation, AVOPSEMU PS1 became an independent repository.

The PlayStation 2 source code, tests, ELF demos, and PS2 documentation were moved into the separate AVOPSEMU_PS2 project.

The PS2 core was preserved independently.

There is no required parent CMake project, and the PS1 and PS2 project directories do not need to be placed next to each other.

**This repository is exclusively for AVOPSEMU PS1.**

---

# Build and Run

### Requirements

1. Visual Studio 2022 or newer.
2. Desktop development with C++ workload.
3. C++ CMake tools for Windows.
4. Windows SDK.

### Build Instructions

1. Extract the AVOPSEMU_PS1 repository.
2. Open the folder containing `CMakeLists.txt` using Visual Studio.
3. Select the `windows-release` preset.
4. Select `AVOPSEMU.exe` as the startup item. The internal CMake target is `pslab`.
5. Build and run using Ctrl+F5.
6. Load a PlayStation 1 BIOS through Settings Center → BIOS or File → Load BIOS.
7. Open Game Library and import a CUE, BIN, or ISO disc image.
8. Select Mount Disc or Boot / Run.

For multi-track BIN/CUE images, select the `.cue` file and keep all referenced BIN files in the same directory with their expected filenames.

### BIOS and Game Files

AVOPSEMU PS1 does not distribute PlayStation BIOS files or commercial game images.

Users must provide their own legally obtained files.

A PlayStation BIOS image is required for the normal BIOS initialization process used by commercial games.

Direct homebrew EXE execution does not replace the BIOS initialization process required by commercial games.

---

# Default Controller Mapping

| Keyboard | PlayStation Controller |
|---|---|
| Arrow Keys | D-Pad |
| Z | Cross |
| X | Circle |
| A | Square |
| S | Triangle |
| Enter | Start |
| Shift | Select |
| Q | L1 |
| W | R1 |
| E | L2 |
| R | R2 |

Click the game window before playing.

The Display menu includes a WASD preset and options to test holding Cross or Start for one second.

Alt+Tab releases keyboard input.

Only digital pad input is currently supported. Analog controls and vibration have not been implemented.

---

# Development Status and Validation

### Release v51

The v51 release includes CPU, GPU, and WSOLA improvements based on the v46 project foundation.

Historical benchmarks and validation information are available in:

- `PS1_PERFORMANCE_CPU_UPDATE_V51.md`
- `BENCHMARK_V51.txt`
- `VALIDATION_V51.txt`

Benchmark results should not be interpreted as real-game FPS measurements.

### Windows Release Validation

The Windows Release build has been built and executed through the GUI.

A user-provided SCPH-1001 BIOS was loaded and validated for its expected file size. The emulator successfully progressed to the BIOS Main Menu, and keyboard input was delivered while the BIOS was running.

### Game Library Validation

The Game Library was tested with a GTA 2 CUE image.

The following operations were verified:

- Importing the disc image.
- Saving the game library entry.
- Detecting `SLUS_007.89` and USA metadata from `SYSTEM.CNF`.
- Mounting the disc successfully.

This test does not establish verified GTA 2 gameplay compatibility.

### Automated Tests

The latest recorded full Windows Release CTest result:

**51 / 54 tests passed.**

Three tests failed during temporary-directory cleanup:

- `cdda_tests`
- `disc_tests`
- `xa_audio_tests`

The failures occurred when `std::filesystem::remove_all` attempted to remove temporary files that were still held open by a process.

The same cleanup issue had appeared in an earlier baseline run.

`frontend_preferences_tests` and `frontend_library_tests` both passed.

### Performance Metrics

The frontend displays two different timing indicators:

- **Hz:** Modeled VBlank frequency.
- **FPS GDI:** Frame submission rate to the GDI rendering path.

Neither should be interpreted as the physical refresh rate of the monitor.

---

# Crash Diagnostics and Windows Capture

Session logs, crash logs, and diagnostic captures are stored in:

`%LOCALAPPDATA%\AVOPSEMU\Logs`

Important files include:

- `session.log`
- `crash.log`
- `capture.log`

The crash handler records a fixed diagnostic entry, flushes it, and forwards the exception to Windows.

Diagnostic information can include:

- Exception code.
- Fault address.
- Loaded module information.
- Thread ID.
- Executable SHA-256.
- Host instruction pointer.
- Last known emulated CPU PC and MIPS opcode.

A complete call stack requires a WER or ProcDump memory dump.

### Automatic Capture

Use:

**File → Automatic Capture to AppData**

The application saves:

- Diagnostic logs.
- Raw VRAM.
- VRAM image.
- Framebuffer image.

Files are stored in:

`%LOCALAPPDATA%\AVOPSEMU\Captures`

The application reports success only after checking file sizes and reading the saved contents.

### Windows Error Reporting LocalDumps

To enable LocalDumps specifically for `AVOPSEMU.exe`, run PowerShell as Administrator:

```powershell
$dumpDir = Join-Path $env:LOCALAPPDATA 'AVOPSEMU\Logs\Dumps'
New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null
$key = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\AVOPSEMU.exe'
New-Item -Force -Path $key | Out-Null
New-ItemProperty -Force -Path $key -Name DumpFolder -PropertyType ExpandString -Value $dumpDir | Out-Null
New-ItemProperty -Force -Path $key -Name DumpCount -PropertyType DWord -Value 10 | Out-Null
New-ItemProperty -Force -Path $key -Name DumpType -PropertyType DWord -Value 2 | Out-Null
```

`DumpType=2` creates a full user-mode dump. Dumps may contain BIOS data, game RAM, and session information. Protect the dump directory accordingly.

To remove the configuration:

```powershell
Remove-Item -Recurse -Force $key
```

Microsoft documentation:

https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps

### ProcDump

To monitor a Release execution with ProcDump from Sysinternals:

```powershell
.\procdump64.exe -accepteula -e -ma -w AVOPSEMU.exe "$dumpDir"
```

- `-e`: Captures unhandled exceptions.
- `-ma`: Requests a full dump.
- `-w`: Waits for the target process to start.

Avoid configuring competing automatic debugging mechanisms when WER LocalDumps collection is required.

Additional documentation:

- https://learn.microsoft.com/en-us/sysinternals/downloads/procdump
- https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump

---

# Roadmap

AVOPSEMU PS1 is an ongoing emulator development project.

Future development priorities include:

- Improving CPU execution performance.
- Investigating CD-ROM FIFO and sector processing.
- Improving DMA transfer behavior and synchronization.
- Investigating audio playback and host buffering.
- Optimizing GPU and frontend rendering.
- Expanding PlayStation game compatibility.
- Improving hardware timing accuracy.
- Expanding diagnostics and automated validation.
- Improving Windows frontend usability.

The project prioritizes measurable improvements and correctness over unsupported feature claims.

---

# Project Resources

- **Repository:** https://github.com/nguyenthithinhbl88-code/AVOPSEMU_PS1
- **Source Code:** https://github.com/nguyenthithinhbl88-code/AVOPSEMU_PS1
- **Performance Update:** `PS1_PERFORMANCE_CPU_UPDATE_V51.md`
- **Benchmark Results:** `BENCHMARK_V51.txt`
- **Validation Results:** `VALIDATION_V51.txt`
- **Visual Studio Guide:** `OPEN_IN_VISUAL_STUDIO.md`
- **Frontend Documentation:** `AVOPSEMU_UI_V45.md`

## License

MIT License.

---

# About AVOPSEMU

AVOPSEMU is an independent emulator development project focused on recreating classic PlayStation hardware through software engineering, performance optimization, and compatibility research.

**From BIOS initialization to commercial game execution — AVOPSEMU PS1 continues to evolve.**

### Search Keywords

AVOPSEMU, AVOPSEMU PS1, AVOPSEMU_PS1, PlayStation 1 Emulator, PlayStation Emulator, PS1 Emulator, PSX Emulator, Sony PlayStation Emulator, C++ PS1 Emulator, Windows PS1 Emulator, PlayStation 1 Emulation, PSX Emulation, FIFA Football 2005 PS1, FIFA 2005 Emulator, MIPS R3000A Emulator, PlayStation GPU, PlayStation CD-ROM, PlayStation SPU, Retro Gaming, Open Source Emulator, Experimental Emulator.
