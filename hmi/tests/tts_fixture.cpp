#define AUDIOCPP_BUILD_DLL
#include <audiocpp.h>

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

struct audiocpp_registry {};
struct audiocpp_model {};
struct audiocpp_session {};
struct audiocpp_request { std::string text; std::unordered_map<std::string, std::string> options; };
struct audiocpp_result
{
    std::vector<float> samples;
    std::vector<std::pair<std::string, std::string>> generation_metadata;
};

namespace
{
struct FixtureState
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> texts;
    std::vector<std::unordered_map<std::string, std::string>> options;
    unsigned models = 0;
    unsigned sessions = 0;
    unsigned model_frees = 0;
    unsigned session_frees = 0;
    unsigned completed = 0;
    unsigned block_call = 0;
    unsigned fail_call = 0;
    unsigned generation_mode = 0;
    bool blocked = false;
    bool released = false;
};

// Test-only state shared by the C ABI and explicit synchronization hooks.
FixtureState state;
}

extern "C"
{
__declspec(dllexport) void hmi_tts_fixture_reset(unsigned block_call, unsigned fail_call)
{
    std::lock_guard lock(state.mutex);
    state.texts.clear();
    state.options.clear();
    state.models = state.sessions = state.model_frees = state.session_frees = state.completed = 0;
    state.block_call = block_call;
    state.fail_call = fail_call;
    state.generation_mode = 0;
    state.blocked = state.released = false;
}

// 0=legacy PCM, 1=first run length then EOS, 2=always length,
// 3=malformed reason, 4=legacy first-run limit, 5=EOS at the frame boundary,
// 6=EOS only when the main sampler is enabled, 7=first two runs length then EOS.
__declspec(dllexport) void hmi_tts_fixture_generation(unsigned mode)
{
    std::lock_guard lock(state.mutex);
    state.generation_mode = mode;
}

__declspec(dllexport) int hmi_tts_fixture_wait_blocked(unsigned timeout_ms)
{
    std::unique_lock lock(state.mutex);
    return state.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] { return state.blocked; }) ? 1 : 0;
}

__declspec(dllexport) void hmi_tts_fixture_release()
{
    { std::lock_guard lock(state.mutex); state.released = true; }
    state.changed.notify_all();
}

// 0=loads, 1=session creates, 2=runs, 3=completed, 4=model frees, 5=session frees.
__declspec(dllexport) unsigned hmi_tts_fixture_count(unsigned kind)
{
    std::lock_guard lock(state.mutex);
    switch (kind)
    {
    case 0: return state.models;
    case 1: return state.sessions;
    case 2: return static_cast<unsigned>(state.texts.size());
    case 3: return state.completed;
    case 4: return state.model_frees;
    case 5: return state.session_frees;
    default: return 0;
    }
}

// One-based run index; a zero capacity can query the required UTF-8 size.
__declspec(dllexport) std::size_t hmi_tts_fixture_text(unsigned index, char *out, std::size_t capacity)
{
    std::lock_guard lock(state.mutex);
    if (index == 0 || index > state.texts.size()) return 0;
    const auto &text = state.texts[index - 1];
    if (out && capacity > text.size()) std::memcpy(out, text.c_str(), text.size() + 1);
    return text.size();
}

__declspec(dllexport) std::size_t hmi_tts_fixture_option(unsigned index, const char *key, char *out, std::size_t capacity)
{
    std::lock_guard lock(state.mutex);
    if (!key || index == 0 || index > state.options.size()) return 0;
    const auto &options = state.options[index - 1];
    const auto found = options.find(key);
    if (found == options.end()) return 0;
    if (out && capacity > found->second.size())
        std::memcpy(out, found->second.c_str(), found->second.size() + 1);
    return found->second.size();
}

uint32_t audiocpp_abi_version(void)
{
    return (AUDIOCPP_ABI_VERSION_MAJOR << 16) | (AUDIOCPP_ABI_VERSION_MINOR << 8) | AUDIOCPP_ABI_VERSION_PATCH;
}
const char *audiocpp_build_version(void) { return "tts-segment-fixture"; }
const char *audiocpp_last_error(void) { return "fixture synthesis failure"; }
const char *audiocpp_status_string(audiocpp_status) { return "fixture status"; }

