// Include the transport before SDK headers that may include windows.h.
#include <httplib.h>

#include "server/inference_http.hpp"
#include "server/inference_session.hpp"
#include "util/json_reader.hpp"
#include "util/json_writer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace hmi::server
{
namespace
{
constexpr std::size_t kMaximumBodyBytes = 1024 * 1024;
constexpr std::size_t kMaximumJsonBytes = 32 * 1024;
constexpr std::size_t kMaximumOutputBytes = 32 * 1024 * 1024;

using util::json::Value;

void json_response(httplib::Response &response, int status, Value::Object object)
{
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_content(util::json::stringify(Value(std::move(object))), "application/json; charset=utf-8");
}

void error_response(httplib::Response &response, int status, const char *message)
{
    json_response(response, status, {{"error", Value(message)}});
}

bool loopback_host(std::string_view host)
{
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

bool valid_token(std::string_view token)
{
    return token.size() <= 1024 && std::all_of(token.begin(), token.end(), [](unsigned char c) {
        return c > 0x20 && c < 0x7f;
    });
}

bool equal_token(std::string_view provided, std::string_view expected)
{
    if (provided.size() != expected.size()) return false;
    unsigned char difference = 0;
    for (std::size_t index = 0; index < expected.size(); ++index)
        difference |= static_cast<unsigned char>(provided[index] ^ expected[index]);
    return difference == 0;
}

bool media_type(const httplib::Request &request, std::string_view expected)
{
    if (request.get_header_value_count("Content-Type") != 1) return false;
    const std::string value = request.get_header_value("Content-Type");
    const std::size_t end = value.find(';');
    std::string_view type(value.data(), end == std::string::npos ? value.size() : end);
    const auto last = type.find_last_not_of(" \t");
    if (last == std::string_view::npos) return false;
    type = type.substr(0, last + 1);
    const auto first = type.find_first_not_of(" \t");
    type.remove_prefix(first);
    if (type.size() != expected.size()) return false;
    return std::equal(type.begin(), type.end(), expected.begin(), [](unsigned char a, unsigned char b) {
        return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b;
    });
}

std::uint16_t read_u16(const char *bytes)
{
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0])) |
           static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[1]) << 8);
}

std::uint32_t read_u32(const char *bytes)
{
    return static_cast<std::uint32_t>(read_u16(bytes)) |
           (static_cast<std::uint32_t>(read_u16(bytes + 2)) << 16);
}

bool decode_wav(std::string_view body, std::vector<float> &pcm)
{
    if (body.size() < 44 || body.substr(0, 4) != "RIFF" || body.substr(8, 4) != "WAVE" ||
        static_cast<std::uint64_t>(read_u32(body.data() + 4)) + 8 != body.size()) return false;

    bool format_seen = false;
    bool data_seen = false;
    std::string_view data;
    std::size_t cursor = 12;
    while (cursor < body.size())
    {
        if (body.size() - cursor < 8) return false;
        const auto name = body.substr(cursor, 4);
        const std::size_t length = read_u32(body.data() + cursor + 4);
        cursor += 8;
        if (length > body.size() - cursor) return false;
        const char *chunk = body.data() + cursor;
        if (name == "fmt ")
        {
            if (format_seen || length < 16 || read_u16(chunk) != 1 || read_u16(chunk + 2) != 1 ||
                read_u32(chunk + 4) != 16000 || read_u32(chunk + 8) != 32000 ||
                read_u16(chunk + 12) != 2 || read_u16(chunk + 14) != 16) return false;
            format_seen = true;
        }
        else if (name == "data")
        {
            if (data_seen || length == 0 || length % 2 != 0)
                return false;
            data = body.substr(cursor, length);
            data_seen = true;
        }
        cursor += length;
        if (length % 2 != 0)
        {
            if (cursor == body.size()) return false;
            ++cursor;
        }
    }
    if (!format_seen || !data_seen) return false;
    pcm.resize(data.size() / 2);
    for (std::size_t index = 0; index < pcm.size(); ++index)
    {
        const std::uint16_t word = read_u16(data.data() + index * 2);
        const int sample = word >= 0x8000 ? static_cast<int>(word) - 0x10000 : word;
        pcm[index] = static_cast<float>(sample) / 32768.0f;
    }
    return true;
}

void write_u16(char *bytes, std::uint16_t value)
{
    bytes[0] = static_cast<char>(value & 0xff);
    bytes[1] = static_cast<char>((value >> 8) & 0xff);
}

void write_u32(char *bytes, std::uint32_t value)
{
    write_u16(bytes, static_cast<std::uint16_t>(value));
    write_u16(bytes + 2, static_cast<std::uint16_t>(value >> 16));
}

