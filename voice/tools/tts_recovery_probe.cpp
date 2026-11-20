#include "engine/tts_session.hpp"
#include "audio/stream_playback.hpp"
#include "audio/playback_diagnostics.hpp"
#include "base/logger.hpp"
#include "base/project_paths.hpp"
#include "util/json_writer.hpp"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace
{
using Clock = std::chrono::steady_clock;
using hmi::util::json::Value;

std::string utf8(std::wstring_view value)
{
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("invalid Unicode argument");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), count, nullptr, nullptr) != count) throw std::runtime_error("cannot encode argument");
    return result;
}

struct Options
{
    std::string text;
    std::string language = "zh";
    std::string instruction;
    std::filesystem::path output;
    int iterations = 1;
    int prepare_wait_ms = -1;
    bool cache = false;
    bool help = false;
};

Options options(int argc, wchar_t **argv)
{
    Options selected;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring key = argv[index];
        if (key == L"--help") { selected.help = true; continue; }
        if (key == L"--cache0" || key == L"--cache1") { selected.cache = key == L"--cache1"; continue; }
        if (++index == argc) throw std::runtime_error("missing option value");
        const std::wstring value = argv[index];
        if (key == L"--text") selected.text = utf8(value);
        else if (key == L"--instruct") selected.instruction = utf8(value);
        else if (key == L"--language")
        {
            if (value != L"zh" && value != L"en" && value != L"auto")
                throw std::runtime_error("language must be zh, en or auto");
            selected.language = utf8(value);
        }
        else if (key == L"--output") selected.output = value;
        else if (key == L"--iterations")
        {
            if (value.size() != 1 || value.front() < L'1' || value.front() > L'3')
                throw std::runtime_error("iterations must be 1..3");
            selected.iterations = static_cast<int>(value.front() - L'0');
        }
        else if (key == L"--cache")
        {
            if (value != L"0" && value != L"1") throw std::runtime_error("cache must be 0 or 1");
            selected.cache = value == L"1";
        }
        else if (key == L"--prepare-wait-ms")
        {
            if (value.empty() || value.size() > 5) throw std::runtime_error("prepare wait must be 0..45000 ms");
            int wait_ms = 0;
            for (const auto digit : value)
            {
                if (digit < L'0' || digit > L'9') throw std::runtime_error("prepare wait must be 0..45000 ms");
                wait_ms = wait_ms * 10 + static_cast<int>(digit - L'0');
            }
            if (wait_ms > 45000) throw std::runtime_error("prepare wait must be 0..45000 ms");
            selected.prepare_wait_ms = wait_ms;
        }
        else throw std::runtime_error("unknown option");
    }
    if (!selected.help && (selected.text.empty() || selected.text.size() > 4096 || selected.output.empty()))
        throw std::runtime_error("--text (1..4096 UTF-8 bytes) and --output are required");
    return selected;
}

std::filesystem::path output_directory(const std::filesystem::path &requested)
{
    const auto root = hmi::base::project_root(hmi::base::process_directory());
    std::filesystem::path resolved;
    std::string error;
    if (!hmi::base::resolve_project_path(requested.is_absolute() ? requested : root / requested,
            hmi::base::process_directory(), resolved, &error)) throw std::runtime_error(error);
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

// Re-read immutable process arguments instead of introducing a mutable sink singleton.
Options process_options()
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) throw std::runtime_error("cannot read probe command line");
    struct Arguments { wchar_t **value; ~Arguments() { LocalFree(value); } } arguments{argv};
    return options(argc, argv);
}

void write_json(const std::filesystem::path &path, const Value &value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << hmi::util::json::stringify(value) << '\n';
    if (!output) throw std::runtime_error("cannot write probe JSON");
}

// Only this opt-in probe is terminated if an SDK call blocks indefinitely.
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
}

