#include "check.hpp"
#include "engine/sdk_library.hpp"
#include "engine/sdk_session.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

HMI_TEST(native_diagnostics_hide_provider_labels_without_changing_sdk_symbols)
{
    const auto message = hmi::engine::detail::native_diagnostic(
        "audio.cpp: audiocpp_registry_create failed; AUDIOCPP_DLL; AudioCpp");
    CHECK(message == "native: native_registry_create failed; native_DLL; native");
}

namespace
{
class SdkEnvironment
{
public:
    explicit SdkEnvironment(const wchar_t *name) : name_(name)
    {
        wchar_t *value = nullptr;
        std::size_t count = 0;
        if (_wdupenv_s(&value, &count, name) == 0 && value) previous_ = value;
        std::free(value);
    }
    ~SdkEnvironment() { _wputenv_s(name_.c_str(), previous_.c_str()); }
    bool set(const std::filesystem::path &value) const { return _wputenv_s(name_.c_str(), value.c_str()) == 0; }

private:
    std::wstring name_;
    std::wstring previous_;
};

struct FakeSdk
{
    std::string success_backend;
    std::vector<std::string> attempted;
    std::vector<audiocpp_session *> freed;
    std::string detail;
    bool return_failed_handles = false;
    bool null_success = false;
    bool parameters_valid = true;
    const audiocpp_options *cuda_tts_options = nullptr;
};

FakeSdk fake;

audiocpp_status create_session(const audiocpp_model *model, const char *task, const char *mode,
                              const audiocpp_backend_config *backend, const audiocpp_options *options,
                              audiocpp_session **out)
{
    fake.parameters_valid = fake.parameters_valid && model != nullptr && task != nullptr &&
        mode != nullptr && backend != nullptr && backend->threads == 7 && backend->device == 0 &&
        options == (std::string(task) == "tts" && std::string(backend->backend) == "cuda" ?
            fake.cuda_tts_options : nullptr) && *out == nullptr &&
        (!fake.return_failed_handles || fake.freed.size() == fake.attempted.size());
    const std::string candidate = backend->backend;
    fake.attempted.push_back(candidate);
    fake.detail = candidate + " unavailable";
    if (candidate == fake.success_backend)
    {
        *out = fake.null_success ? nullptr : reinterpret_cast<audiocpp_session *>(0x1234);
        return AUDIOCPP_OK;
    }
    *out = fake.return_failed_handles ? reinterpret_cast<audiocpp_session *>(
        static_cast<std::uintptr_t>(fake.attempted.size())) : nullptr;
    return AUDIOCPP_ERR_NOT_AVAILABLE;
}

void free_session(audiocpp_session *session)
{
    fake.freed.push_back(session);
    fake.detail = "cleanup replaced diagnostics";
}

const char *last_error() { return fake.detail.c_str(); }

bool open_fake(const std::string &backend, audiocpp_session **out, std::string *error,
               std::string *selected_backend = nullptr)
{
    return hmi::engine::detail::create_sdk_session({create_session, free_session, last_error},
        reinterpret_cast<audiocpp_model *>(0x1000), "asr", "streaming", backend, 7, out, error, selected_backend);
}
}

HMI_TEST(sdk_auto_retries_vulkan_after_cuda_failure)
{
    fake = {};
    fake.success_backend = "vulkan";
    audiocpp_session *session = nullptr;
    std::string error = "stale failure";
    std::string selected = "stale backend";
    CHECK(open_fake("auto", &session, &error, &selected));
    CHECK(selected == "vulkan");
    CHECK((fake.attempted == std::vector<std::string>{"cuda", "vulkan"}));
    CHECK(fake.parameters_valid);
    CHECK(fake.freed.empty());
    CHECK(session != nullptr);
    CHECK(error.empty());
}

HMI_TEST(sdk_auto_releases_failed_gpu_sessions_before_cpu)
{
    fake = {};
    fake.success_backend = "cpu";
    fake.return_failed_handles = true;
    audiocpp_session *session = nullptr;
    std::string error;
    std::string selected;
    CHECK(open_fake("auto", &session, &error, &selected));
    CHECK(selected == "cpu");
    CHECK((fake.attempted == std::vector<std::string>{"cuda", "vulkan", "cpu"}));
    CHECK(fake.freed.size() == 2);
    CHECK(std::find(fake.freed.begin(), fake.freed.end(), session) == fake.freed.end());
    CHECK(fake.parameters_valid);
}

HMI_TEST(sdk_auto_preserves_all_backend_failures_before_cleanup)
{
    fake = {};
    fake.return_failed_handles = true;
    audiocpp_session *session = reinterpret_cast<audiocpp_session *>(0x9999);
    std::string error;
    std::string selected = "old cuda";
    CHECK(!open_fake("auto", &session, &error, &selected));
    CHECK(selected.empty());
    CHECK(session == nullptr);
    CHECK(fake.freed.size() == 3);
    CHECK(error.find("cuda: cuda unavailable") != std::string::npos);
    CHECK(error.find("vulkan: vulkan unavailable") != std::string::npos);
    CHECK(error.find("cpu: cpu unavailable") != std::string::npos);
    CHECK(error.find("cleanup replaced") == std::string::npos);
}