audiocpp_status audiocpp_registry_create(const char *, audiocpp_registry **out)
{
    if (!out) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = new (std::nothrow) audiocpp_registry;
    return *out ? AUDIOCPP_OK : AUDIOCPP_ERR_OUT_OF_MEMORY;
}
void audiocpp_registry_free(audiocpp_registry *registry) { delete registry; }
size_t audiocpp_registry_family_count(const audiocpp_registry *registry) { return registry ? 1 : 0; }
audiocpp_status audiocpp_registry_family(const audiocpp_registry *registry, size_t index, const char **out)
{
    if (!registry || !out) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = index == 0 ? "qwen3_tts" : nullptr;
    return *out ? AUDIOCPP_OK : AUDIOCPP_ERR_OUT_OF_RANGE;
}
size_t audiocpp_task_count(void) { return 1; }
const char *audiocpp_task_name(size_t index) { return index == 0 ? "tts" : nullptr; }

audiocpp_status audiocpp_model_load(audiocpp_registry *registry, const char *path,
                                   const audiocpp_model_config *selection, const audiocpp_options *,
                                   audiocpp_model **out)
{
    if (!registry || !path || !selection || !selection->family_hint || !out ||
        std::strcmp(selection->family_hint, "qwen3_tts") != 0) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = new (std::nothrow) audiocpp_model;
    if (!*out) return AUDIOCPP_ERR_OUT_OF_MEMORY;
    {
        std::unique_lock lock(state.mutex);
        ++state.models;
        // The all-bits sentinel blocks model loading before any SDK request runs.
        if (state.block_call == ~0U)
        {
            state.blocked = true;
            state.changed.notify_all();
            if (!state.changed.wait_for(lock, std::chrono::seconds(10), [] { return state.released; }))
            {
                delete *out;
                *out = nullptr;
                return AUDIOCPP_ERR_RUNTIME;
            }
        }
        // One-shot model failure lets preparation recovery retry a fresh load.
        if (state.fail_call == ~0U)
        {
            state.fail_call = 0;
            delete *out;
            *out = nullptr;
            return AUDIOCPP_ERR_RUNTIME;
        }
    }
    return AUDIOCPP_OK;
}
void audiocpp_model_free(audiocpp_model *model)
{
    if (model) { std::lock_guard lock(state.mutex); ++state.model_frees; }
    delete model;
}

audiocpp_status audiocpp_session_create(const audiocpp_model *model, const char *task, const char *mode,
                                       const audiocpp_backend_config *backend, const audiocpp_options *,
                                       audiocpp_session **out)
{
    if (!model || !task || !mode || !backend || !backend->backend || !out || backend->threads < 1 ||
        std::strcmp(task, "tts") != 0 || std::strcmp(mode, "offline") != 0) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (std::strcmp(backend->backend, "cpu") != 0) return AUDIOCPP_ERR_NOT_AVAILABLE;
    *out = new (std::nothrow) audiocpp_session;
    if (!*out) return AUDIOCPP_ERR_OUT_OF_MEMORY;
    { std::lock_guard lock(state.mutex); ++state.sessions; }
    return AUDIOCPP_OK;
}
void audiocpp_session_free(audiocpp_session *session)
{
    if (session) { std::lock_guard lock(state.mutex); ++state.session_frees; }
    delete session;
}

audiocpp_request *audiocpp_request_create(void) { return new (std::nothrow) audiocpp_request; }
void audiocpp_request_free(audiocpp_request *request) { delete request; }
audiocpp_status audiocpp_request_set_text(audiocpp_request *request, const char *text, const char *language)
{
    if (!request || !text || !*text || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text, -1, nullptr, 0) <= 0) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    try
    {
        request->text = text;
        request->options["fixture.text_language"] = language ? language : "";
    }
    catch (...) { return AUDIOCPP_ERR_OUT_OF_MEMORY; }
    return AUDIOCPP_OK;
}

audiocpp_status audiocpp_request_set_option(audiocpp_request *request, const char *key, const char *value)
{
    if (!request || !key || !*key || !value) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    try { request->options[key] = value; }
    catch (...) { return AUDIOCPP_ERR_OUT_OF_MEMORY; }
    return AUDIOCPP_OK;
}
audiocpp_status audiocpp_request_set_voice_id(audiocpp_request *request, const char *voice)
{
    if (!request || !voice || !*voice) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    try { request->options["fixture.voice_id"] = voice; }
    catch (...) { return AUDIOCPP_ERR_OUT_OF_MEMORY; }
    return AUDIOCPP_OK;
}
audiocpp_status audiocpp_request_set_voice_audio(audiocpp_request *request, const float *samples,
                                                size_t frames, int rate, int channels)
{
    return request && samples && frames && rate > 0 && channels > 0 ? AUDIOCPP_OK : AUDIOCPP_ERR_INVALID_ARGUMENT;
}

