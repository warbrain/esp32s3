#include "engine/native_client.hpp"
#include "engine/model_location.hpp"
#include "engine/sdk_threads.hpp"
#include "base/logger.hpp"
#include "base/project_paths.hpp"
#include "util/json_writer.hpp"

#include <audiocpp.h>
#include <windows.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
using Clock = std::chrono::steady_clock;
using hmi::util::json::Value;

std::string utf8(std::wstring_view value)
{
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("invalid Unicode argument");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), count, nullptr, nullptr) != count) throw std::runtime_error("cannot encode argument");
    return result;
}

std::uint64_t integer(const std::wstring &value, std::uint64_t maximum)
{
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::runtime_error("numeric option must contain decimal digits");
    const auto parsed = std::stoull(value);
    if (parsed > maximum) throw std::runtime_error("numeric option exceeds its limit");
    return parsed;
}

struct Options
{
    std::string text;
    std::string language = "zh";
    std::string instruction;
    std::filesystem::path output;
    std::uint32_t seed = 42;
    int budget = 96;
    int iterations = 1;
    bool main_sampling = false;
    bool subtalker_sampling = false;
    bool help = false;
};

Options options(int argc, wchar_t **argv)
{
    Options result;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring key = argv[index];
        if (key == L"--help") { result.help = true; continue; }
        if (++index == argc) throw std::runtime_error("missing option value");
        const std::wstring value = argv[index];
        if (key == L"--text") result.text = utf8(value);
        else if (key == L"--instruct") result.instruction = utf8(value);
        else if (key == L"--output") result.output = value;
        else if (key == L"--language")
        {
            if (value != L"zh" && value != L"auto" && value != L"en")
                throw std::runtime_error("language must be zh, auto or en");
            result.language = utf8(value);
        }
        else if (key == L"--main-sampling") result.main_sampling = integer(value, 1) != 0;
        else if (key == L"--subtalker-sampling") result.subtalker_sampling = integer(value, 1) != 0;
        else if (key == L"--seed")
            result.seed = static_cast<std::uint32_t>(integer(value, std::numeric_limits<std::uint32_t>::max()));
        else if (key == L"--budget") result.budget = static_cast<int>(integer(value, 2048));
        else if (key == L"--iterations") result.iterations = static_cast<int>(integer(value, 2));
        else throw std::runtime_error("unknown option");
    }
    if (!result.help && (result.text.empty() || result.text.size() > 4096 || result.output.empty() ||
            result.budget < 1 || result.iterations < 1))
        throw std::runtime_error("text must be 1..4096 UTF-8 bytes, output required, budget 1..2048, iterations 1..2");
    return result;
}

std::filesystem::path output_directory(const std::filesystem::path &requested)
{
    const auto base = hmi::base::process_directory();
    const auto root = hmi::base::project_root(base);
    std::filesystem::path resolved;
    std::string error;
    if (!hmi::base::resolve_project_path(requested.is_absolute() ? requested : root / requested, base, resolved, &error))
        throw std::runtime_error(error);
    const auto build = std::filesystem::weakly_canonical(root / "build");
    auto part = resolved.begin();
    for (const auto &expected : build)
    {
        if (part == resolved.end() || CompareStringOrdinal(expected.c_str(), -1, part->c_str(), -1, TRUE) != CSTR_EQUAL)
            throw std::runtime_error("output must remain inside the project build directory");
        ++part;
    }
    if (part == resolved.end()) throw std::runtime_error("output must be a build subdirectory");
    return resolved;
}

void write_json(const std::filesystem::path &path, const Value &value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << hmi::util::json::stringify(value) << '\n';
    if (!output) throw std::runtime_error("cannot write quality probe JSON");
}

std::uint64_t fingerprint(const void *data, std::size_t bytes)
{
    auto hash = std::uint64_t{14695981039346656037ULL};
    const auto *input = static_cast<const unsigned char *>(data);
    for (std::size_t index = 0; index < bytes; ++index) { hash ^= input[index]; hash *= 1099511628211ULL; }
    return hash;
}

class Deadline
{
public:
    Deadline() : worker_([this] {
        std::unique_lock lock(mutex_);
        if (changed_.wait_for(lock, std::chrono::seconds(120), [this] { return done_; })) return;
        std::fprintf(stderr, "FAIL watchdog timeout_seconds=120\n");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 124);
    }) {}
    ~Deadline()
    {
        { std::lock_guard lock(mutex_); done_ = true; }
        changed_.notify_one();
        worker_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
    std::thread worker_;
};

