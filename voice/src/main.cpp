#include "app/settings.hpp"
#include "app/voice_runtime.hpp"
#include "base/config.hpp"
#include "base/project_paths.hpp"
#include "server/inference_http.hpp"
#include "server/inference_session.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
using namespace std::chrono_literals;

std::atomic_bool stop_requested = false;

BOOL WINAPI console_signal(DWORD signal)
{
    if (signal != CTRL_C_EVENT && signal != CTRL_BREAK_EVENT) return FALSE;
    stop_requested.store(true);
    return TRUE;
}

struct LaunchOptions
{
    bool http = false;
    bool help = false;
    bool ble = false;
    bool verbose = false;
    bool terminal_input = false;
    std::string board_host;
    int board_port = 0;
    hmi::server::HttpOptions server;
};

std::string environment(const char *name)
{
    char *value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
    return value;
}

LaunchOptions parse_arguments(int argc, char **argv)
{
    LaunchOptions options;
    bool changed_http_address = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") options.help = true;
        else if (argument == "--http") options.http = true;
        else if (argument == "--ble") options.ble = true;
        else if (argument == "--verbose") options.verbose = true;
        else if (argument == "--terminal-input") options.terminal_input = true;
        else if (argument == "--esp32-host" || argument == "--esp32-port" ||
                 argument == "--bind" || argument == "--port")
        {
            if (++index == argc || argv[index][0] == '\0')
                throw std::runtime_error(std::string(argument) + " requires a value");
            const std::string_view value(argv[index]);
            if (argument == "--esp32-host") options.board_host = value;
            else if (argument == "--bind")
            {
                options.server.host = value;
                changed_http_address = true;
            }
            else
            {
                int port = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    port < 1 || port > 65535)
                    throw std::runtime_error(std::string(argument) + " must be an integer from 1 to 65535");
                if (argument == "--esp32-port") options.board_port = port;
                else
                {
                    options.server.port = port;
                    changed_http_address = true;
                }
            }
        }
        else throw std::runtime_error("unknown argument: " + std::string(argument));
    }
    if (changed_http_address && !options.http) throw std::runtime_error("--bind and --port require --http");
    if (options.http && (options.ble || !options.board_host.empty() || options.board_port != 0))
        throw std::runtime_error("--ble, --esp32-host and --esp32-port are for ESP32-S3 mode");
    if (options.http && options.terminal_input)
        throw std::runtime_error("--terminal-input requires ESP32-S3 mode");
    if (options.ble && !options.board_host.empty()) throw std::runtime_error("use either --ble or --esp32-host");
    if (options.ble)
    {
        options.board_host = "127.0.0.1";
        if (options.board_port == 0) options.board_port = 5000;
    }
    options.server.bearer_token = environment("HMI_HTTP_TOKEN");
    options.server.allowed_origin = environment("HMI_HTTP_ORIGIN");
    return options;
}

hmi::app::Settings load_settings()
{
    const auto directory = hmi::app::executable_directory();
    auto settings = hmi::app::default_settings(directory);
    settings.voice.audio_source.source = "esp32";
    auto file = hmi::app::settings_path();
    std::string error;
    if (!hmi::base::resolve_project_path(file, directory, file, &error))
        throw std::runtime_error("invalid voice configuration path: " + error);
    const bool default_file = file == std::filesystem::weakly_canonical(directory / "config/app.yaml");
    if (!std::filesystem::exists(file) && default_file &&
        !hmi::app::write_default_settings(file, settings, directory, &error))
        throw std::runtime_error("cannot create voice configuration: " + error);

    hmi::base::Config config;
    if (!config.load(file, &error) || !hmi::app::read_settings(config, directory, settings, &error) ||
        !hmi::app::validate_settings_paths(settings, file, directory, &error))
        throw std::runtime_error("cannot load voice configuration: " + error);
    return settings;
}

std::string console_line(std::string_view value)
{
    std::string line(value);
    for (char &character : line)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7f) character = ' ';
    }
    return line;
}

