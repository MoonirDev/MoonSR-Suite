// MoonSR application entry point: single instance, log setup, common controls, message loop.
#include "main_window.h"
#include "nr_import.h"
#include "settings.h"
#include "ui/theme_file.h"

#include <cstdio>
#include <string>

#include "core/log.h"

#include <windows.h>
#include <shellapi.h>

#include <cwchar>

namespace {

constexpr wchar_t kMutexName[] = L"Local\\MoonSR.SingleInstance";
constexpr wchar_t kTestMutexName[] = L"Local\\MoonSR.SingleInstance.Test";

void ActivateExistingInstance() {
    HWND existing = FindWindowW(moonsr::app::MainWindow::kClassName, nullptr);
    if (!existing) return;
    if (!IsWindowVisible(existing)) {  // hidden in the notification area: ask it to restore itself
        SendMessageTimeoutW(existing, moonsr::app::WM_APP_RESTORE, 0, 0, SMTO_ABORTIFHUNG, 2000, nullptr);
        return;
    }
    if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
    SetForegroundWindow(existing);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    {
        // Load check for an imported neural runtime (nr_import.cpp, ProbeLoad): a separate process, so a runtime that
        // crashes or hangs while loading cannot take the UI down. No window, no log, no single-instance mutex.
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc == 4 && wcscmp(argv[1], L"--nr-probe-load") == 0) {
            const int code = moonsr::app::nr::RunProbeChild(argv[2], argv[3]);
            LocalFree(argv);
            ExitProcess(static_cast<UINT>(code));
        }
        if (argv) LocalFree(argv);
    }
    const moonsr::app::TestOptions test = moonsr::app::ParseTestOptions();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, test.active ? kTestMutexName : kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance();
        CloseHandle(mutex);
        return 0;
    }

    if (!test.themeSelfTest.empty()) {
        // Parser / catalog tests: scratch folder under %TEMP%, report written to the given file, exit code = failed checks.
        wchar_t tmp[MAX_PATH] = {};
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        const std::wstring scratch = (n > 0 && n < MAX_PATH ? std::wstring(tmp) : std::wstring(L".\\")) + L"MoonSR-theme-selftest";
        std::wstring report;
        const int failed = moonsr::app::ui::RunThemeSelfTest(scratch, report);
        report += failed == 0 ? L"\nALL CHECKS PASSED\n" : L"\nFAILED CHECKS: " + std::to_wstring(failed) + L"\n";
        FILE* f = nullptr;
        if (_wfopen_s(&f, test.themeSelfTest.c_str(), L"wb") == 0 && f) {
            const unsigned char bom[2] = {0xFF, 0xFE};
            fwrite(bom, 1, 2, f);
            fwrite(report.data(), sizeof(wchar_t), report.size(), f);
            fclose(f);
        }
        return failed == 0 ? 0 : 1;
    }

    const std::wstring dir = moonsr::app::AppDataDirectory();
    if (!dir.empty()) moonsr::InitLog(dir + L"\\moonsr.log");
    MSR_LOG_INFO(L"MoonSR UI starting");

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 0;
    if (!moonsr::app::MainWindow::Register(instance)) {
        MSR_LOG_ERROR(L"Window class registration failed ({})", GetLastError());
        exitCode = 1;
    } else {
        moonsr::app::MainWindow window;
        if (!window.Create(instance, showCmd, test)) {
            MSR_LOG_ERROR(L"Main window creation failed ({})", GetLastError());
            exitCode = 1;
        } else {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }
    }

    if (SUCCEEDED(coHr)) CoUninitialize();
    MSR_LOG_INFO(L"MoonSR UI exiting ({})", exitCode);
    if (mutex) CloseHandle(mutex);
    return exitCode;
}