namespace hmi::audio::detail
{
// Link-time substitution: no WASAPI client, audio device, or playback-duration wait.
bool play_stream(const ReadAudio &read, std::string *error, const std::function<bool()> &cancelled,
                 PlaybackStatistics *out)
{
    const auto started = Clock::now();
    PlaybackStatistics measured;
    measured.endpoint_id = "probe:fast_sink";
    bool ok = false;
    if (error) error->clear();
    try
    {
        const auto selected = process_options();
        const auto directory = output_directory(selected.output);
        std::filesystem::path pcm_path;
        for (int index = 1; index <= 3; ++index)
        {
            const auto candidate = directory / ("iteration-" + std::to_string(index) + ".f32");
            if (!std::filesystem::exists(candidate)) { pcm_path = candidate; break; }
        }
        if (pcm_path.empty()) throw std::runtime_error("no unused iteration PCM file");
        std::ofstream pcm(pcm_path, std::ios::binary | std::ios::trunc);
        if (!pcm) throw std::runtime_error("cannot create consumed PCM file");
        for (;;)
        {
            if (cancelled && cancelled())
            {
                measured.finish_reason = "interrupted";
                if (error) *error = "probe sink interrupted";
                break;
            }
            AudioBuffer audio;
            const auto state = read(audio, error);
            if (state == StreamRead::Failed) { measured.finish_reason = "source_failed"; break; }
            if (state == StreamRead::Finished) { ok = measured.accepted_chunks != 0; measured.drained = ok; break; }
            if (state == StreamRead::Pending)
            {
                ++measured.pending_reads;
                const auto waiting = Clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                measured.source_wait_ms += std::chrono::duration<double, std::milli>(Clock::now() - waiting).count();
                continue;
            }
            if (audio.sample_rate < 8000 || audio.sample_rate > 192000 || audio.channels < 1 || audio.channels > 8 ||
                audio.samples.empty() || audio.samples.size() % static_cast<std::size_t>(audio.channels) != 0)
                throw std::runtime_error("invalid probe sink PCM");
            if (measured.accepted_chunks && (measured.sample_rate != audio.sample_rate || measured.channels != audio.channels))
                throw std::runtime_error("probe sink PCM format changed");
            for (const auto sample : audio.samples)
                if (!std::isfinite(sample)) throw std::runtime_error("non-finite probe sink PCM");
            measured.sample_rate = audio.sample_rate;
            measured.channels = audio.channels;
            pcm.write(reinterpret_cast<const char *>(audio.samples.data()),
                static_cast<std::streamsize>(audio.samples.size() * sizeof(float)));
            if (!pcm) throw std::runtime_error("cannot save consumed PCM");
            const auto frames = audio.samples.size() / static_cast<std::size_t>(audio.channels);
            measured.submitted_frames += frames;
            measured.consumed_frames += frames;
            measured.padding_available = true; // The virtual sink consumes each submitted chunk immediately.
            ++measured.accepted_chunks;
            ++measured.completed_chunks;
        }
        pcm.flush();
        if (!pcm) throw std::runtime_error("cannot flush consumed PCM");
        if (measured.finish_reason.empty()) measured.finish_reason = ok ? "drained" : "failed";
        write_json(pcm_path.parent_path() / (pcm_path.stem().string() + ".json"), Value(Value::Object{
            {"format", Value("float32_le_interleaved")}, {"sink", Value("fast_sink")},
            {"sample_rate", measured.sample_rate ? Value(static_cast<double>(measured.sample_rate)) : Value{}},
            {"channels", measured.channels ? Value(static_cast<double>(measured.channels)) : Value{}},
            {"frames", Value(static_cast<double>(measured.consumed_frames))}, {"complete", Value(ok)}}));
    }
    catch (const std::exception &exception)
    {
        if (error) *error = exception.what();
        measured.finish_reason = "sink_failed";
        ok = false;
    }
    measured.wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    measured.finalized = true;
    if (out) *out = std::move(measured);
    return ok;
}

bool play_stream(const ReadAudio &read, std::string *error, const std::function<bool()> &cancelled)
{
    return play_stream(read, error, cancelled, nullptr);
}
}

