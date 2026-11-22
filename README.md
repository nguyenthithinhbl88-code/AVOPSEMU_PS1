# AVOPSEMU PS1 v51

Dự án giả lập PlayStation viết bằng C++17, giao diện Win32 cho Visual Studio 2022. Tên trước đây: PSLab. Nhánh PS2 đã chuyển thành dự án AVOPSEMU_PS2 độc lập; không biên dịch hoặc liên kết PS2 trong dự án này.

## Cập nhật lõi v51

- CPU: hoàn thiện COP0 TAR, CAUSE.BT, mặt nạ DCIC và breakpoint BPC/BPCM với vector debug riêng; giảm lấy mẫu ngắt trùng khi thiết bị không thay đổi.
- GPU: giảm tính toán địa chỉ texture/CLUT lặp trong sprite lớn, vẫn đọc VRAM trực tiếp để giữ đúng thứ tự khi texture và framebuffer chồng nhau.
- Âm thanh: tối ưu tìm điểm ghép WSOLA bằng tổng năng lượng tích lũy; giữ nguyên mẫu PCM và nhịp SPU.
- Chi tiết phạm vi, đo tốc độ và giới hạn: `PS1_PERFORMANCE_CPU_UPDATE_V51.md`, `BENCHMARK_V51.txt`, `VALIDATION_V51.txt`.
- Nhánh PS2 v50 không thay đổi. Chưa xác nhận FPS game thật hoặc chất lượng thiết bị âm thanh Windows của bản này.

## Tách dự án v46

Mở riêng AVOPSEMU_PS1 để build PS1. Mã PS2, kiểm thử, ELF demo và tài liệu PS2 được chuyển sang AVOPSEMU_PS2; lõi PS2 được giữ nguyên. Không có CMake cha bắt buộc và không cần đặt hai thư mục cạnh nhau.

## Frontend Windows

- GUI Win32/GDI có Settings Center, Game Library và vùng hiển thị 4:3. F5 chạy/tạm dừng, F9 bật/tắt debugger hiện có, F11 đổi chế độ rạp.
- Settings Center lưu tại `%LOCALAPPDATA%\AVOPSEMU\settings.ini`, có schema/validation và migration cấu hình bàn phím cũ. Theme Dark/Light/AMOLED, accent, font, toolbar/status/FPS HUD/sidebar/debugger, kích thước cửa sổ, phím, mute, pacing và BIOS path được nối với frontend/runtime.
- Các trang CPU/GPU/CD-ROM/Audio/Timing chỉ hiển thị capability thật. Dynarec, internal resolution, texture filtering, drive-speed, sample-rate/latency, VSync và các override timing chưa được core/backend hỗ trợ nên không có nút giả.
- Game Library lưu tại `%LOCALAPPDATA%\AVOPSEMU\library.tsv`; hỗ trợ import CUE/BIN/ISO, tìm kiếm/sắp xếp, metadata `SYSTEM.CNF`, cover PNG/JPEG/BMP, mount/boot và cấu hình host per-game giới hạn ở modeled 1x/mute.
- Playtime được lưu theo phiên; chỉ cộng khi đĩa game đã boot và Runner thực sự chạy, không cộng lúc pause/stop. BIOS-only và EXE homebrew không được tính là phiên game.
- Mapping hiện là keyboard cho digital pad. Gamepad/analog/rung, profile controller per-game, dockable panels, animation/language settings và debugger studio đầy đủ (memory hex editor, execution trace, GPU command monitor, v.v.) chưa được triển khai.
- Tệp GUI là **AVOPSEMU.exe**; target CMake nội bộ vẫn là `pslab` để giữ tương thích cấu hình build.

## Modern UI WinUI 3

- `modern_ui/AVOPSEMU.ModernUI.vcxproj` là ứng dụng desktop WinUI 3/C++/WinRT độc lập, không thay thế frontend Win32/GDI và không liên kết với emulator core.
- Giao diện có Dashboard, thư viện, chi tiết game, Settings và About. Thư viện chỉ đọc `library.tsv` hiện có; tìm kiếm, lọc, sắp xếp và xem metadata/cover hoạt động trong phạm vi dữ liệu đó.
- Refresh chỉ đọc lại metadata. Lịch sử chơi, trạng thái chạy/FPS, nạp/mount đĩa, Play và các thiết lập core chưa được nối; các khả năng chưa có được ghi là Prototype/Not connected, không mô phỏng trạng thái.
- Build riêng bằng `modern_ui/AVOPSEMU.ModernUI.vcxproj` với cấu hình Release|x64. Phụ thuộc WinUI hiện hữu được khai báo trong project; không cần thêm package cho giao diện.

## Build và chạy

1. Cài Visual Studio 2022 hoặc mới hơn với **Desktop development with C++**, **C++ CMake tools for Windows** và Windows SDK.
2. Giải nén, mở thư mục `AVOPSEMU_PS1` chứa `CMakeLists.txt` bằng **File → Open → Folder**.
3. Chọn preset **windows-release**, Startup Item **AVOPSEMU.exe** (target `pslab`), Ctrl+F5.
4. Nạp BIOS PS1 512 KiB qua **Settings Center → BIOS** hoặc **Tệp → Nạp BIOS**. BIOS không được phân phối kèm dự án.
5. Mở Game Library, import `.cue` / `.bin` / `.iso`, rồi chọn **Mount disc** hoặc **Boot / run**. Có thể dùng thanh công cụ để boot đĩa đã mount.
6. Với BIN/CUE nhiều track, chọn `.cue` và giữ tất cả BIN trong cùng thư mục theo đường dẫn trong CUE.