template<class Function>
Function symbol(HMODULE module, const char *name)
{
    const auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("native SDK export missing: ") + name);
    Function result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

struct Api
{
    explicit Api(HMODULE module)
        : last_error(symbol<decltype(last_error)>(module, "audiocpp_last_error")),
          model_load(symbol<decltype(model_load)>(module, "audiocpp_model_load")),
          model_free(symbol<decltype(model_free)>(module, "audiocpp_model_free")),
          session_create(symbol<decltype(session_create)>(module, "audiocpp_session_create")),
          session_free(symbol<decltype(session_free)>(module, "audiocpp_session_free")),
          request_create(symbol<decltype(request_create)>(module, "audiocpp_request_create")),
          request_free(symbol<decltype(request_free)>(module, "audiocpp_request_free")),
          set_text(symbol<decltype(set_text)>(module, "audiocpp_request_set_text")),
          set_voice(symbol<decltype(set_voice)>(module, "audiocpp_request_set_voice_id")),
          set_option(symbol<decltype(set_option)>(module, "audiocpp_request_set_option")),
          run(symbol<decltype(run)>(module, "audiocpp_session_run")),
          result_free(symbol<decltype(result_free)>(module, "audiocpp_result_free")),
          audio(symbol<decltype(audio)>(module, "audiocpp_result_audio")),
          artifact_count(symbol<decltype(artifact_count)>(module, "audiocpp_result_artifact_count")),
          artifact(symbol<decltype(artifact)>(module, "audiocpp_result_artifact")),
          meta_count(symbol<decltype(meta_count)>(module, "audiocpp_result_artifact_meta_count")),
          meta(symbol<decltype(meta)>(module, "audiocpp_result_artifact_meta")) {}

    void check(audiocpp_status status, const char *operation) const
    {
        if (status == AUDIOCPP_OK) return;
        const char *detail = last_error();
        throw std::runtime_error(std::string(operation) + ": " + (detail ? detail : "SDK call failed"));
    }

    decltype(&::audiocpp_last_error) last_error;
    decltype(&::audiocpp_model_load) model_load;
    decltype(&::audiocpp_model_free) model_free;
    decltype(&::audiocpp_session_create) session_create;
    decltype(&::audiocpp_session_free) session_free;
    decltype(&::audiocpp_request_create) request_create;
    decltype(&::audiocpp_request_free) request_free;
    decltype(&::audiocpp_request_set_text) set_text;
    decltype(&::audiocpp_request_set_voice_id) set_voice;
    decltype(&::audiocpp_request_set_option) set_option;
    decltype(&::audiocpp_session_run) run;
    decltype(&::audiocpp_result_free) result_free;
    decltype(&::audiocpp_result_audio) audio;
    decltype(&::audiocpp_result_artifact_count) artifact_count;
    decltype(&::audiocpp_result_artifact) artifact;
    decltype(&::audiocpp_result_artifact_meta_count) meta_count;
    decltype(&::audiocpp_result_artifact_meta) meta;
};

Value::Object generation_metadata(const Api &api, const audiocpp_result *result)
{
    Value::Object metadata;
    const auto count = api.artifact_count(result);
    if (count > 4096) throw std::runtime_error("excessive SDK artifact count");
    bool found = false;
    for (std::size_t index = 0; index < count; ++index)
    {
        audiocpp_artifact_kind kind = AUDIOCPP_ARTIFACT_CUSTOM;
        const char *id = nullptr;
        api.check(api.artifact(result, index, &kind, &id, nullptr, nullptr), "read generation artifact");
        if (!id || std::strcmp(id, "qwen3_tts_generation") != 0) continue;
        if (found || kind != AUDIOCPP_ARTIFACT_CUSTOM) throw std::runtime_error("invalid generation artifact");
        found = true;
        const auto fields = api.meta_count(result, index);
        if (fields > 4096) throw std::runtime_error("excessive generation metadata count");
        for (std::size_t field = 0; field < fields; ++field)
        {
            const char *key = nullptr, *value = nullptr;
            api.check(api.meta(result, index, field, &key, &value), "read generation metadata");
            if (!key || !value || !metadata.emplace(key, Value(value)).second)
                throw std::runtime_error("invalid generation metadata field");
        }
    }
    return metadata;
}

std::string metadata_text(const Value::Object &metadata, const char *key)
{
    const auto found = metadata.find(key);
    return found == metadata.end() ? std::string{} : std::get<std::string>(found->second.data);
}
}

