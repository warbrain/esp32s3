#define AUDIOCPP_BUILD_DLL
#include <audiocpp.h>

#include <windows.h>

#include <cwchar>
#include <new>
#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

struct audiocpp_registry {};
struct audiocpp_model {};
struct audiocpp_request
{
    bool format_valid = false;
    std::size_t maximum_frames = 16000 * 30;
};
struct audiocpp_session
{
    bool active = false;
    std::size_t frames = 0;
    std::size_t previous_frames = 0;
    std::size_t maximum_frames = 16000 * 30;
};
struct audiocpp_event {};
struct audiocpp_result
{
    std::string text;
    std::vector<float> samples;
    int sample_rate = 16000;
    int channels = 1;
};

#ifndef HMI_TEST_NO_AUDIO_FRONTEND
struct audiocpp_audio_frontend
{
    audiocpp_audio_frontend_statistics statistics{};
    std::vector<float> pending;
    std::vector<float> vad_pending;
    std::size_t maximum_frames = 480000;
    float threshold = 0.35f;
    bool active = false;
};
struct audiocpp_hgtcrn
{
    audiocpp_hgtcrn_statistics statistics{};
    std::size_t maximum_frames = 480000;
};
#endif

namespace
{
bool failure_mode(const wchar_t *expected)
{
    wchar_t value[32]{};
    const DWORD count = GetEnvironmentVariableW(L"HMI_TEST_SDK_FAILURE", value, 32);
    return count != 0 && count < 32 && wcscmp(value, expected) == 0;
}

bool asr_mode(const wchar_t *expected)
{
    wchar_t value[32]{};
    const DWORD count = GetEnvironmentVariableW(L"HMI_TEST_ASR_MODE", value, 32);
    return count != 0 && count < 32 && wcscmp(value, expected) == 0;
}

#ifndef HMI_TEST_NO_AUDIO_FRONTEND
bool frontend_mode(const wchar_t *expected)
{
    wchar_t value[32]{};
    const DWORD count = GetEnvironmentVariableW(L"HMI_TEST_FRONTEND_MODE", value, 32);
    return count != 0 && count < 32 && wcscmp(value, expected) == 0;
}

bool hgtcrn_mode(const wchar_t *expected)
{
    wchar_t value[32]{};
    const DWORD count = GetEnvironmentVariableW(L"HMI_TEST_HGTCRN_MODE", value, 32);
    return count != 0 && count < 32 && wcscmp(value, expected) == 0;
}

void frontend_observe_vad(audiocpp_audio_frontend &frontend, bool flush)
{
    if ((frontend.statistics.flags & AUDIOCPP_AUDIO_FRONTEND_VAD_ENABLED) == 0) return;
    while (frontend.vad_pending.size() >= 512 || (flush && !frontend.vad_pending.empty()))
    {
        const auto count = std::min<std::size_t>(512, frontend.vad_pending.size());
        const bool nonzero = std::any_of(frontend.vad_pending.begin(),
            frontend.vad_pending.begin() + static_cast<std::ptrdiff_t>(count),
            [](float sample) { return std::abs(sample) > 0.00001f; });
        const float probability = frontend_mode(L"silent") ? 0.0f :
            frontend_mode(L"uncertain") ? frontend.threshold * 0.75f : nonzero ? 0.8f : 0.0f;
        ++frontend.statistics.vad_chunks;
        frontend.statistics.latest_vad_probability = probability;
        frontend.statistics.max_vad_probability = std::max(frontend.statistics.max_vad_probability, probability);
        frontend.statistics.speech_active = probability >= frontend.threshold ? 1u : 0u;
        if (frontend.statistics.speech_active != 0)
        {
            ++frontend.statistics.voiced_chunks;
            frontend.statistics.voiced_frames += count;
            frontend.statistics.speech_seen = 1;
        }
        else if (probability >= frontend.threshold / 2.0f)
            ++frontend.statistics.uncertain_chunks;
        frontend.vad_pending.erase(frontend.vad_pending.begin(),
            frontend.vad_pending.begin() + static_cast<std::ptrdiff_t>(count));
    }
}

audiocpp_status frontend_output(audiocpp_audio_frontend &frontend, std::size_t count,
                                audiocpp_result **out)
{
    if (count == 0) return AUDIOCPP_OK;
    auto *result = new (std::nothrow) audiocpp_result;
    if (!result) return AUDIOCPP_ERR_OUT_OF_MEMORY;
    result->samples.assign(frontend.pending.begin(),
        frontend.pending.begin() + static_cast<std::ptrdiff_t>(count));
    frontend.pending.erase(frontend.pending.begin(),
        frontend.pending.begin() + static_cast<std::ptrdiff_t>(count));
    frontend.statistics.output_frames += count;
    frontend.statistics.pending_frames = frontend.pending.size();
    if (frontend_mode(L"invalid_format")) result->sample_rate = 48000;
    if (frontend_mode(L"invalid_audio")) result->samples.front() = std::numeric_limits<float>::quiet_NaN();
    if (frontend_mode(L"excess_output"))
        result->samples.resize(static_cast<std::size_t>(frontend.statistics.input_frames) + 1, 0.25f);
    *out = result;
    return AUDIOCPP_OK;
}
#endif
}

