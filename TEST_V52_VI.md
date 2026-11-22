# Test PS2 v52 trên Visual Studio

1. Giải nén vào thư mục mới, mở riêng AVOPSEMU_PS2 chứa CMakeLists.txt. Gói này không chứa thư mục build/cache/EXE Windows có sẵn.
2. Chọn Windows x64 Release (windows-release), chờ CMake cấu hình rồi Build All. Startup Item AVOPSEMU_PS2.exe, Ctrl+F5.
3. Nếu dùng lại thư mục cũ và CMake báo source mismatch, đóng Visual Studio, xóa build (và out nếu có) trong đúng dự án PS2 rồi mở lại; không sửa source path trong CMakeLists.txt.
4. Nạp chính BIOS đã dùng cho log (6), bấm Chạy/F5. Cửa sổ GS riêng vẫn có để quan sát.
5. Kiểm tra log phải ghi v52. Bản vá đã kiểm thử qua opcode 4B000BFF ở 8000DF08; chưa dự đoán được điểm dừng kế tiếp của BIOS thật.
6. Nếu dừng tiếp, bấm Lưu log, gửi toàn bộ TXT. Nếu chỉ đen nhưng CPU chạy, log sẽ cho biết PMODE, GIF và lịch sử GS; không kết luận chỉ từ màu màn hình.

Bản vá không thay BIOS và không kèm BIOS/game. Mục tiêu kiểm tra lần này là tiến trình khởi tạo VU1, không phải khẳng định game PS2 chạy được.