int run_board(hmi::app::VoiceOptions options, hmi::base::Logger &logger)
{
    options.mode = hmi::app::VoiceMode::BoardDictation;
    options.audio_source.source = "esp32";
    // The current board switch, sent with the link heartbeat, owns readback.
    options.tts_enabled = false;
    options.tts_prewarm = false;
    std::printf("[启动] voice | ESP32-S3 语音转写 | %s %s:%d\n"
                "[模式] 转写结果输入当前文本框。在手表开启「语音朗读」可听回放。\n"
                "[操作] Ctrl+C 退出\n",
                options.audio_source.esp32_host == "127.0.0.1" ? "BLE 转发" : "Wi-Fi UDP",
                options.audio_source.esp32_host.c_str(), options.audio_source.esp32_port);
    std::fflush(stdout);
    hmi::app::VoiceRuntime runtime(&logger);
    std::string error;
    if (!runtime.start(options, &error)) throw std::runtime_error(error);

    auto next_status = std::chrono::steady_clock::now();
    std::string status;
    std::string preview;
    bool board_connected = false;
    bool tts_enabled = false;
    bool tts_busy = false;
    bool model_reported = false;
    std::uint64_t request_id = 0;
    std::deque<std::uint64_t> printed_finals;
    const auto print_final = [&printed_finals](std::uint64_t id, std::string_view value) {
        if (id == 0 || value.empty() ||
            std::find(printed_finals.begin(), printed_finals.end(), id) != printed_finals.end()) return;
        printed_finals.push_back(id);
        if (printed_finals.size() > 64) printed_finals.pop_front();
        std::printf("[转写 #%llu] %s\n", static_cast<unsigned long long>(id), console_line(value).c_str());
    };
    const auto starting_view = runtime.snapshot();
    std::uint64_t last_event = starting_view.recent.empty() ? 0 : starting_view.recent.back().sequence;
    std::puts("[连接] 等待板端");
    while (!stop_requested.load())
    {
        runtime.tick();
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_status)
        {
            next_status = now + 100ms;
            auto snapshot = runtime.snapshot();
            if (snapshot.status != status)
            {
                status = std::move(snapshot.status);
                std::printf("[状态] %s\n", console_line(status).c_str());
            }
            if (!model_reported && snapshot.model_ready)
            {
                model_reported = true;
                std::printf("[模型] %s | %d 线程\n", snapshot.backend.c_str(), snapshot.inference_threads);
            }
            if (std::exchange(board_connected, snapshot.board_connected) != snapshot.board_connected)
                std::puts(board_connected ? "[连接] 板端已连通" : "[连接] 板端已断开，等待重连");
            if (std::exchange(tts_enabled, snapshot.tts_enabled) != snapshot.tts_enabled)
                std::puts(tts_enabled ? "[模式] 转写并朗读" : "[模式] 只转写");

            if (request_id != snapshot.transcription_request_id)
            {
                request_id = snapshot.transcription_request_id;
                preview.clear();
                if (request_id != 0) std::printf("[录音 #%llu] 已开始\n", static_cast<unsigned long long>(request_id));
            }
            if (request_id != 0 && !snapshot.last_transcript.empty())
            {
                const auto rendered = console_line(snapshot.last_transcript);
                if (snapshot.transcript_final && !snapshot.awaiting_release)
                    print_final(request_id, snapshot.last_transcript);
                else if (!snapshot.transcript_final && preview != rendered)
                {
                    preview = rendered;
                    std::printf("[预览 #%llu] %s\n", static_cast<unsigned long long>(request_id), preview.c_str());
                }
            }
            if (!std::exchange(tts_busy, snapshot.tts_busy) && tts_busy)
                std::puts("[朗读] 正在准备并发送语音");
            for (const auto &event : snapshot.recent)
            {
                if (event.sequence <= last_event) continue;
                last_event = event.sequence;
                const auto id = static_cast<unsigned long long>(event.request_id);
                print_final(event.request_id, event.transcript);
                if (event.action == "text")
                {
                    if (!event.ok || event.cancelled || event.skipped)
                        std::printf("[输入 #%llu] 写入未完成 | %s\n", id, console_line(event.detail).c_str());
                    else
                        std::printf("[输入 #%llu] %s | %s\n", id,
                            event.input_delivery == "verified" ? "已确认写入" : "已发送文本",
                            console_line(event.detail).c_str());
                }
                else if (event.cancelled || !event.ok || event.skipped)
                    std::printf("[结果 #%llu] %s\n", id, console_line(event.detail).c_str());
                else if (event.action == "speak")
                    std::printf("[朗读 #%llu] 合成完成，语音已提交板端播放 | %.0f ms\n", id, event.tts_ms);
                if (event.action != "speak" && event.asr_ms >= 0)
                    std::printf("[识别 #%llu] %.0f ms\n", id, event.asr_ms);
            }
            std::fflush(stdout);
            if (!snapshot.model_ready && (snapshot.state == hmi::app::VoiceState::Unavailable ||
                                         snapshot.state == hmi::app::VoiceState::Error)) return 1;
        }
        std::this_thread::sleep_for(20ms);
    }
    runtime.stop();
    return 0;
}

