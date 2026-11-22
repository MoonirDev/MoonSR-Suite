// Enumeration of top-level windows that are reasonable capture targets.
//
// Threading: EnumerateCaptureWindows() is stateless and callable from any thread. It temporarily switches the calling
// thread to per-monitor-v2 DPI awareness so client sizes are physical pixels.
#include "capture/capture_source.h"

#include "core/log.h"
#include "gpu/dpi_scope.h"

#include <dwmapi.h>

#include <algorithm>
#include <iterator>

namespace moonsr {

namespace {

struct EnumState {
    std::vector<WindowInfo>* out;
    DWORD selfPid;
    HWND shell;
};

std::wstring ExeNameOf(DWORD pid) {
    std::wstring name;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return name;
    wchar_t path[MAX_PATH * 2];
    DWORD len = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &len)) {
        std::wstring_view full(path, len);
        const size_t slash = full.find_last_of(L"\\/");
        name = full.substr(slash == std::wstring_view::npos ? 0 : slash + 1);
    }
    CloseHandle(process);
    return name;
}

BOOL CALLBACK VisitWindow(HWND hwnd, LPARAM param) {
    auto& st = *reinterpret_cast<EnumState*>(param);
    if (hwnd == st.shell || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == st.selfPid) return TRUE;

    const int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen <= 0) return TRUE;
    std::wstring title(static_cast<size_t>(titleLen) + 1, L'\0');
    const int got = GetWindowTextW(hwnd, title.data(), titleLen + 1);
    if (got <= 0) return TRUE;
    title.resize(static_cast<size_t>(got));

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    const Size client{static_cast<uint32_t>(std::max<LONG>(0, rc.right - rc.left)),
                      static_cast<uint32_t>(std::max<LONG>(0, rc.bottom - rc.top))};
    if (client.Empty()) return TRUE;

    WindowInfo w;
    w.hwnd = hwnd;
    w.title = std::move(title);
    w.processId = pid;
    w.exeName = ExeNameOf(pid);
    w.clientSize = client;
    st.out->push_back(std::move(w));
    return TRUE;
}

} // namespace

std::vector<WindowInfo> EnumerateCaptureWindows() {
    ScopedPerMonitorDpi dpi;
    std::vector<WindowInfo> result;
    EnumState st{&result, GetCurrentProcessId(), GetShellWindow()};
    if (!EnumWindows(VisitWindow, reinterpret_cast<LPARAM>(&st)))
        MSR_LOG_WARN(L"EnumerateCaptureWindows: EnumWindows failed: {}", HrToString(HRESULT_FROM_WIN32(GetLastError())));
    return result;
}

} // namespace moonsr