bool encode_wav(const audio::AudioBuffer &audio, std::string &wav)
{
    if (audio.samples.empty() || audio.channels != 1 || audio.sample_rate < 8000 ||
        audio.sample_rate > 192000 || audio.samples.size() > (kMaximumOutputBytes - 44) / 2 ||
        !std::all_of(audio.samples.begin(), audio.samples.end(), [](float value) {
            return std::isfinite(value);
        })) return false;
    const std::size_t bytes = audio.samples.size() * 2;
    wav.assign(44 + bytes, '\0');
    wav.replace(0, 4, "RIFF");
    write_u32(wav.data() + 4, static_cast<std::uint32_t>(36 + bytes));
    wav.replace(8, 8, "WAVEfmt ");
    write_u32(wav.data() + 16, 16);
    write_u16(wav.data() + 20, 1);
    write_u16(wav.data() + 22, 1);
    write_u32(wav.data() + 24, static_cast<std::uint32_t>(audio.sample_rate));
    write_u32(wav.data() + 28, static_cast<std::uint32_t>(audio.sample_rate * 2));
    write_u16(wav.data() + 32, 2);
    write_u16(wav.data() + 34, 16);
    wav.replace(36, 4, "data");
    write_u32(wav.data() + 40, static_cast<std::uint32_t>(bytes));
    for (std::size_t index = 0; index < audio.samples.size(); ++index)
    {
        const float scaled = std::clamp(audio.samples[index], -1.0f, 1.0f) * 32768.0f;
        const int sample = std::clamp(static_cast<int>(std::lround(scaled)), -32768, 32767);
        write_u16(wav.data() + 44 + index * 2, static_cast<std::uint16_t>(sample));
    }
    return true;
}
}

struct HttpServer::Impl
{
    InferenceSession &session;
    HttpOptions options;
    httplib::Server server;
    std::mutex lifecycle;
    std::atomic<bool> stopping{false};
    std::atomic<bool> listening{false};
    bool bound = false;

    Impl(InferenceSession &inference, HttpOptions config)
        : session(inference), options(std::move(config))
    {
        server.new_task_queue = [this] {
            return new httplib::ThreadPool(options.workers, options.workers, options.queued_connections);
        };
        server.set_payload_max_length(kMaximumBodyBytes);
        server.set_read_timeout(5, 0);
        server.set_write_timeout(5, 0);
        server.set_keep_alive_max_count(4);
        server.set_keep_alive_timeout(2);
        server.set_pre_routing_handler([this](const httplib::Request &request, httplib::Response &response) {
            return authorize(request, response);
        });
        server.set_exception_handler([](const httplib::Request &, httplib::Response &response,
                                        std::exception_ptr) {
            error_response(response, 500, "Internal server error");
        });
        server.set_error_handler([](const httplib::Request &, httplib::Response &response) {
            if (response.body.empty()) error_response(response, response.status, "Request failed");
        });
        server.Get("/health", [this](const httplib::Request &, httplib::Response &response) {
            const auto health = session.health();
            json_response(response, health.ready ? 200 : 503,
                {{"ready", Value(health.ready)}, {"asr_ready", Value(health.asr_ready)},
                 {"tts_ready", Value(health.tts_ready)}, {"busy", Value(health.busy)},
                 {"backend", Value(health.backend)}, {"status", Value(health.status)}});
        });
        server.Post("/v1/asr", [this](const httplib::Request &request, httplib::Response &response) {
            transcribe(request, response);
        });
        server.Post("/v1/tts", [this](const httplib::Request &request, httplib::Response &response) {
            synthesize(request, response);
        });
    }

    httplib::Server::HandlerResponse authorize(const httplib::Request &request, httplib::Response &response)
    {
        using Result = httplib::Server::HandlerResponse;
        if (stopping.load())
        {
            error_response(response, 503, "Server is stopping");
            return Result::Handled;
        }
        const std::string origin = request.get_header_value("Origin");
        if (request.get_header_value_count("Origin") > 1 ||
            (!origin.empty() && (options.allowed_origin.empty() || origin != options.allowed_origin)))
        {
            error_response(response, 403, "Origin is not allowed");
            return Result::Handled;
        }
        if (!origin.empty())
        {
            response.set_header("Access-Control-Allow-Origin", options.allowed_origin);
            response.set_header("Vary", "Origin");
        }
        if (request.method == "OPTIONS" && !origin.empty() &&
            (request.path == "/health" || request.path == "/v1/asr" || request.path == "/v1/tts"))
        {
            response.status = 204;
            response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type");
            response.set_header("Access-Control-Max-Age", "600");
            return Result::Handled;
        }
        if (!options.bearer_token.empty())
        {
            const std::string expected = "Bearer " + options.bearer_token;
            if (request.get_header_value_count("Authorization") != 1 ||
                !equal_token(request.get_header_value("Authorization"), expected))
            {
                error_response(response, 401, "Bearer token required");
                response.set_header("WWW-Authenticate", "Bearer");
                return Result::Handled;
            }
        }
        return Result::Unhandled;
    }