int run_http(const hmi::app::VoiceOptions &voice, const hmi::server::HttpOptions &options)
{
    std::string error;
    hmi::server::InferenceSession inference;
    if (!inference.open(voice, &error)) throw std::runtime_error(error);
    hmi::server::HttpServer server(inference, options);
    if (!server.bind(&error)) throw std::runtime_error(error);
    std::printf("HTTP inference | %s:%d | Ctrl+C to stop\n", options.host.c_str(), options.port);
    std::fflush(stdout);
    std::jthread shutdown([&server](const std::stop_token &done) {
        while (!done.stop_requested())
        {
            if (stop_requested.load())
            {
                server.stop();
                break;
            }
            std::this_thread::sleep_for(50ms);
        }
    });
    return server.listen() || stop_requested.load() ? 0 : 1;
}
}

int main(int argc, char **argv)
{
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetConsoleOutputCP(CP_UTF8);
    try
    {
        const auto launch = parse_arguments(argc, argv);
        if (launch.help)
        {
            std::puts("voice [--ble | --esp32-host IPv4] [--esp32-port number] [--terminal-input] [--verbose]\n"
                      "voice --http [--bind address] [--port number] [--verbose]\n"
                      "--ble connects to the local Python BLE bridge (default port 5000).\n"
                      "--terminal-input permits dictation into the focused Windows Terminal pane.\n"
                      "--verbose also prints the configured diagnostic log to the console.\n"
                      "ESP32-S3 transcription is sent to the focused editor. Select readback on the watch to enable TTS.\n"
                      "HTTP defaults to 127.0.0.1:8080. Set HMI_HTTP_TOKEN for remote connections.");
            return 0;
        }
        if (!SetConsoleCtrlHandler(console_signal, TRUE))
            throw std::runtime_error("cannot register console shutdown handler");
        auto settings = load_settings();
        if (launch.verbose) settings.logging.console = true;
        hmi::base::Logger logger;
        std::string error;
        if (!logger.configure(settings.logging, &error)) std::fprintf(stderr, "voice logging: %s\n", error.c_str());
        else
        {
            const auto log_file = settings.logging.file.u8string();
            std::printf("[日志] %s\n", reinterpret_cast<const char *>(log_file.c_str()));
        }
        if (launch.http) return run_http(settings.voice, launch.server);
        settings.voice.allow_terminal_input = launch.terminal_input;
        if (!launch.board_host.empty()) settings.voice.audio_source.esp32_host = launch.board_host;
        if (launch.board_port != 0) settings.voice.audio_source.esp32_port = launch.board_port;
        return run_board(settings.voice, logger);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "voice: %s\n", error.what());
        return 1;
    }
}
