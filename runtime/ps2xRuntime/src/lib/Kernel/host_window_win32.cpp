// The game window's hand-off from a launcher (ps2_runtime.cpp openHostWindow). Kept here so <windows.h> stays out of
// ps2_runtime.cpp (raylib's names clash with it).
//
// Why the window is replaced: a launcher that drew into the window with OpenGL while it filled its monitor (borderless
// fullscreen) leaves it as a swap chain Windows shows straight on the screen, skipping composition. The game then shows
// its picture in PCSX2's GS child window and the host window stops presenting, so the launcher's last frame stayed on
// screen until something forced composition (the Start menu, F11). Started without the launcher, the window never
// presents and the picture shows (K, Nov 17). Wine (Proton, the Steam Deck) has no such composition step: the window is
// kept there, as before.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
    HWND g_cover = nullptr;
    constexpr wchar_t kCoverClass[] = L"PS2XHandoffCover";
}

// True on Windows itself, false under Wine (ntdll exports wine_get_version there).
bool ps2xHostReplacesLauncherWindow() noexcept
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return !(ntdll && GetProcAddress(ntdll, "wine_get_version"));
}

// A plain black window (GDI, no swap chain) over the monitor the given window is on, so the screen stays black while
// the window is closed and made again (~0.1 s) instead of showing the desktop. Never activated.
void ps2xHostHandoffCoverShow(void *window) noexcept
{
    if (g_cover)
        return;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.hCursor = nullptr;
        wc.lpszClassName = kCoverClass;
        registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (!registered)
            return;
    }
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(window), MONITOR_DEFAULTTONEAREST), &info))
        return;
    const RECT &r = info.rcMonitor;
    g_cover = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kCoverClass, L"", WS_POPUP,
                              r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
    if (!g_cover)
        return;
    ShowWindow(g_cover, SW_SHOWNOACTIVATE);
    UpdateWindow(g_cover); // painted black now (the class brush), before the game window goes
}

void ps2xHostHandoffCoverHide() noexcept
{
    if (!g_cover)
        return;
    DestroyWindow(g_cover);
    g_cover = nullptr;
}
#endif