    void transcribe(const httplib::Request &request, httplib::Response &response)
    {
        if (!media_type(request, "audio/wav"))
        {
            error_response(response, 415, "Content-Type must be audio/wav");
            return;
        }
        std::vector<float> pcm;
        if (!decode_wav(request.body, pcm))
        {
            error_response(response, 400, "Expected PCM16 mono 16000 Hz WAV");
            return;
        }
        engine::Transcript transcript;
        std::string error;
        switch (session.transcribe(std::span<const float>(pcm), transcript, &error))
        {
        case TranscriptionStatus::InvalidAudio:
            error_response(response, 400, error.c_str());
            return;
        case TranscriptionStatus::Unavailable:
            error_response(response, 503, "Inference session is unavailable");
            return;
        case TranscriptionStatus::Failed:
            error_response(response, 500, "ASR inference failed");
            return;
        case TranscriptionStatus::Success:
            break;
        }
        Value confidence;
        if (std::isfinite(transcript.confidence) && transcript.confidence >= 0.0f)
            confidence = Value(static_cast<double>(transcript.confidence));
        json_response(response, 200, {{"text", Value(std::move(transcript.text))},
                                     {"final", Value(transcript.final)},
                                     {"confidence", std::move(confidence)}});
    }

    void synthesize(const httplib::Request &request, httplib::Response &response)
    {
        if (!media_type(request, "application/json"))
        {
            error_response(response, 415, "Content-Type must be application/json");
            return;
        }
        if (request.body.size() > kMaximumJsonBytes)
        {
            error_response(response, 413, "JSON body is too large");
            return;
        }
        Value object;
        if (!util::json::parse(request.body, object))
        {
            error_response(response, 400, "Invalid JSON body");
            return;
        }
        const Value *value = object.find("text");
        const auto *text = value == nullptr ? nullptr : std::get_if<std::string>(&value->data);
        if (text == nullptr || text->empty() || text->size() > 4096 ||
            text->find('\0') != std::string::npos || text->find_first_not_of(" \t\r\n") == std::string::npos)
        {
            error_response(response, 400, "text must contain 1 to 4096 UTF-8 bytes");
            return;
        }
        audio::AudioBuffer audio;
        std::string error;
        if (!session.synthesize(*text, audio, &error))
        {
            if (session.ready()) error_response(response, 500, "TTS inference failed");
            else error_response(response, 503, "Inference session is unavailable");
            return;
        }
        std::string wav;
        if (!encode_wav(audio, wav))
        {
            error_response(response, 500, "TTS returned unsupported audio");
            return;
        }
        response.status = 200;
        response.set_header("Cache-Control", "no-store");
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_content(std::move(wav), "audio/wav");
    }
};

HttpServer::HttpServer(InferenceSession &session, HttpOptions options)
    : impl_(std::make_unique<Impl>(session, std::move(options))) {}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::bind(std::string *error)
{
    if (error) error->clear();
    Impl &impl = *impl_;
    std::lock_guard lock(impl.lifecycle);
    if (impl.stopping.load())
    {
        if (error) *error = "HTTP server has been stopped";
        return false;
    }
    if (impl.bound) return true;
    if (impl.options.host.empty() || impl.options.port < 1 || impl.options.port > 65535 ||
        impl.options.workers < 1 || impl.options.workers > 8 ||
        impl.options.queued_connections < 1 || impl.options.queued_connections > 32 ||
        !valid_token(impl.options.bearer_token) ||
        (!loopback_host(impl.options.host) && impl.options.bearer_token.empty()))
    {
        if (error) *error = "Invalid HTTP options or missing token for a non-loopback address";
        return false;
    }
    const auto &origin = impl.options.allowed_origin;
    if (!origin.empty() && ((!origin.starts_with("http://") && !origin.starts_with("https://")) ||
        origin.size() > 2048 || origin.find_first_of("\r\n\t ") != std::string::npos ||
        origin.size() == origin.find("://") + 3 ||
        origin.find_first_of("/?#", origin.find("://") + 3) != std::string::npos))
    {
        if (error) *error = "Allowed origin must be one exact HTTP or HTTPS origin";
        return false;
    }
    if (!impl.server.bind_to_port(impl.options.host, impl.options.port))
    {
        if (error) *error = "Could not bind HTTP address and port";
        return false;
    }
    impl.bound = true;
    return true;
}

bool HttpServer::listen()
{
    Impl &impl = *impl_;
    {
        std::lock_guard lock(impl.lifecycle);
        if (!impl.bound || impl.stopping.load() || impl.listening.exchange(true)) return false;
    }
    bool result = false;
    try { result = impl.server.listen_after_bind(); }
    catch (...)
    {
        impl.server.stop();
    }
    {
        std::lock_guard lock(impl.lifecycle);
        impl.bound = false;
        impl.listening.store(false);
    }
    return result;
}

void HttpServer::stop()
{
    Impl &impl = *impl_;
    {
        std::lock_guard lock(impl.lifecycle);
        impl.stopping.store(true);
    }
    // cpp-httplib closes a bound socket atomically even before its accept loop starts.
    impl.server.stop();
}
}