int wmain(int argc, wchar_t **argv)
{
    try
    {
        const auto selected = options(argc, argv);
        if (selected.help)
        {
            std::puts("hmi_tts_recovery_probe --text TEXT --output build/DIR [--iterations 1..3] [--cache 0|1] [--language zh|en|auto] [--instruct TEXT]\n"
                      "                       [--prepare-wait-ms 0..45000]\n"
                      "Real TTS worker; silent fast sink; output must be a fresh build subdirectory.\n"
                      "Optional load-only preparation runs before each speech request; the 120-second watchdog covers the whole run.");
            return 0;
        }
        const auto directory = output_directory(selected.output);
        if (std::filesystem::exists(directory) && !std::filesystem::is_empty(directory))
            throw std::runtime_error("output directory must be empty to preserve previous evidence");
        std::filesystem::create_directories(directory);
        if (!SetEnvironmentVariableW(L"HMI_TTS_PCM_CACHE", selected.cache ? L"1" : L"0"))
            throw std::runtime_error("cannot configure probe cache");
        Deadline deadline;
        hmi::base::Logger logger;
        hmi::base::LoggerOptions logging;
        logging.file = directory / "worker.log";
        logging.level = hmi::base::LogLevel::Debug;
        std::string error;
        if (!logger.configure(logging, &error)) throw std::runtime_error(error);
        hmi::engine::Client client;
        if (!client.open(&error, {}, &logger)) throw std::runtime_error(error);
        hmi::engine::TtsSession session;
        hmi::engine::TtsConfig config;
        config.backend = "cuda";
        config.threads = 4;
        config.language = selected.language;
        config.instruction = selected.instruction;
        config.logger = &logger;
        config.run_id = "tts-recovery-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
        if (!session.open(client, config, &error)) throw std::runtime_error(error);
        Value::Array results;
        int failures = 0;
        for (int iteration = 1; iteration <= selected.iterations; ++iteration)
        {
            const auto request_id = static_cast<std::uint64_t>(iteration);
            const bool prepare_requested = selected.prepare_wait_ms >= 0;
            double prepare_queue_call_ms = 0.0;
            double preparation_ready_observed_ms = -1.0;
            auto prepare_started = Clock::time_point{};
            if (prepare_requested)
            {
                prepare_started = Clock::now();
                if (!session.prepare(request_id)) throw std::runtime_error("model preparation was rejected");
                prepare_queue_call_ms = std::chrono::duration<double, std::milli>(Clock::now() - prepare_started).count();
                const auto wait_deadline = Clock::now() + std::chrono::milliseconds(selected.prepare_wait_ms);
                for (;;)
                {
                    if (session.statistics().model_ready)
                    {
                        preparation_ready_observed_ms = std::chrono::duration<double, std::milli>(Clock::now() - prepare_started).count();
                        std::this_thread::sleep_until(wait_deadline);
                        break;
                    }
                    if (Clock::now() >= wait_deadline) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            const bool model_ready_at_speak = session.statistics().model_ready;
            const auto started = Clock::now();
            const double preparation_lead_ms = prepare_requested ?
                std::chrono::duration<double, std::milli>(started - prepare_started).count() : 0.0;
            if (!session.speak(selected.text, request_id)) throw std::runtime_error(session.last_error());
            while (session.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto state = session.statistics();
            const auto failure = session.last_error();
            const bool success = failure.empty() && state.pipeline_finish_reason == "completed" && state.playback.drained;
            if (!success) ++failures;
            Value result(Value::Object{
                {"event", Value("tts_recovery_probe")}, {"schema", Value(1.0)}, {"run_id", Value(config.run_id)},
                {"request_id", Value(std::to_string(request_id))}, {"success", Value(success)}, {"detail", Value(failure)},
                {"text", Value(selected.text)}, {"text_bytes", Value(static_cast<double>(selected.text.size()))},
                {"backend", Value(state.backend)}, {"threads", Value(4.0)}, {"cache_enabled", Value(selected.cache)},
                {"language", Value(selected.language)},
                {"prepare_requested", Value(prepare_requested)},
                {"prepare_wait_ms", prepare_requested ? Value(static_cast<double>(selected.prepare_wait_ms)) : Value{}},
                {"prepare_queue_call_ms", prepare_requested ? Value(prepare_queue_call_ms) : Value{}},
                {"preparation_lead_ms", prepare_requested ? Value(preparation_lead_ms) : Value{}},
                {"preparation_ready_observed_ms", preparation_ready_observed_ms >= 0.0 ? Value(preparation_ready_observed_ms) : Value{}},
                {"model_ready_at_speak", Value(model_ready_at_speak)},
                {"segments", Value(static_cast<double>(state.segments))},
                {"generated_segments", Value(static_cast<double>(state.generated_segments))},
                {"generated_audio_frames", Value(static_cast<double>(state.generated_audio_frames))},
                {"audio_seconds", Value(state.audio_seconds)},
                {"first_pcm_ms", state.generated_segments ? Value(state.first_audio_ms) : Value{}},
                {"first_pcm_origin", Value("speak_accepted")},
                {"playback_timing_origin", Value("play_stream_started")},
                {"synthesis_ms", Value(state.synthesis_ms)}, {"model_load_ms", Value(state.model_load_ms)},
                {"worker_queue_ms", Value(state.queue_ms)},
                {"wall_ms", Value(std::chrono::duration<double, std::milli>(Clock::now() - started).count())},
                {"limit_hits", Value(static_cast<double>(state.limit_hits))},
                {"generation_retries", Value(static_cast<double>(state.generation_retries))},
                {"normal_sampling", Value(state.sampling_enabled)},
                {"sampling_fallbacks", Value(static_cast<double>(state.sampling_fallbacks))},
                {"recovery_splits", Value(static_cast<double>(state.recovery_splits))},
                {"recovery_attempts", Value(static_cast<double>(state.recovery_attempts))},
                {"recovery_exhausted", Value(state.recovery_exhausted)},
                {"cache_hits", Value(static_cast<double>(state.utterance_cache_hits))},
                {"sdk_finish_reason", Value(state.last_finish_reason)},
                {"pipeline_finish_reason", Value(state.pipeline_finish_reason)},
                {"pcm_file", Value("iteration-" + std::to_string(iteration) + ".f32")},
                {"consumed_bytes", Value(static_cast<double>(state.playback.consumed_frames) *
                    static_cast<double>(state.playback.channels) * static_cast<double>(sizeof(float)))},
                {"playback_final", Value(state.playback.finalized)},
                {"playback", hmi::audio::detail::playback_fields(state.playback)}});
            const auto json = hmi::util::json::stringify(result);
            logger.write(success ? hmi::base::LogLevel::Info : hmi::base::LogLevel::Warning, "probe", json);
            std::printf("%s\n", json.c_str());
            std::fflush(stdout);
            results.push_back(std::move(result));
            write_json(directory / "summary.json", Value(results));
            logger.flush();
        }
        session.stop();
        client.close();
        logger.flush();
        return failures ? 1 : 0;
    }
    catch (const std::exception &exception)
    {
        std::fprintf(stderr, "FAIL %s\n", exception.what());
        return 2;
    }
}
