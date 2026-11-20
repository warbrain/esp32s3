#if HMI_TEST_NEW_SDK
extern "C"
{
// Keep an imported zero-filled export beyond the section raw-data tail.
__declspec(dllexport) int ggml_fixture_zero_padding[65536]{};
__declspec(dllexport) int ggml_fixture_zero_filled{};
}
extern "C" __declspec(dllexport) int ggml_fixture_required() { return 22; }
#else
extern "C" __declspec(dllexport) int ggml_fixture_legacy() { return 11; }
#endif
