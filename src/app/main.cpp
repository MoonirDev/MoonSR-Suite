// MoonSR application entry point: single instance, log setup, common controls, message loop.
#include "main_window.h"
#include "settings.h"

#include "core/log.h"

#include <windows.h>

namespace {

constexpr wchar_t kMutexName[] = L"Local\\MoonSR.SingleInstance";
constexpr wchar_t kTestMutexName[] = L"Local\\MoonSR.SingleInstance.Test";

void ActivateExistingInstance() {
    HWND existing = FindWindowW(moonsr::app::MainWindow::kClassName, nullptr);
    if (!existing) return;
    if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
    SetForegroundWindow(existing);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    const moonsr::app::TestOptions test = moonsr::app::ParseTestOptions();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, test.active ? kTestMutexName : kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance();
        CloseHandle(mutex);
        return 0;
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
