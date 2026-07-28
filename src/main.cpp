#include "ODMApp.h"

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#endif

int main() {
#if defined(_WIN32)
    // Single instance: the bridge server owns 127.0.0.1:47923, so a second
    // copy must not run — hand-offs always reach the one running instance.
    HANDLE inst_mtx =
        CreateMutexW(nullptr, TRUE, L"Local\\ODM.SingleInstance.Bridge");
    if (inst_mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
                    L"ODM is already running. Check the taskbar or System tray.",
                    L"ODM", MB_OK | MB_ICONINFORMATION);
        CloseHandle(inst_mtx);
        return 0;
    }
#endif

    odm::ODMApp app;
    app.Run();

#if defined(_WIN32)
    if (inst_mtx) CloseHandle(inst_mtx);
#endif
    return 0;
}
