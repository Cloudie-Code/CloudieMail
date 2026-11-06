#define wWinMain cloudiemail_gui_entrypoint
#include "../src/platform/windows/gui_windows.cpp"
#undef wWinMain
#include <fstream>
#include <iostream>
void snapshot(HWND window, const std::filesystem::path &path) {
    RECT rect{};
    GetClientRect(window, &rect);
    int width = rect.right, height = rect.bottom;
    HDC screen = GetDC(window), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    auto old = SelectObject(memory, bitmap);
    if (!PrintWindow(window, memory, PW_CLIENTONLY))
        throw std::runtime_error("PrintWindow failed");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<char> pixels(static_cast<std::size_t>(width) * height * 4);
    SelectObject(memory, old);
    if (!GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS))
        throw std::runtime_error("GetDIBits failed");
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    output.write(reinterpret_cast<const char *>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, screen);
}
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    auto output = cm::utf8_path(argv[2]);
    std::string failure;
    std::jthread watcher([&] {
        HWND window = nullptr;
        for (int i = 0; i < 100 && !window; ++i) {
            Sleep(100);
            window = FindWindowW(L"CloudieMailWindow", nullptr);
        }
        if (!window) {
            failure = "Window missing";
            return;
        }
        try {
            Sleep(1000);
            snapshot(window, output / "windows-domain.bmp");
            SendMessageW(window, WM_COMMAND, 405, 0);
            Sleep(500);
            snapshot(window, output / "windows-settings.bmp");
        } catch (const std::exception &e) {
            failure = e.what();
        }
        PostMessageW(window, WM_COMMAND, 4, 0);
    });
    auto result = cloudiemail_gui_entrypoint(GetModuleHandleW(nullptr), nullptr, nullptr, SW_SHOW);
    watcher.join();
    if (!failure.empty()) {
        std::cerr << failure;
        return 1;
    }
    std::cout << "Windows GUI controls and snapshots passed\n";
    return result;
}