HMI_TEST(sdk_explicit_backend_never_changes_to_another_backend)
{
    fake = {};
    fake.success_backend = "cpu";
    audiocpp_session *session = nullptr;
    std::string error;
    CHECK(!open_fake("cuda", &session, &error));
    CHECK((fake.attempted == std::vector<std::string>{"cuda"}));
    CHECK(error.find("cuda unavailable") != std::string::npos);
    CHECK(!open_fake("best", &session, &error));
    CHECK(fake.attempted.back() == "best");
    CHECK(fake.attempted.size() == 2);
}

HMI_TEST(sdk_cuda_tts_options_do_not_leak_to_asr_or_backend_fallback)
{
    fake = {};
    fake.success_backend = "cpu";
    fake.cuda_tts_options = reinterpret_cast<const audiocpp_options *>(0x5678);
    audiocpp_session *session = nullptr;
    std::string error;
    const hmi::engine::detail::SessionApi api{create_session, free_session, last_error};
    const auto *options = fake.cuda_tts_options;
    auto *model = reinterpret_cast<audiocpp_model *>(0x1000);
    CHECK(hmi::engine::detail::create_sdk_session(api, model, "tts", "offline", "auto", 7,
        &session, &error, nullptr, options));
    CHECK((fake.attempted == std::vector<std::string>{"cuda", "vulkan", "cpu"}));
    CHECK(fake.parameters_valid);
    fake = {};
    fake.success_backend = "cuda";
    CHECK(hmi::engine::detail::create_sdk_session(api, model, "asr", "streaming", "cuda", 7,
        &session, &error, nullptr, options));
    CHECK(fake.parameters_valid);
}

HMI_TEST(sdk_success_without_a_session_is_rejected)
{
    fake = {};
    fake.success_backend = "cpu";
    fake.null_success = true;
    audiocpp_session *session = nullptr;
    std::string error;
    CHECK(!open_fake("cpu", &session, &error));
    CHECK(session == nullptr);
    CHECK(error.find("engine returned no session") != std::string::npos);
}

HMI_TEST(sdk_library_prefers_the_project_bundle_before_legacy_dlls)
{
    const std::filesystem::path base = L"C:/HMI";
    std::vector<std::filesystem::path> probed;
    const auto primary = base / "native" / "native.dll";
    const auto all_present = [&](const auto &candidate) {
        probed.push_back(candidate);
        return true;
    };
    CHECK(hmi::engine::detail::select_sdk_library_path(base, {}, all_present) == primary);
    CHECK(probed.size() == 1);
    // Selection never probes legacy DLLs once the new file exists, even if load later fails.
    const auto legacy = base / "asr.dll";
    CHECK(hmi::engine::detail::select_sdk_library_path(base, {}, [&](const auto &candidate) {
        return candidate == legacy;
    }) == primary);
    CHECK(hmi::engine::detail::select_sdk_library_path(base, {}, [](const auto &) {
        return false;
    }) == primary);
    const auto flat = base / "native.dll";
    CHECK(hmi::engine::detail::select_sdk_library_path(base, {}, [&](const auto &candidate) {
        return candidate == flat;
    }) == flat);
}

HMI_TEST(native_library_override_precedes_legacy_environment_and_preserves_compatibility)
{
    const auto base = hmi::base::process_directory();
    const auto primary = base / "selected-native.dll";
    const auto legacy = base / "legacy-sdk.dll";
    SdkEnvironment native(L"HMI_NATIVE_DLL");
    SdkEnvironment old(L"AUDIOCPP_DLL");
    CHECK(native.set(primary));
    CHECK(old.set(legacy));
    std::string error;
    CHECK(hmi::engine::detail::sdk_library_path(&error) == std::filesystem::weakly_canonical(primary));
    CHECK(error.empty());
    CHECK(native.set({}));
    CHECK(hmi::engine::detail::sdk_library_path(&error) == std::filesystem::weakly_canonical(legacy));
}

HMI_TEST(sdk_library_explicit_override_is_strict)
{
    int probes = 0;
    const std::filesystem::path configured = L"C:/custom/absent.dll";
    const auto selected = hmi::engine::detail::select_sdk_library_path(L"C:/HMI", configured,
        [&](const auto &) { ++probes; return true; });
    CHECK(selected == configured);
    CHECK(probes == 0);
}

HMI_TEST(sdk_module_retention_keeps_the_actual_loaded_module)
{
    CHECK(hmi::engine::detail::retain_sdk_library(nullptr) == nullptr);
    const HMODULE original = GetModuleHandleW(L"kernel32.dll");
    const HMODULE retained = hmi::engine::detail::retain_sdk_library(original);
    CHECK(original != nullptr);
    CHECK(retained == original);
    if (retained) FreeLibrary(retained);
}