audiocpp_status audiocpp_session_run(audiocpp_session *session, const audiocpp_request *request,
                                    audiocpp_result **out)
{
    if (!session || !request || request->text.empty() || !out) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    try
    {
        unsigned index = 0;
        unsigned generation_mode = 0;
        {
            std::unique_lock lock(state.mutex);
            state.texts.push_back(request->text);
            state.options.push_back(request->options);
            index = static_cast<unsigned>(state.texts.size());
            if (index == state.block_call)
            {
                state.blocked = true;
                state.changed.notify_all();
                if (!state.changed.wait_for(lock, std::chrono::seconds(10), [] { return state.released; }))
                    return AUDIOCPP_ERR_RUNTIME;
            }
            if (index == state.fail_call) return AUDIOCPP_ERR_RUNTIME;
            generation_mode = state.generation_mode;
            ++state.completed;
        }
        auto *result = new (std::nothrow) audiocpp_result;
        if (!result) return AUDIOCPP_ERR_OUT_OF_MEMORY;
        try
        {
            const auto found = request->options.find("max_tokens");
            const auto budget = found != request->options.end() ? std::stoull(found->second) : 2048;
            const auto sampled = request->options.find("do_sample");
            const bool main_sampling = sampled != request->options.end() && sampled->second == "true";
            const bool limited = generation_mode == 2 || (generation_mode == 6 && !main_sampling) ||
                (generation_mode == 7 && index <= 2) ||
                ((generation_mode == 1 || generation_mode == 4) && index == 1);
            const auto codec_frames = limited || generation_mode == 5 ? budget - 1 : 1;
            const auto pcm_frames = generation_mode == 0 || (generation_mode == 4 && !limited)
                ? std::size_t{240} : static_cast<std::size_t>(codec_frames) * 1920;
            result->samples.assign(pcm_frames, static_cast<float>(index) / 1000.0f);
            if (generation_mode != 0 && generation_mode != 4)
                result->generation_metadata = {
                    {"finish_reason", generation_mode == 3 ? "invalid" : limited ? "length" : "eos"},
                    {"generated_frames", std::to_string(codec_frames)},
                    {"max_tokens", std::to_string(budget)},
                    {"generation_chunks", "1"}};
        }
        catch (...) { delete result; return AUDIOCPP_ERR_OUT_OF_MEMORY; }
        *out = result;
        return AUDIOCPP_OK;
    }
    catch (...) { return AUDIOCPP_ERR_RUNTIME; }
}
audiocpp_status audiocpp_result_audio(const audiocpp_result *result, const float **samples,
                                     size_t *frames, int *rate, int *channels)
{
    if (!result || !samples || !frames || !rate || !channels) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    *samples = result->samples.data();
    *frames = result->samples.size();
    *rate = 24000;
    *channels = 1;
    return AUDIOCPP_OK;
}
void audiocpp_result_free(audiocpp_result *result) { delete result; }

size_t audiocpp_result_artifact_count(const audiocpp_result *result)
{
    return result && !result->generation_metadata.empty() ? 1 : 0;
}
audiocpp_status audiocpp_result_artifact(const audiocpp_result *result, size_t index,
                                       audiocpp_artifact_kind *kind, const char **id,
                                       const void **payload, size_t *payload_bytes)
{
    if (!result) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (index != 0 || result->generation_metadata.empty()) return AUDIOCPP_ERR_OUT_OF_RANGE;
    if (kind) *kind = AUDIOCPP_ARTIFACT_CUSTOM;
    if (id) *id = "qwen3_tts_generation";
    if (payload) *payload = nullptr;
    if (payload_bytes) *payload_bytes = 0;
    return AUDIOCPP_OK;
}
size_t audiocpp_result_artifact_meta_count(const audiocpp_result *result, size_t index)
{
    return result && index == 0 ? result->generation_metadata.size() : 0;
}
audiocpp_status audiocpp_result_artifact_meta(const audiocpp_result *result, size_t index,
                                            size_t field, const char **key, const char **value)
{
    if (!result) return AUDIOCPP_ERR_INVALID_ARGUMENT;
    if (index != 0 || field >= result->generation_metadata.size()) return AUDIOCPP_ERR_OUT_OF_RANGE;
    if (key) *key = result->generation_metadata[field].first.c_str();
    if (value) *value = result->generation_metadata[field].second.c_str();
    return AUDIOCPP_OK;
}
}