Không kèm BIOS hoặc game. Người dùng cung cấp tệp của mình. Homebrew EXE trực tiếp không thay thế quá trình BIOS khởi tạo cho game thương mại.

Cấu hình người dùng nằm trong `%LOCALAPPDATA%\AVOPSEMU` (settings, thư viện và log/capture); không còn ghi cấu hình chính cạnh EXE.

## Điều khiển mặc định

| Phím | Nút PS1 |
|---|---|
| Mũi tên | D-pad |
| Z / X / A / S | Cross / Circle / Square / Triangle |
| Enter / Shift | Start / Select |
| Q / W | L1 / R1 |
| E / R | L2 / R2 |

Bấm vào cửa sổ game trước khi chơi. Menu **Hiển thị** có preset WASD và thử giữ Cross/Start 1 giây. Alt-Tab nhả phím. Đây là digital pad, chưa có analog/rung.

## Trạng thái và kiểm tra

- V51 cập nhật lõi CPU/GPU và WSOLA trên nền v46. Lịch sử đo/kiểm thử lõi nằm trong `PS1_PERFORMANCE_CPU_UPDATE_V51.md`, `BENCHMARK_V51.txt` và `VALIDATION_V51.txt`; không xem benchmark tổng hợp là FPS game.
- Windows Release đã được build và chạy trong GUI. BIOS SCPH-1001 do người dùng cung cấp được nạp/kiểm tra đúng kích thước và thực tế tiến tới BIOS Main Menu; keyboard input được gửi khi BIOS đang chạy.
- Game Library đã được thử bằng GTA 2 CUE: import, lưu thư viện, nhận diện `SLUS_007.89`/USA từ `SYSTEM.CNF` và mount đĩa thành công. Việc này không đồng nghĩa đã xác minh gameplay GTA 2 trong lượt frontend này.
- Full Windows Release CTest: **51/54 PASS**. `cdda_tests`, `disc_tests` và `xa_audio_tests` thất bại khi `std::filesystem::remove_all` dọn thư mục tạm vì file vẫn bị process giữ mở; cùng lỗi cleanup này đã xuất hiện ở lượt baseline trước frontend. `frontend_preferences_tests` và `frontend_library_tests` đều PASS.
- Chỉ số Hz trên giao diện là VBlank mô hình; `FPS GDI` là nhịp gửi frame tới đường GDI, không phải phép đo refresh/FPS vật lý của màn hình.

Xem `OPEN_IN_VISUAL_STUDIO.md`, `AVOPSEMU_UI_V45.md`, `VALIDATION_V45.txt`. Các tài liệu v44 trở về trước giữ tên PSLab như lịch sử dự án.

## Crash diagnostics và capture trên Windows

- Log phiên, lỗi và capture được ghi vào `%LOCALAPPDATA%\AVOPSEMU\Logs` (`session.log`, `crash.log`, `capture.log`). `crash.log` được mở sẵn để exception handler chỉ ghi một bản ghi cố định, flush rồi chuyển tiếp cho Windows; không chạy DbgHelp trong process đang lỗi.
- Exception record gồm mã/lỗi, fault address, module đã nạp, thread ID, SHA-256 của executable, host instruction pointer và CPU PC/opcode MIPS gần nhất nếu thread đã chạy CPU. Call stack đầy đủ cần đọc từ WER/ProcDump dump.
- Menu **Tệp → Chụp tự động vào AppData** lưu log, raw VRAM, ảnh VRAM và framebuffer vào `%LOCALAPPDATA%\AVOPSEMU\Captures` mà không mở Save dialog. Ứng dụng chỉ báo thành công sau khi kiểm tra kích thước và đọc lại nội dung từng file.

Để bật WER LocalDumps **chỉ cho `AVOPSEMU.exe`**, chạy PowerShell với quyền Administrator:

```powershell
$dumpDir = Join-Path $env:LOCALAPPDATA 'AVOPSEMU\Logs\Dumps'
New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null
$key = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\AVOPSEMU.exe'
New-Item -Force -Path $key | Out-Null
New-ItemProperty -Force -Path $key -Name DumpFolder -PropertyType ExpandString -Value $dumpDir | Out-Null
New-ItemProperty -Force -Path $key -Name DumpCount -PropertyType DWord -Value 10 | Out-Null
New-ItemProperty -Force -Path $key -Name DumpType -PropertyType DWord -Value 2 | Out-Null
```

`DumpType=2` tạo full user-mode dump; dump có thể chứa BIOS, RAM game và dữ liệu phiên, vì vậy hãy bảo vệ thư mục. Có thể gỡ cấu hình bằng `Remove-Item -Recurse -Force $key`. Tài liệu Microsoft: [Collecting User-Mode Dumps](https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps).

Để theo dõi một lần chạy Release bằng ProcDump từ Sysinternals:

```powershell
.\procdump64.exe -accepteula -e -ma -w AVOPSEMU.exe "$dumpDir"
```

`-e` bắt unhandled exception, `-ma` yêu cầu full dump và `-w` chờ process khởi chạy. Không bật tự động debugging cùng LocalDumps nếu cần WER thu dump; xem thêm [ProcDump](https://learn.microsoft.com/en-us/sysinternals/downloads/procdump) và [MiniDumpWriteDump](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump).