extern "C"
{
uint32_t audiocpp_abi_version(void)
{
    constexpr uint32_t abi = (AUDIOCPP_ABI_VERSION_MAJOR << 16) |
        (AUDIOCPP_ABI_VERSION_MINOR << 8) | AUDIOCPP_ABI_VERSION_PATCH;
    return failure_mode(L"abi") ? abi ^ 1u : abi;
}

const char *audiocpp_build_version(void) { return "lifetime-fixture"; }
const char *audiocpp_last_error(void) { return "fixture registry failure"; }
const char *audiocpp_status_string(audiocpp_status) { return "fixture status"; }

#ifndef HMI_TEST_NO_AUDIO_FRONTEND
audiocpp_status audiocpp_audio_frontend_create(const audiocpp_audio_frontend_config *config,
                                              audiocpp_audio_frontend **out)
{
    if (out) *out = nullptr;
    if (!config || !out || config->struct_size < sizeof(*config) || config->max_frames > 480000 ||
        (config->vad_threshold != 0.0f && !(config->vad_threshold > 0.0f && config->vad_threshold < 1.0f)))
        return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (frontend_mode(L"create_failure")) return AUDIOCPP_ERR_LOAD_FAILED;
    auto *frontend = new (std::nothrow) audiocpp_audio_frontend;
    if (!frontend) return AUDIOCPP_ERR_OUT_OF_MEMORY;
    frontend->statistics.struct_size = sizeof(frontend->statistics);
    frontend->statistics.version = AUDIOCPP_AUDIO_FRONTEND_STATISTICS_VERSION;
    if (config->gtcrn_model_path && *config->gtcrn_model_path)
        frontend->statistics.flags |= AUDIOCPP_AUDIO_FRONTEND_GTCRN_ENABLED;
    if (config->vad_model_path && *config->vad_model_path)
        frontend->statistics.flags |= AUDIOCPP_AUDIO_FRONTEND_VAD_ENABLED;
    frontend->maximum_frames = config->max_frames != 0 ? config->max_frames : 480000;
    frontend->threshold = config->vad_threshold != 0.0f ? config->vad_threshold : 0.35f;
    *out = frontend;
    return AUDIOCPP_OK;
}

audiocpp_status audiocpp_audio_frontend_reset(audiocpp_audio_frontend *frontend, uint64_t request_id)
{
    if (!frontend) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (frontend_mode(L"reset_failure")) return AUDIOCPP_ERR_RUNTIME;
    const auto flags = frontend->statistics.flags &
        (AUDIOCPP_AUDIO_FRONTEND_GTCRN_ENABLED | AUDIOCPP_AUDIO_FRONTEND_VAD_ENABLED);
    frontend->statistics = {};
    frontend->statistics.struct_size = sizeof(frontend->statistics);
    frontend->statistics.version = AUDIOCPP_AUDIO_FRONTEND_STATISTICS_VERSION;
    frontend->statistics.flags = flags;
    frontend->statistics.request_id = request_id;
    frontend->pending.clear();
    frontend->vad_pending.clear();
    frontend->active = true;
    return AUDIOCPP_OK;
}

audiocpp_status audiocpp_audio_frontend_push(audiocpp_audio_frontend *frontend, const float *samples,
                                           size_t frames, audiocpp_result **out)
{
    if (out) *out = nullptr;
    if (!frontend || !out || !frontend->active || (!samples && frames != 0) ||
        frames > frontend->maximum_frames - frontend->statistics.input_frames)
        return AUDIOCPP_ERR_INVALID_ARGUMENT;
    for (std::size_t index = 0; index < frames; ++index)
        if (!std::isfinite(samples[index])) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (frontend_mode(L"push_failure"))
    {
        frontend->statistics.flags |= AUDIOCPP_AUDIO_FRONTEND_FAILED;
        frontend->active = false;
        return AUDIOCPP_ERR_RUNTIME;
    }
    if (frames == 0) return AUDIOCPP_OK;
    frontend->pending.insert(frontend->pending.end(), samples, samples + frames);
    if ((frontend->statistics.flags & AUDIOCPP_AUDIO_FRONTEND_VAD_ENABLED) != 0)
        frontend->vad_pending.insert(frontend->vad_pending.end(), samples, samples + frames);
    frontend->statistics.input_frames += frames;
    frontend->statistics.pending_frames = frontend->pending.size();
    frontend_observe_vad(*frontend, false);
    const bool denoising = (frontend->statistics.flags & AUDIOCPP_AUDIO_FRONTEND_GTCRN_ENABLED) != 0;
    // Simulate lookahead deterministically; this fixture never runs a denoising model.
    const auto delayed = denoising ? std::min<std::size_t>(256, frontend->pending.size()) : 0;
    const auto count = frontend->pending.size() - delayed;
    if (denoising) frontend->statistics.gtcrn_frames += (frames + 255) / 256;
    return frontend_output(*frontend, count, out);
}

audiocpp_status audiocpp_audio_frontend_finish(audiocpp_audio_frontend *frontend, audiocpp_result **out)
{
    if (out) *out = nullptr;
    if (!frontend || !out || !frontend->active) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (frontend_mode(L"finish_failure"))
    {
        frontend->statistics.flags |= AUDIOCPP_AUDIO_FRONTEND_FAILED;
        frontend->active = false;
        return AUDIOCPP_ERR_RUNTIME;
    }
    frontend_observe_vad(*frontend, true);
    const auto status = frontend_output(*frontend, frontend->pending.size(), out);
    if (status != AUDIOCPP_OK) return status;
    frontend->active = false;
    frontend->statistics.flags |= AUDIOCPP_AUDIO_FRONTEND_FINISHED;
    return AUDIOCPP_OK;
}

audiocpp_status audiocpp_audio_frontend_get_statistics(const audiocpp_audio_frontend *frontend,
                                                     audiocpp_audio_frontend_statistics *out)
{
    if (!frontend || !out || out->struct_size < sizeof(*out)) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (frontend_mode(L"statistics_failure")) return AUDIOCPP_ERR_RUNTIME;
    const auto size = out->struct_size;
    *out = frontend->statistics;
    out->struct_size = size;
    return AUDIOCPP_OK;
}

void audiocpp_audio_frontend_free(audiocpp_audio_frontend *frontend) { delete frontend; }

audiocpp_status audiocpp_hgtcrn_create(const audiocpp_hgtcrn_config *config, audiocpp_hgtcrn **out)
{
    if (out) *out = nullptr;
    if (!config || !out || config->struct_size < sizeof(*config) || !config->model_path ||
        !*config->model_path || config->max_frames > 480000 || config->reserved != 0)
        return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (hgtcrn_mode(L"create_failure")) return AUDIOCPP_ERR_LOAD_FAILED;
    auto *session = new (std::nothrow) audiocpp_hgtcrn;
    if (!session) return AUDIOCPP_ERR_OUT_OF_MEMORY;
    session->maximum_frames = config->max_frames != 0 ? config->max_frames : 480000;
    session->statistics.struct_size = sizeof(session->statistics);
    session->statistics.version = AUDIOCPP_HGTCRN_STATISTICS_VERSION;
    session->statistics.flags = AUDIOCPP_HGTCRN_READY;
    *out = session;
    return AUDIOCPP_OK;
}

audiocpp_status audiocpp_hgtcrn_run(audiocpp_hgtcrn *session, const float *samples, size_t frames,
                                   uint64_t request_id, audiocpp_result **out)
{
    if (out) *out = nullptr;
    if (!session || !samples || !out || frames < 4608 || frames > session->maximum_frames)
        return AUDIOCPP_ERR_INVALID_ARGUMENT;
    for (std::size_t index = 0; index < frames * 2; ++index)
        if (!std::isfinite(samples[index])) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    session->statistics = {};
    auto &stats = session->statistics;
    stats.struct_size = sizeof(stats);
    stats.version = AUDIOCPP_HGTCRN_STATISTICS_VERSION;
    stats.flags = AUDIOCPP_HGTCRN_READY;
    stats.channels = 2;
    stats.request_id = request_id;
    stats.input_frames = frames;
    if (hgtcrn_mode(L"run_failure"))
    {
        stats.flags |= AUDIOCPP_HGTCRN_FAILED;
        return AUDIOCPP_ERR_RUNTIME;
    }
    if (hgtcrn_mode(L"missing_audio")) return AUDIOCPP_OK;
    auto *result = new (std::nothrow) audiocpp_result;
    if (!result) return AUDIOCPP_ERR_OUT_OF_MEMORY;
    result->samples.resize(frames);
    for (std::size_t index = 0; index < frames; ++index) result->samples[index] = samples[index * 2];
    stats.flags |= AUDIOCPP_HGTCRN_SUCCEEDED;
    stats.output_frames = frames;
    stats.right_padding_frames = static_cast<uint32_t>((256 - frames % 256) % 256);
    stats.stft_frames = (frames + stats.right_padding_frames) / 256 + 1;
    stats.wpe_taps = 18;
    stats.iva_iterations = 20;
    stats.iva_speech_channel = 1;
    stats.channel_correlation = 0.25;
    stats.channel_rms_ratio = 0.75;
    if (hgtcrn_mode(L"invalid_rate")) result->sample_rate = 48000;
    if (hgtcrn_mode(L"invalid_channels")) result->channels = 2;
    if (hgtcrn_mode(L"wrong_length")) result->samples.pop_back();
    if (hgtcrn_mode(L"invalid_audio")) result->samples.front() = std::numeric_limits<float>::quiet_NaN();
    if (hgtcrn_mode(L"wrong_request")) ++stats.request_id;
    *out = result;
    return AUDIOCPP_OK;
}

audiocpp_status audiocpp_hgtcrn_get_statistics(const audiocpp_hgtcrn *session,
                                             audiocpp_hgtcrn_statistics *out)
{
    if (!session || !out || out->struct_size < sizeof(*out)) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (hgtcrn_mode(L"statistics_failure")) return AUDIOCPP_ERR_RUNTIME;
    const auto size = out->struct_size;
    *out = session->statistics;
    out->struct_size = size;
    if (hgtcrn_mode(L"invalid_statistics")) out->total_ms = std::numeric_limits<double>::quiet_NaN();
    return AUDIOCPP_OK;
}

void audiocpp_hgtcrn_free(audiocpp_hgtcrn *session) { delete session; }
#endif

audiocpp_status audiocpp_registry_create(const char *, audiocpp_registry **out)
{
    if (!out) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (failure_mode(L"registry")) return AUDIOCPP_ERR_RUNTIME;
    *out = new (std::nothrow) audiocpp_registry;
    return *out ? AUDIOCPP_OK : AUDIOCPP_ERR_OUT_OF_MEMORY;
}

void audiocpp_registry_free(audiocpp_registry *registry) { delete registry; }
size_t audiocpp_registry_family_count(const audiocpp_registry *registry) { return registry ? 1 : 0; }

audiocpp_status audiocpp_registry_family(const audiocpp_registry *registry, size_t index,
                                        const char **out)
{
    if (!registry || !out) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = index == 0 ? "qwen3_asr" : nullptr;
    return *out ? AUDIOCPP_OK : AUDIOCPP_ERR_OUT_OF_RANGE;
}

size_t audiocpp_task_count(void) { return 1; }
const char *audiocpp_task_name(size_t index) { return index == 0 ? "asr" : nullptr; }

audiocpp_status audiocpp_model_load(audiocpp_registry *registry, const char *,
                                   const audiocpp_model_config *, const audiocpp_options *,
                                   audiocpp_model **out)
{
    if (!registry || !out) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = new (std::nothrow) audiocpp_model;
    return *out ? AUDIOCPP_OK : AUDIOCPP_ERR_OUT_OF_MEMORY;
}
void audiocpp_model_free(audiocpp_model *model) { delete model; }

audiocpp_status audiocpp_session_create(const audiocpp_model *model, const char *task, const char *mode,
                                       const audiocpp_backend_config *backend, const audiocpp_options *,
                                       audiocpp_session **out)
{
    if (!model || !task || !mode || !backend || !backend->backend || !out ||
        std::strcmp(task, "asr") != 0 || std::strcmp(mode, "streaming") != 0)
        return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (std::strcmp(backend->backend, "cpu") != 0) return AUDIOCPP_ERR_NOT_AVAILABLE;
    *out = new (std::nothrow) audiocpp_session;
    return *out ? AUDIOCPP_OK : AUDIOCPP_ERR_OUT_OF_MEMORY;
}
void audiocpp_session_free(audiocpp_session *session) { delete session; }

audiocpp_status audiocpp_stream_policy(const audiocpp_session *session, audiocpp_stream_input_kind *input,
                                      audiocpp_stream_output_kind *output, int64_t *samples, double *seconds)
{
    if (!session) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (input) *input = AUDIOCPP_STREAM_INPUT_AUDIO_CHUNKS;
    if (output) *output = AUDIOCPP_STREAM_OUTPUT_FINAL_RESULT;
    if (samples) *samples = 16000;
    if (seconds) *seconds = 1.0;
    return AUDIOCPP_OK;
}

audiocpp_request *audiocpp_request_create(void) { return new (std::nothrow) audiocpp_request; }
void audiocpp_request_free(audiocpp_request *request) { delete request; }
audiocpp_status audiocpp_request_set_audio(audiocpp_request *request, const float *samples,
                                          size_t frames, int rate, int channels)
{
    if (!request) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    request->format_valid = samples == nullptr && frames == 0 && rate == 16000 && channels == 1;
    return request->format_valid ? AUDIOCPP_OK : AUDIOCPP_ERR_INVALID_ARGUMENT;
}
audiocpp_status audiocpp_request_set_option(audiocpp_request *request, const char *key, const char *value)
{
    if (!request || !key || !value) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (std::strcmp(key, "audio_chunk_duration_sec") == 0)
    {
        char *end = nullptr;
        const auto seconds = std::strtoul(value, &end, 10);
        if (!end || *end != '\0' || seconds < 1 || seconds > 30) return AUDIOCPP_ERR_INVALID_ARGUMENT;
        request->maximum_frames = static_cast<std::size_t>(seconds) * 16000;
    }
    return AUDIOCPP_OK;
}
audiocpp_status audiocpp_stream_start(audiocpp_session *session, const audiocpp_request *request)
{
    if (!session || !request || !request->format_valid) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    session->active = true;
    session->frames = 0;
    session->maximum_frames = request->maximum_frames;
    return AUDIOCPP_OK;
}
audiocpp_status audiocpp_stream_push(audiocpp_session *session, const float *samples, size_t frames,
                                    int rate, int channels, int64_t start_sample, audiocpp_event **out)
{
    if (out) *out = nullptr;
    if (!session || !session->active || !samples || frames == 0 || frames > 16000 ||
        rate != 16000 || channels != 1 || start_sample != static_cast<int64_t>(session->frames) ||
        session->frames > session->maximum_frames || frames > session->maximum_frames - session->frames)
        return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (asr_mode(L"push_failure")) return AUDIOCPP_ERR_RUNTIME;
    session->frames += frames;
    return AUDIOCPP_OK;
}
audiocpp_status audiocpp_stream_next_event(audiocpp_session *session, audiocpp_event **out)
{
    if (!session || !out) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    return AUDIOCPP_OK;
}
audiocpp_status audiocpp_stream_finish(audiocpp_session *session, audiocpp_result **out)
{
    if (!session || !session->active || session->frames == 0 || !out) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (asr_mode(L"finish_failure")) return AUDIOCPP_ERR_RUNTIME;
    if (asr_mode(L"slow")) Sleep(25);
    auto *result = new (std::nothrow) audiocpp_result;
    if (!result) return AUDIOCPP_ERR_OUT_OF_MEMORY;
    if (asr_mode(L"report_frames"))
        result->text = "frames=" + std::to_string(session->frames);
    else if (session->previous_frames == session->frames)
        result->text = "final refined phrase";
    else if (session->frames >= 64000)
        result->text = "corrected complete prefix";
    else
        result->text = "early draft";
    session->previous_frames = session->frames;
    session->active = false;
    *out = result;
    return AUDIOCPP_OK;
}
audiocpp_status audiocpp_stream_reset(audiocpp_session *session)
{
    if (!session) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    session->active = false;
    session->frames = 0;
    return AUDIOCPP_OK;
}
void audiocpp_event_free(audiocpp_event *event) { delete event; }
void audiocpp_result_free(audiocpp_result *result) { delete result; }

audiocpp_status audiocpp_result_audio(const audiocpp_result *result, const float **samples,
                                     size_t *frames, int *rate, int *channels)
{
    if (!result) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (result->samples.empty()) return AUDIOCPP_ERR_NOT_AVAILABLE;
    if (samples) *samples = result->samples.data();
    if (frames) *frames = result->samples.size();
    if (rate) *rate = result->sample_rate;
    if (channels) *channels = result->channels;
    return AUDIOCPP_OK;
}
audiocpp_status audiocpp_result_text(const audiocpp_result *result, const char **text, const char **language)
{
    if (!result || !text) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *text = result->text.c_str();
    if (language) *language = "zh";
    return AUDIOCPP_OK;
}
}
