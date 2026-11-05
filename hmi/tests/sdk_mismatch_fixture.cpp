#include <windows.h>
extern "C" __declspec(dllimport) int ggml_fixture_required();
extern "C" __declspec(dllimport) int ggml_fixture_zero_filled;
extern "C" __declspec(dllexport) int hmi_test_sdk_preflight_version() { return ggml_fixture_required() + ggml_fixture_zero_filled; }
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        wchar_t marker[32768]{};
        if (GetEnvironmentVariableW(L"HMI_TEST_SDK_MARKER", marker, 32768))
        {
            const HANDLE file = CreateFileW(marker, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
    }
    return TRUE;
}
