#include "kamite/launcher_platform.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <winhttp.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#endif

namespace kamite::launcher::platform
{
    std::filesystem::path pick(void *window, bool folder)
    {
#if defined(_WIN32)
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::filesystem::path result;
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
            if (folder)
                dialog->SetTitle(L"Choose the texture pack folder (the one holding replacements)");
            else
            {
                const COMDLG_FILTERSPEC filters[] = {{L"PS2 disc image (*.iso, *.chd)", L"*.iso;*.chd"}, {L"All files", L"*.*"}};
                dialog->SetFileTypes(2, filters);
                dialog->SetTitle(L"Choose your disc image (USA, SLUS-21503)");
            }
            // A borderless window covering the whole monitor has its frames shown straight on screen, without the
            // desktop's composition, so the dialog opened over it stays invisible until something (the Start key)
            // brings composition back (the same cause as the Nov 17 black screen after Play). One pixel off the
            // bottom while the dialog is up gives composition back; the old size returns after it.
            const HWND owner = static_cast<HWND>(window);
            RECT kept{};
            bool shrunk = false;
            if (owner != nullptr && GetWindowRect(owner, &kept))
            {
                MONITORINFO monitor{};
                monitor.cbSize = sizeof(monitor);
                const RECT &m = monitor.rcMonitor;
                if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor) && kept.left <= m.left &&
                    kept.top <= m.top && kept.right >= m.right && kept.bottom >= m.bottom)
                {
                    shrunk = SetWindowPos(owner, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top - 1,
                                          SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
                }
            }
            const bool chosen = SUCCEEDED(dialog->Show(owner));
            if (shrunk)
                SetWindowPos(owner, nullptr, kept.left, kept.top, kept.right - kept.left, kept.bottom - kept.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            if (chosen)
            {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        result = std::filesystem::path(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        return result;
#else
        (void)window;
        (void)folder;
        return {};
#endif
    }

    void open(const std::filesystem::path &path)
    {
#if defined(_WIN32)
        ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        (void)path;
#endif
    }

    bool shiftHeld()
    {
#if defined(_WIN32)
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
        return false;
#endif
    }

#if defined(_WIN32)
    namespace
    {
        std::wstring wide(const std::string &text)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
            return out;
        }
    }
#endif

    bool httpGet(const std::string &url, std::vector<uint8_t> &out, size_t maxBytes, const char *accept,
                 std::atomic<uint64_t> *received)
    {
        out.clear();
#if defined(_WIN32)
        const std::wstring address = wide(url);
        wchar_t host[256] = {}, path[2048] = {};
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts))
            return false;
        // The agent name is the User-Agent GitHub's API asks for.
        HINTERNET session = WinHttpOpen(L"GHR", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) // before Windows 8.1
            session = WinHttpOpen(L"GHR", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr)
            return false;
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
        const std::wstring headers = accept ? L"Accept: " + wide(accept) + L"\r\n" : std::wstring();
        bool ok = false;
        if (HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0))
        {
            if (HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0))
            {
                DWORD status = 0, size = sizeof(status);
                if (WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                       headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(request, nullptr) &&
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                        &size, WINHTTP_NO_HEADER_INDEX) &&
                    status == 200)
                {
                    ok = true;
                    for (;;)
                    {
                        DWORD available = 0, read = 0;
                        if (!WinHttpQueryDataAvailable(request, &available))
                        {
                            ok = false;
                            break;
                        }
                        if (available == 0)
                            break;
                        const size_t at = out.size();
                        if (at + available > maxBytes)
                        {
                            ok = false;
                            break;
                        }
                        out.resize(at + available);
                        if (!WinHttpReadData(request, out.data() + at, available, &read))
                        {
                            ok = false;
                            break;
                        }
                        out.resize(at + read);
                        if (received)
                            received->store(out.size(), std::memory_order_relaxed);
                    }
                }
                WinHttpCloseHandle(request);
            }
            WinHttpCloseHandle(connection);
        }
        WinHttpCloseHandle(session);
        return ok;
#else
        (void)url;
        (void)maxBytes;
        (void)accept;
        (void)received;
        return false;
#endif
    }

    std::filesystem::path thisExe()
    {
#if defined(_WIN32)
        std::wstring path(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(n);
        return std::filesystem::path(path);
#else
        return {};
#endif
    }

    bool relaunch(const std::filesystem::path &exe)
    {
#if defined(_WIN32)
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(),
                            &startup, &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
#else
        (void)exe;
        return false;
#endif
    }
}
