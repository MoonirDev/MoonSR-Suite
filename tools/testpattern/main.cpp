// moonsr_testpattern: a stand-in "game" window for pipeline tests. Renders an animated pattern with GDI at a
// requested client size and frame rate: moving bars (motion), a fine checker and 1-pixel lines (detail),
// static text (HUD stand-in) and a frame counter.
//
//   moonsr_testpattern [--width 1280] [--height 720] [--fps 60] [--seconds 0] [--x 100] [--y 100]
//                      [--motion-pattern 0|1|2] (normal, slow pan, fast pan)
//
// --seconds 0 runs until the window is closed.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

int g_width = 1280, g_height = 720;
double g_fps = 60.0;
uint64_t g_frame = 0;
int g_motionPattern = 0;

void Paint(HDC dc) {
    RECT rc{0, 0, g_width, g_height};
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    if (g_motionPattern) {
        // Broad, continuously translating texture for motion and temporal-reconstruction tests.
        const int shift = static_cast<int>((g_frame * (g_motionPattern == 2 ? 10u : 2u)) % 192u);
        for (int y = -96; y < g_height + 96; y += 48) {
            for (int x = -192; x < g_width + 192; x += 48) {
                const int index = (((x + 192) / 48 + (y + 96) / 48 * 3) & 3) * 4;
                RECT tile{x + shift - 96, y, x + shift - 48, y + 48};
                HBRUSH brush = CreateSolidBrush(RGB(32 + index * 12, 210 - index * 8, 45 + index * 10));
                FillRect(dc, &tile, brush);
                DeleteObject(brush);
            }
        }
        return;
    }

    // Gradient background strips.
    for (int i = 0; i < 16; ++i) {
        RECT s{i * g_width / 16, 0, (i + 1) * g_width / 16, g_height / 6};
        HBRUSH b = CreateSolidBrush(RGB(i * 16, 255 - i * 16, 128));
        FillRect(dc, &s, b);
        DeleteObject(b);
    }

    // Fine detail: 1-pixel checker block and 1-pixel diagonal lines.
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 96; ++x)
            if ((x ^ y) & 1) SetPixelV(dc, 40 + x, g_height / 4 + y, RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 0));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    for (int i = 0; i < 12; ++i) {
        MoveToEx(dc, 180, g_height / 4 + i * 8, nullptr);
        LineTo(dc, 180 + 300, g_height / 4 + i * 8 + 10 + i * 6);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    // Moving bars at different speeds and directions.
    const int t = static_cast<int>(g_frame);
    const struct { int speed; int y; COLORREF c; } bars[] = {
        {4, g_height / 2, RGB(255, 64, 64)}, {-7, g_height / 2 + 60, RGB(64, 255, 64)}, {13, g_height / 2 + 120, RGB(64, 128, 255)}};
    for (const auto& b : bars) {
        int x = ((t * b.speed) % (g_width + 80) + (g_width + 80)) % (g_width + 80) - 80;
        RECT r{x, b.y, x + 80, b.y + 40};
        HBRUSH br = CreateSolidBrush(b.c);
        FillRect(dc, &r, br);
        DeleteObject(br);
    }

    // Static HUD-like text and the frame counter.
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    wchar_t text[128];
    swprintf_s(text, L"MoonSR test pattern  %dx%d @ %.0f fps", g_width, g_height, g_fps);
    TextOutW(dc, 20, g_height - 60, text, static_cast<int>(wcslen(text)));
    swprintf_s(text, L"frame %llu", g_frame);
    TextOutW(dc, 20, g_height - 36, text, static_cast<int>(wcslen(text)));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    double seconds = 0.0;
    int posX = 100, posY = 100;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring a = argv[i];
        if (a == L"--width") g_width = _wtoi(argv[i + 1]);
        else if (a == L"--height") g_height = _wtoi(argv[i + 1]);
        else if (a == L"--fps") g_fps = _wtof(argv[i + 1]);
        else if (a == L"--seconds") seconds = _wtof(argv[i + 1]);
        else if (a == L"--motion-pattern") g_motionPattern = _wtoi(argv[i + 1]);
        else if (a == L"--x") posX = _wtoi(argv[i + 1]);
        else if (a == L"--y") posY = _wtoi(argv[i + 1]);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MoonSRTestPattern";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr{0, 0, g_width, g_height};
    AdjustWindowRect(&wr, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MoonSR Test Pattern", style, posX, posY, wr.right - wr.left,
                              wr.bottom - wr.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    HDC wdc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, g_width, g_height);
    SelectObject(mem, bmp);

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Paint(mem);
        BitBlt(wdc, 0, 0, g_width, g_height, mem, 0, 0, SRCCOPY);
        ++g_frame;

        // Sleep until the next frame boundary measured from the start (no drift).
        QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
        if (seconds > 0.0 && elapsed >= seconds) break;
        const double wait = static_cast<double>(g_frame) / g_fps - elapsed;
        if (wait > 0.0) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(wait * 1e7);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
        }
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, wdc);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    return 0;
}
