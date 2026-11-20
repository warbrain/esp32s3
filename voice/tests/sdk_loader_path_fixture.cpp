extern "C" __declspec(dllimport) int hmi_test_external_runtime_version();

extern "C" __declspec(dllexport) int hmi_test_path_fixture_version()
{
    return hmi_test_external_runtime_version();
}
