#include "engine/hgtcrn_session.hpp"
#include "engine/native_client.hpp"
#include "engine/sdk_library.hpp"
#include "engine/sdk_threads.hpp"
#include "base/project_paths.hpp"

#include <audiocpp.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace hmi::engine
{
struct HgtcrnSession::Impl
{
    HMODULE module = nullptr;
    audiocpp_hgtcrn *session = nullptr;
    std::size_t maximum_frames = 480000;
    HgtcrnStatistics stats;
    decltype(&::audiocpp_last_error) last_error = nullptr;
    decltype(&::audiocpp_hgtcrn_create) create = nullptr;
    decltype(&::audiocpp_hgtcrn_run) run = nullptr;
    decltype(&::audiocpp_hgtcrn_get_statistics) get_statistics = nullptr;
    decltype(&::audiocpp_hgtcrn_free) free = nullptr;
    decltype(&::audiocpp_result_audio) result_audio = nullptr;
    decltype(&::audiocpp_result_free) result_free = nullptr;

    bool fail(const std::string &message, std::string *error)
    {
        stats.failed = true;
        stats.succeeded = false;
        if (error) *error = message;
        return false;
    }

    bool check(audiocpp_status status, const char *operation, std::string *error)
    {
        if (status == AUDIOCPP_OK) return true;
        const char *message = last_error ? last_error() : nullptr;
        return fail(std::string(operation) + ": " + (message && *message ?
            detail::native_diagnostic(message) : "native dual-channel processing failed"), error);
    }

    bool refresh(std::string *error)
    {
        audiocpp_hgtcrn_statistics value{};
        value.struct_size = sizeof(value);
        if (!check(get_statistics(session, &value), "read dual-channel processing statistics", error)) return false;
        if (value.version != AUDIOCPP_HGTCRN_STATISTICS_VERSION || value.input_frames > maximum_frames ||
            value.output_frames > value.input_frames || value.right_padding_frames > 255 ||
            !std::isfinite(value.channel_correlation) || value.channel_correlation < -1.0 || value.channel_correlation > 1.0 ||
            !std::isfinite(value.channel_rms_ratio) || value.channel_rms_ratio < 0.0 ||
            !std::isfinite(value.stft_ms) || value.stft_ms < 0.0 ||
            !std::isfinite(value.wpe_ms) || value.wpe_ms < 0.0 ||
            !std::isfinite(value.iva_ms) || value.iva_ms < 0.0 ||
            !std::isfinite(value.network_ms) || value.network_ms < 0.0 ||
            !std::isfinite(value.istft_ms) || value.istft_ms < 0.0 ||
            !std::isfinite(value.total_ms) || value.total_ms < 0.0)
            return fail("invalid dual-channel processing statistics", error);
        stats.ready = (value.flags & AUDIOCPP_HGTCRN_READY) != 0;
        stats.succeeded = (value.flags & AUDIOCPP_HGTCRN_SUCCEEDED) != 0;
        stats.failed = (value.flags & AUDIOCPP_HGTCRN_FAILED) != 0;
        stats.channels = value.channels;
        stats.request_id = value.request_id;
        stats.input_frames = value.input_frames;
        stats.output_frames = value.output_frames;
        stats.stft_frames = value.stft_frames;
        stats.wpe_taps = value.wpe_taps;
        stats.iva_iterations = value.iva_iterations;
        stats.iva_speech_channel = value.iva_speech_channel;
        stats.right_padding_frames = value.right_padding_frames;
        stats.channel_correlation = value.channel_correlation;
        stats.channel_rms_ratio = value.channel_rms_ratio;
        stats.stft_ms = value.stft_ms;
        stats.wpe_ms = value.wpe_ms;
        stats.iva_ms = value.iva_ms;
        stats.network_ms = value.network_ms;
        stats.istft_ms = value.istft_ms;
        stats.total_ms = value.total_ms;
        return true;
    }
};

HgtcrnSession::HgtcrnSession() : impl_(std::make_unique<Impl>()) {}
HgtcrnSession::~HgtcrnSession() { close(); }

bool HgtcrnSession::open(Client &client, const HgtcrnConfig &config, std::string *error)
{
    close();
    if (error) error->clear();
    auto &impl = *impl_;
    if (!client.ready() || config.maximum_audio_seconds < 1 || config.maximum_audio_seconds > 30)
        return impl.fail("dual-channel processing requires an open engine and a 1..30 second limit", error);
    std::filesystem::path model;
    if (!base::resolve_project_path(config.model, base::process_directory(), model, error)) return false;
    const auto path = model.generic_u8string();
    const std::string model_path(path.begin(), path.end());
    impl.maximum_frames = static_cast<std::size_t>(config.maximum_audio_seconds) * 16000;
    impl.module = detail::retain_sdk_library(client.module_handle());
    if (!impl.module) return impl.fail("cannot retain dual-channel processing engine", error);
    const auto bind = [&impl, error](const char *name, auto &target) {
        target = reinterpret_cast<std::decay_t<decltype(target)>>(GetProcAddress(impl.module, name));
        return target != nullptr || impl.fail("native dual-channel interface missing: " + detail::native_diagnostic(name), error);
    };
    if (!bind("audiocpp_last_error", impl.last_error) || !bind("audiocpp_hgtcrn_create", impl.create) ||
        !bind("audiocpp_hgtcrn_run", impl.run) || !bind("audiocpp_hgtcrn_get_statistics", impl.get_statistics) ||
        !bind("audiocpp_hgtcrn_free", impl.free) || !bind("audiocpp_result_audio", impl.result_audio) ||
        !bind("audiocpp_result_free", impl.result_free))
    { close(); return false; }
    audiocpp_hgtcrn_config selected{};
    selected.struct_size = sizeof(selected);
    selected.cpu_threads = 1;
    selected.model_path = model_path.c_str();
    selected.max_frames = static_cast<std::uint32_t>(impl.maximum_frames);
    const detail::ScopedSdkThreads threads(1);
    if (!impl.check(impl.create(&selected, &impl.session), "open dual-channel processing", error) || !impl.session)
    {
        if (error && error->empty()) *error = "native dual-channel processing returned no handle";
        close();
        return false;
    }
    if (!impl.refresh(error)) { close(); return false; }
    return true;
}

void HgtcrnSession::close()
{
    auto &impl = *impl_;
    if (impl.session && impl.free) impl.free(impl.session);
    impl.session = nullptr;
    if (impl.module) FreeLibrary(impl.module);
    impl.module = nullptr;
    impl.stats = {};
}

bool HgtcrnSession::ready() const noexcept { return impl_->session != nullptr; }

bool HgtcrnSession::run(const float *interleaved, std::size_t frames, std::uint64_t request_id,
                        std::vector<float> &mono, std::string *error)
{
    mono.clear();
    if (error) error->clear();
    auto &impl = *impl_;
    impl.stats = {};
    impl.stats.ready = ready();
    impl.stats.request_id = request_id;
    if (!ready() || !interleaved || frames == 0 || frames > impl.maximum_frames ||
        !std::all_of(interleaved, interleaved + frames * 2, [](float sample) { return std::isfinite(sample); }))
        return impl.fail("dual-channel processing requires finite interleaved stereo PCM within the audio limit", error);
    const detail::ScopedSdkThreads threads(1);
    audiocpp_result *raw = nullptr;
    const auto status = impl.run(impl.session, interleaved, frames, request_id, &raw);
    const std::unique_ptr<audiocpp_result, decltype(impl.result_free)> result(raw, impl.result_free);
    if (!impl.check(status, "enhance dual-channel recording", error))
    {
        std::string ignored;
        (void)impl.refresh(&ignored);
        impl.stats.failed = true;
        impl.stats.succeeded = false;
        return false;
    }
    if (!result) return impl.fail("native dual-channel processing returned no audio", error);
    const float *samples = nullptr;
    std::size_t count = 0;
    int sample_rate = 0, channels = 0;
    if (!impl.check(impl.result_audio(result.get(), &samples, &count, &sample_rate, &channels),
            "read enhanced recording", error) || !impl.refresh(error)) return false;
    if (!samples || count != frames || sample_rate != 16000 || channels != 1 || !impl.stats.succeeded ||
        impl.stats.failed || impl.stats.request_id != request_id || impl.stats.input_frames != frames ||
        impl.stats.output_frames != frames || impl.stats.channels != 2 ||
        !std::all_of(samples, samples + count, [](float sample) { return std::isfinite(sample); }))
        return impl.fail("invalid enhanced recording or frame count", error);
    mono.assign(samples, samples + count);
    return true;
}

HgtcrnStatistics HgtcrnSession::statistics() const noexcept { return impl_->stats; }
}