int wmain(int argc, wchar_t **argv)
{
    try
    {
        const auto selected = options(argc, argv);
        if (selected.help)
        {
            std::puts("hmi_tts_quality_probe --text TEXT --output build/FRESH_DIR [--language zh|auto|en] [--instruct TEXT]\n"
                      "  [--main-sampling 0|1] [--subtalker-sampling 0|1] [--seed 42] [--budget 96] [--iterations 1..2]\n"
                      "Direct SDK single-text diagnostic; CUDA/4 threads/serena; no splitting, retries, playback or ASR.");
            return 0;
        }
        const auto directory = output_directory(selected.output);
        if (std::filesystem::exists(directory) && !std::filesystem::is_empty(directory))
            throw std::runtime_error("output directory must be empty to preserve previous evidence");
        std::filesystem::create_directories(directory);
        Deadline deadline;
        hmi::base::Logger logger;
        hmi::base::LoggerOptions logging;
        logging.file = directory / "quality.log";
        logging.level = hmi::base::LogLevel::Debug;
        std::string error;
        if (!logger.configure(logging, &error)) throw std::runtime_error(error);
        hmi::engine::Client client;
        if (!client.open(&error, {}, &logger)) throw std::runtime_error(error);
        const auto sdk_path = std::filesystem::weakly_canonical(hmi::base::process_directory() / "native/native.dll");
        // Borrow the module already preflighted and owned by Client; never load another SDK copy.
        const auto module = GetModuleHandleW(sdk_path.c_str());
        wchar_t actual[32768]{};
        const auto count = module ? GetModuleFileNameW(module, actual, 32768) : 0;
        if (!count || count >= 32768 || !std::filesystem::equivalent(sdk_path, std::filesystem::path(std::wstring(actual, count))))
            throw std::runtime_error("Client must load the packaged native/native.dll beside this probe");
        const Api api(module);
        const hmi::engine::detail::ScopedSdkThreads threads(4);
        const auto model_path = hmi::engine::default_model_path("tts.gguf");
        const auto load_started = Clock::now();
        const audiocpp_model_config selection{"qwen3_tts", nullptr, nullptr, nullptr};
        audiocpp_model *raw_model = nullptr;
        const auto model_status = api.model_load(client.registry(), model_path.c_str(), &selection, nullptr, &raw_model);
        std::unique_ptr<audiocpp_model, decltype(api.model_free)> model(raw_model, api.model_free);
        api.check(model_status, "load Qwen3-TTS");
        if (!model) throw std::runtime_error("native SDK returned no model");
        const audiocpp_backend_config backend{"cuda", 0, 4};
        audiocpp_session *raw_session = nullptr;
        const auto session_status = api.session_create(model.get(), "tts", "offline", &backend, nullptr, &raw_session);
        std::unique_ptr<audiocpp_session, decltype(api.session_free)> session(raw_session, api.session_free);
        api.check(session_status, "create Qwen3-TTS CUDA session");
        if (!session) throw std::runtime_error("native SDK returned no session");
        const auto load_ms = std::chrono::duration<double, std::milli>(Clock::now() - load_started).count();
        const std::string run_id = "tts-quality-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
        const std::pair<const char *, std::string> settings[]{
            {"max_tokens", std::to_string(selected.budget)}, {"seed", std::to_string(selected.seed)},
            {"do_sample", selected.main_sampling ? "true" : "false"},
            {"subtalker_do_sample", selected.subtalker_sampling ? "true" : "false"},
            {"temperature", "0.9"}, {"top_k", "50"}, {"top_p", "1.0"}, {"repetition_penalty", "1.05"},
            {"subtalker_temperature", "0.9"}, {"subtalker_top_k", "50"}, {"subtalker_top_p", "1.0"},
            {"text_chunk_size", "8192"}, {"instruct", selected.instruction}};
        Value::Object option_fields{{"language", Value(selected.language)}, {"voice_id", Value("serena")}};
        for (const auto &[key, value] : settings) option_fields.emplace(key, Value(value));
        Value::Array records;
        int failures = 0;
        for (int iteration = 1; iteration <= selected.iterations; ++iteration)
        {
            std::unique_ptr<audiocpp_request, decltype(api.request_free)> request(api.request_create(), api.request_free);
            if (!request) throw std::runtime_error("cannot allocate native request");
            api.check(api.set_text(request.get(), selected.text.c_str(), selected.language.c_str()), "set original text");
            api.check(api.set_voice(request.get(), "serena"), "set speaker");
            for (const auto &[key, value] : settings)
                api.check(api.set_option(request.get(), key, value.c_str()), "set diagnostic request option");
            const auto request_id = std::to_string(iteration);
            api.check(api.set_option(request.get(), "hmi.request_id", request_id.c_str()), "set request trace");
            api.check(api.set_option(request.get(), "hmi.segment_index", "1"), "set segment trace");
            api.check(api.set_option(request.get(), "hmi.attempt", "1"), "set attempt trace");
            audiocpp_result *raw_result = nullptr;
            const auto started = Clock::now();
            const auto status = api.run(session.get(), request.get(), &raw_result);
            const auto run_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            std::unique_ptr<audiocpp_result, decltype(api.result_free)> result(raw_result, api.result_free);
            api.check(status, "run Qwen3-TTS");
            if (!result) throw std::runtime_error("native SDK returned no result");
            const float *samples = nullptr;
            std::size_t frames = 0;
            int rate = 0, channels = 0;
            api.check(api.audio(result.get(), &samples, &frames, &rate, &channels), "read native PCM");
            if (!samples || !frames || rate != 24000 || channels != 1 || frames > 24000U * 300U)
                throw std::runtime_error("quality probe expects finite, nonempty 24 kHz mono PCM shorter than 300 seconds");
            for (std::size_t index = 0; index < frames; ++index)
                if (!std::isfinite(samples[index])) throw std::runtime_error("native PCM contains a non-finite sample");
            const auto bytes = frames * sizeof(float);
            const auto pcm_name = "iteration-" + std::to_string(iteration) + ".f32";
            std::ofstream pcm(directory / pcm_name, std::ios::binary | std::ios::trunc);
            pcm.write(reinterpret_cast<const char *>(samples), static_cast<std::streamsize>(bytes));
            pcm.close();
            if (!pcm) throw std::runtime_error("cannot save raw diagnostic PCM");
            const auto metadata = generation_metadata(api, result.get());
            const auto reason = metadata_text(metadata, "finish_reason");
            const bool success = reason == "eos" && metadata_text(metadata, "generation_chunks") == "1" &&
                metadata_text(metadata, "max_tokens") == std::to_string(selected.budget);
            if (!success) ++failures;
            Value record(Value::Object{
                {"event", Value("tts_quality_probe")}, {"schema", Value(1.0)}, {"run_id", Value(run_id)},
                {"request_id", Value(request_id)}, {"success", Value(success)},
                {"text", Value(selected.text)}, {"text_bytes", Value(static_cast<double>(selected.text.size()))},
                {"text_hash", Value(std::to_string(fingerprint(selected.text.data(), selected.text.size())))},
                {"options", Value(option_fields)}, {"backend", Value("cuda")}, {"threads", Value(4.0)},
                {"speaker", Value("serena")}, {"model", Value(model_path)},
                {"sdk_dll", Value(utf8(std::wstring_view(actual, count)))},
                {"sdk_build", Value(client.version().build)}, {"sdk_abi", Value(static_cast<double>(client.version().abi))},
                {"session_load_ms", Value(load_ms)}, {"run_ms", Value(run_ms)},
                {"finish_reason", reason.empty() ? Value{} : Value(reason)}, {"generation", Value(metadata)},
                {"pcm_file", Value(pcm_name)}, {"format", Value("float32_le_interleaved")},
                {"sample_rate", Value(static_cast<double>(rate))}, {"channels", Value(static_cast<double>(channels))},
                {"pcm_frames", Value(static_cast<double>(frames))}, {"pcm_bytes", Value(static_cast<double>(bytes))},
                {"audio_seconds", Value(static_cast<double>(frames) / rate)},
                {"pcm_hash_fnv1a64", Value(std::to_string(fingerprint(samples, bytes)))},
                {"host_splits", Value(0.0)}, {"retries", Value(0.0)}, {"playback", Value(false)}});
            write_json(directory / ("iteration-" + std::to_string(iteration) + ".json"), record);
            records.push_back(record);
            write_json(directory / "summary.json", Value(records));
            const auto json = hmi::util::json::stringify(record);
            logger.write(success ? hmi::base::LogLevel::Info : hmi::base::LogLevel::Warning, "quality_probe", json);
            logger.flush();
            std::printf("%s\n", json.c_str());
            std::fflush(stdout);
        }
        session.reset();
        model.reset();
        // The OpenMP guard restores its calling-thread state before Client unloads the SDK.
        logger.flush();
        return failures ? 1 : 0;
    }
    catch (const std::exception &exception)
    {
        std::fprintf(stderr, "FAIL %s\n", exception.what());
        return 2;
    }
}
