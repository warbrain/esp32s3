extern "C" __declspec(dllimport) int hmi_test_sdk_dependency_version();

extern "C" __declspec(dllexport) int hmi_test_sdk_fixture_version()
{
    return hmi_test_sdk_dependency_version();
}
