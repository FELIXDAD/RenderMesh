#include <windows.h>

#include <fstream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    wchar_t marker[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"RM_UPDATE_TEST_MARKER", marker, static_cast<DWORD>(std::size(marker)));
    if (length > 0 && length < std::size(marker)) {
        std::ofstream output(marker, std::ios::binary | std::ios::trunc);
#ifdef UPDATE_STUB_OLD
        output << "old-started";
#else
        output << "started";
#endif
        output.flush();
    }
    const bool skipHealth = GetEnvironmentVariableW(
        L"RM_UPDATE_TEST_SKIP_HEALTH", nullptr, 0) > 0;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wstring(argv[index]) != L"--update-health-event") continue;
        if (skipHealth) break;
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[index + 1]);
        if (!event) return 3;
        const BOOL signalled = SetEvent(event);
        CloseHandle(event);
        if (!signalled) return 4;
        break;
    }
    Sleep(6500);
    return 0;
}
