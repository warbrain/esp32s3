#include "audiocpp_hmi.h"
#include "audio_frontend.hpp"
#include "native_telemetry.hpp"

// HMI TELEMETRY: rename selected upstream implementations in this translation
// unit so the public entry points below can measure them without changing the
// upstream source, public header, or result/error ownership.
#define audiocpp_registry_create upstream_registry_create
#define audiocpp_model_load upstream_model_load
#define audiocpp_session_create upstream_session_create
#define audiocpp_session_create_with_impl upstream_session_create_with_impl
#define audiocpp_session_prepare upstream_session_prepare
#define audiocpp_session_run upstream_session_run
#define audiocpp_session_free upstream_session_free
#define audiocpp_stream_start upstream_stream_start
#define audiocpp_stream_push upstream_stream_push
#define audiocpp_stream_finish upstream_stream_finish
#define audiocpp_stream_reset upstream_stream_reset

audiocpp_status upstream_session_create_with_impl(
    const audiocpp_model *, const char *, const char *, const audiocpp_backend_config *,
    const char *, const audiocpp_options *, audiocpp_session **);

#include "audiocpp.cpp"

#undef audiocpp_registry_create
#undef audiocpp_model_load
#undef audiocpp_session_create
#undef audiocpp_session_create_with_impl
#undef audiocpp_session_prepare
#undef audiocpp_session_run
#undef audiocpp_session_free
#undef audiocpp_stream_start
#undef audiocpp_stream_push
#undef audiocpp_stream_finish
#undef audiocpp_stream_reset

#include "engine/framework/audio/utilities/hgtcrn.h"

namespace {

// HMI TELEMETRY: labels are captured at stream start and copied independently
// of the request, which can be freed before the next audio chunk.
struct RequestLabels {
    std::string request_id;
    std::string segment_index;
    std::string attempt;
    std::string stage;
    std::string revision;
};

std::mutex stream_labels_mutex;
std::unordered_map<const audiocpp_session *, RequestLabels> stream_labels;

RequestLabels read_labels(const audiocpp_request * request) noexcept {
    if (!request || !engine::debug::timing_log_enabled()) return {};
    try {
        const auto get = [&](const char * key) {
            const auto entry = request->request.options.find(key);
            return entry == request->request.options.end() ? std::string{} : entry->second.substr(0, 128);
        };
        return {get("hmi.request_id"), get("hmi.segment_index"), get("hmi.attempt"),
                get("hmi.stage"), get("hmi.revision")};
    } catch (...) {
        return {};
    }
}

RequestLabels read_stream_labels(const audiocpp_session * session) noexcept {
    if (!engine::debug::timing_log_enabled()) return {};
    try {
        std::lock_guard<std::mutex> lock(stream_labels_mutex);
        const auto entry = stream_labels.find(session);
        if (entry != stream_labels.end()) return entry->second;
    } catch (...) { }
    return {};
}

void set_stream_labels(const audiocpp_session * session, RequestLabels labels = {}) noexcept {
    try {
        std::lock_guard<std::mutex> lock(stream_labels_mutex);
        if (labels.request_id.empty() && labels.segment_index.empty() &&
            labels.attempt.empty() && labels.stage.empty() && labels.revision.empty()) {
            stream_labels.erase(session);
        } else {
            stream_labels.insert_or_assign(session, std::move(labels));
        }
    } catch (...) { }
}

audiocpp_status observe(audiocpp_status status, engine::debug::DiagnosticOperation & operation) noexcept {
    operation.finish(status == AUDIOCPP_OK);
    return status;
}

}  // namespace

// HMI TELEMETRY: calls retain upstream validation, execution order and results.
audiocpp_status audiocpp_registry_create(const char * config_path, audiocpp_registry ** out_registry) {
    engine::debug::DiagnosticOperation operation("registry_create", "runtime", {}, {}, {}, "initialization");
    return observe(upstream_registry_create(config_path, out_registry), operation);
}

audiocpp_status audiocpp_model_load(audiocpp_registry * registry, const char * model_path,
                                   const audiocpp_model_config * config, const audiocpp_options * options,
                                   audiocpp_model ** out_model) {
    engine::debug::DiagnosticOperation operation("model_load",
        config && config->family_hint ? config->family_hint : "");
    return observe(upstream_model_load(registry, model_path, config, options, out_model), operation);
}

audiocpp_status audiocpp_session_create_with_impl(
    const audiocpp_model * model, const char * task, const char * mode,
    const audiocpp_backend_config * backend, const char * implementation,
    const audiocpp_options * options, audiocpp_session ** out_session) {
    engine::debug::DiagnosticOperation operation("session_create", audiocpp_model_family(model));
    return observe(upstream_session_create_with_impl(
        model, task, mode, backend, implementation, options, out_session), operation);
}

audiocpp_status audiocpp_session_create(
    const audiocpp_model * model, const char * task, const char * mode,
    const audiocpp_backend_config * backend, const audiocpp_options * options,
    audiocpp_session ** out_session) {
    engine::debug::DiagnosticOperation operation("session_create", audiocpp_model_family(model));
    return observe(upstream_session_create(model, task, mode, backend, options, out_session), operation);
}

audiocpp_status audiocpp_session_prepare(audiocpp_session * session, const audiocpp_request * request) {
    const auto labels = read_labels(request);
    engine::debug::DiagnosticOperation operation("prepare", audiocpp_session_family(session),
        labels.request_id, labels.segment_index, labels.attempt, labels.stage, labels.revision);
    return observe(upstream_session_prepare(session, request), operation);
}

audiocpp_status audiocpp_session_run(audiocpp_session * session, const audiocpp_request * request,
                                    audiocpp_result ** out_result) {
    const auto labels = read_labels(request);
    engine::debug::DiagnosticOperation operation("session_run", audiocpp_session_family(session),
        labels.request_id, labels.segment_index, labels.attempt, labels.stage, labels.revision);
    return observe(upstream_session_run(session, request, out_result), operation);
}

audiocpp_status audiocpp_stream_start(audiocpp_session * session, const audiocpp_request * request) {
    auto labels = read_labels(request);
    engine::debug::DiagnosticOperation operation("stream_start", audiocpp_session_family(session),
        labels.request_id, labels.segment_index, labels.attempt, labels.stage, labels.revision);
    const auto status = upstream_stream_start(session, request);
    if (status == AUDIOCPP_OK) set_stream_labels(session, std::move(labels));
    return observe(status, operation);
}

audiocpp_status audiocpp_stream_push(audiocpp_session * session, const float * samples, size_t frames,
                                    int sample_rate, int channels, int64_t start_sample,
                                    audiocpp_event ** out_event) {
    const auto labels = read_stream_labels(session);
    engine::debug::DiagnosticOperation operation("stream_push", audiocpp_session_family(session),
        labels.request_id, labels.segment_index, labels.attempt, labels.stage, labels.revision);
    return observe(upstream_stream_push(session, samples, frames, sample_rate, channels, start_sample, out_event), operation);
}

audiocpp_status audiocpp_stream_finish(audiocpp_session * session, audiocpp_result ** out_result) {
    const auto labels = read_stream_labels(session);
    engine::debug::DiagnosticOperation operation("stream_finish", audiocpp_session_family(session),
        labels.request_id, labels.segment_index, labels.attempt, labels.stage, labels.revision);
    const auto status = upstream_stream_finish(session, out_result);
    set_stream_labels(session);
    return observe(status, operation);
}

audiocpp_status audiocpp_stream_reset(audiocpp_session * session) {
    set_stream_labels(session);
    return upstream_stream_reset(session);
}

void audiocpp_session_free(audiocpp_session * session) {
    set_stream_labels(session);
    upstream_session_free(session);
}

// HMI AUDIO: optional extensions to the unchanged upstream public C ABI.
struct audiocpp_audio_frontend {
    engine::capi::AudioFrontend frontend;
    explicit audiocpp_audio_frontend(const audiocpp_audio_frontend_config & config) : frontend(config) {}
};

struct audiocpp_hgtcrn {
    std::unique_ptr<engine::audio::HgtcrnModel> model;
};

namespace {

audiocpp_hgtcrn_statistics hgtcrn_statistics(const audiocpp_hgtcrn & handle) noexcept {
    const auto source = handle.model->statistics();
    audiocpp_hgtcrn_statistics measured{};
    measured.struct_size = static_cast<uint32_t>(sizeof(measured));
    measured.version = AUDIOCPP_HGTCRN_STATISTICS_VERSION;
    measured.flags = AUDIOCPP_HGTCRN_READY;
    if (source.succeeded) measured.flags |= AUDIOCPP_HGTCRN_SUCCEEDED;
    if (source.failed) measured.flags |= AUDIOCPP_HGTCRN_FAILED;
    measured.channels = 2;
    measured.request_id = source.request_id;
    measured.input_frames = source.input_frames;
    measured.output_frames = source.output_frames;
    measured.stft_frames = source.stft_frames;
    measured.wpe_taps = source.wpe_taps;
    measured.iva_iterations = source.iva_iterations;
    measured.iva_speech_channel = source.iva_speech_channel;
    measured.right_padding_frames = source.right_padding_frames;
    measured.channel_correlation = source.channel_correlation;
    measured.channel_rms_ratio = source.channel_rms_ratio;
    measured.stft_ms = source.stft_ms;
    measured.wpe_ms = source.wpe_ms;
    measured.iva_ms = source.iva_ms;
    measured.network_ms = source.network_ms;
    measured.istft_ms = source.istft_ms;
    measured.total_ms = source.total_ms;
    return measured;
}

// HMI TELEMETRY: additional signal and processing metrics.
void log_hgtcrn_statistics(const audiocpp_hgtcrn & handle) {
    const auto measured = hgtcrn_statistics(handle);
    engine::debug::timing_log_scalar("hgtcrn.flags", measured.flags);
    engine::debug::timing_log_scalar("hgtcrn.input_frames", measured.input_frames);
    engine::debug::timing_log_scalar("hgtcrn.output_frames", measured.output_frames);
    engine::debug::timing_log_scalar("hgtcrn.stft_frames", measured.stft_frames);
    engine::debug::timing_log_scalar("hgtcrn.wpe_taps", measured.wpe_taps);
    engine::debug::timing_log_scalar("hgtcrn.iva_iterations", measured.iva_iterations);
    engine::debug::timing_log_scalar("hgtcrn.iva_speech_channel", measured.iva_speech_channel);
    engine::debug::timing_log_scalar("hgtcrn.right_padding_frames", measured.right_padding_frames);
    engine::debug::timing_log_scalar("hgtcrn.channel_correlation", measured.channel_correlation);
    engine::debug::timing_log_scalar("hgtcrn.channel_rms_ratio", measured.channel_rms_ratio);
    engine::debug::timing_log_scalar("hgtcrn.stft_ms", measured.stft_ms);
    engine::debug::timing_log_scalar("hgtcrn.wpe_ms", measured.wpe_ms);
    engine::debug::timing_log_scalar("hgtcrn.iva_ms", measured.iva_ms);
    engine::debug::timing_log_scalar("hgtcrn.network_ms", measured.network_ms);
    engine::debug::timing_log_scalar("hgtcrn.istft_ms", measured.istft_ms);
    engine::debug::timing_log_scalar("hgtcrn.total_ms", measured.total_ms);
}

}  // namespace

audiocpp_status audiocpp_audio_frontend_create(
    const audiocpp_audio_frontend_config * config, audiocpp_audio_frontend ** out_frontend) {
    if (out_frontend != nullptr) *out_frontend = nullptr;
    if (config == nullptr || out_frontend == nullptr) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "config and out_frontend must be non-null");
    }
    return guard([&] {
        // HMI TELEMETRY: native operation timing scope.
        engine::debug::DiagnosticOperation operation("frontend_create", "audio_frontend", {}, {}, {}, "initialization");
#ifdef GGML_BACKEND_DL
        load_library_backends();
#endif
        auto handle = std::make_unique<audiocpp_audio_frontend>(*config);
        const auto measured = handle->frontend.statistics();
        engine::debug::timing_log_scalar("audio_frontend.enabled_flags", measured.flags);
        *out_frontend = handle.release();
        return AUDIOCPP_OK;
    }, AUDIOCPP_ERR_LOAD_FAILED);
}

audiocpp_status audiocpp_audio_frontend_reset(audiocpp_audio_frontend * frontend, uint64_t request_id) {
    if (frontend == nullptr) return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "frontend must be non-null");
    return guard([&] {
        // HMI TELEMETRY: native operation timing scope.
        engine::debug::DiagnosticOperation operation("frontend_reset", "audio_frontend", std::to_string(request_id));
        frontend->frontend.reset(request_id);
        return AUDIOCPP_OK;
    });
}

audiocpp_status audiocpp_audio_frontend_push(
    audiocpp_audio_frontend * frontend, const float * samples, size_t frames, audiocpp_result ** out_result) {
    if (out_result != nullptr) *out_result = nullptr;
    if (frontend == nullptr || out_result == nullptr) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "frontend and out_result must be non-null");
    }
    return guard([&] {
        const auto measured = frontend->frontend.statistics();
        // HMI TELEMETRY: native operation timing scope.
        engine::debug::DiagnosticOperation operation("frontend_push", "audio_frontend", std::to_string(measured.request_id));
        // Allocate the result before mutating the stream, so an allocation
        // failure cannot consume audio that the caller did not receive.
        auto result = std::make_unique<audiocpp_result>();
        result->result.audio_output = rt::AudioBuffer{};
        auto & audio = *result->result.audio_output;
        audio.sample_rate = 16000;
        audio.channels = 1;
        audio.samples = frontend->frontend.push(samples, frames);
        if (!audio.samples.empty()) *out_result = result.release();
        return AUDIOCPP_OK;
    });
}

audiocpp_status audiocpp_audio_frontend_finish(audiocpp_audio_frontend * frontend, audiocpp_result ** out_result) {
    if (out_result != nullptr) *out_result = nullptr;
    if (frontend == nullptr || out_result == nullptr) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "frontend and out_result must be non-null");
    }
    return guard([&] {
        const auto before = frontend->frontend.statistics();
        if ((before.flags & AUDIOCPP_AUDIO_FRONTEND_FINISHED) != 0) return AUDIOCPP_OK;
        // HMI TELEMETRY: native operation timing scope.
        engine::debug::DiagnosticOperation operation("frontend_finish", "audio_frontend", std::to_string(before.request_id));
        auto result = std::make_unique<audiocpp_result>();
        result->result.audio_output = rt::AudioBuffer{};
        auto & audio = *result->result.audio_output;
        audio.sample_rate = 16000;
        audio.channels = 1;
        audio.samples = frontend->frontend.finish();
        const auto measured = frontend->frontend.statistics();
        engine::debug::timing_log_scalar("audio_frontend.input_frames", measured.input_frames);
        engine::debug::timing_log_scalar("audio_frontend.output_frames", measured.output_frames);
        engine::debug::timing_log_scalar("audio_frontend.gtcrn_frames", measured.gtcrn_frames);
        engine::debug::timing_log_scalar("audio_frontend.vad_chunks", measured.vad_chunks);
        engine::debug::timing_log_scalar("audio_frontend.voiced_chunks", measured.voiced_chunks);
        engine::debug::timing_log_scalar("audio_frontend.uncertain_chunks", measured.uncertain_chunks);
        engine::debug::timing_log_scalar("audio_frontend.max_vad_probability", measured.max_vad_probability);
        engine::debug::timing_log_scalar("audio_frontend.gtcrn_ms", measured.gtcrn_ms);
        engine::debug::timing_log_scalar("audio_frontend.vad_ms", measured.vad_ms);
        engine::debug::timing_log_scalar("audio_frontend.wall_ms", measured.wall_ms);
        if (!audio.samples.empty()) *out_result = result.release();
        return AUDIOCPP_OK;
    });
}

audiocpp_status audiocpp_audio_frontend_get_statistics(
    const audiocpp_audio_frontend * frontend, audiocpp_audio_frontend_statistics * out_statistics) {
    if (frontend == nullptr || out_statistics == nullptr) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "frontend and out_statistics must be non-null");
    }
    if (out_statistics->struct_size < sizeof(*out_statistics)) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "audio frontend statistics struct_size is too small");
    }
    return guard([&] {
        const auto caller_size = out_statistics->struct_size;
        *out_statistics = frontend->frontend.statistics();
        out_statistics->struct_size = caller_size;
        return AUDIOCPP_OK;
    });
}

void audiocpp_audio_frontend_free(audiocpp_audio_frontend * frontend) {
    delete frontend;
}

audiocpp_status audiocpp_hgtcrn_create(const audiocpp_hgtcrn_config * config, audiocpp_hgtcrn ** out_hgtcrn) {
    if (out_hgtcrn != nullptr) *out_hgtcrn = nullptr;
    if (config == nullptr || out_hgtcrn == nullptr) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "config and out_hgtcrn must be non-null");
    }
    return guard([&] {
        // HMI TELEMETRY: native operation timing scope.
        engine::debug::DiagnosticOperation operation("hgtcrn_create", "hgtcrn", {}, {}, {}, "initialization");
        if (config->struct_size < sizeof(*config) || config->reserved != 0) {
            throw std::invalid_argument("H-GTCRN config size or reserved field is invalid");
        }
        if (config->model_path == nullptr || config->model_path[0] == '\0' || config->cpu_threads > 256) {
            throw std::invalid_argument("H-GTCRN requires a model path and at most 256 CPU threads");
        }
#ifdef GGML_BACKEND_DL
        load_library_backends();
#endif
        engine::core::BackendConfig backend;
        backend.type = engine::core::BackendType::Cpu;
        backend.threads = config->cpu_threads == 0 ? 1 : static_cast<int>(config->cpu_threads);
        auto handle = std::make_unique<audiocpp_hgtcrn>();
        handle->model = std::make_unique<engine::audio::HgtcrnModel>(
            std::filesystem::u8path(config->model_path), backend, config->max_frames);
        *out_hgtcrn = handle.release();
        return AUDIOCPP_OK;
    }, AUDIOCPP_ERR_LOAD_FAILED);
}

audiocpp_status audiocpp_hgtcrn_run(
    audiocpp_hgtcrn * hgtcrn, const float * samples, size_t frames,
    uint64_t request_id, audiocpp_result ** out_result) {
    if (out_result != nullptr) *out_result = nullptr;
    if (hgtcrn == nullptr || out_result == nullptr) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "hgtcrn and out_result must be non-null");
    }
    return guard([&] {
        // HMI TELEMETRY: native operation timing scope.
        engine::debug::DiagnosticOperation operation("hgtcrn_run", "hgtcrn", std::to_string(request_id));
        auto result = std::make_unique<audiocpp_result>();
        result->result.audio_output = rt::AudioBuffer{};
        auto & audio = *result->result.audio_output;
        audio.sample_rate = 16000;
        audio.channels = 1;
        try {
            audio.samples = hgtcrn->model->run(samples, frames, request_id);
        } catch (...) {
            log_hgtcrn_statistics(*hgtcrn);
            throw;
        }
        log_hgtcrn_statistics(*hgtcrn);
        *out_result = result.release();
        return AUDIOCPP_OK;
    });
}

audiocpp_status audiocpp_hgtcrn_get_statistics(
    const audiocpp_hgtcrn * hgtcrn, audiocpp_hgtcrn_statistics * out_statistics) {
    if (hgtcrn == nullptr || out_statistics == nullptr) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "hgtcrn and out_statistics must be non-null");
    }
    if (out_statistics->struct_size < sizeof(*out_statistics)) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "H-GTCRN statistics struct_size is too small");
    }
    return guard([&] {
        const auto caller_size = out_statistics->struct_size;
        *out_statistics = hgtcrn_statistics(*hgtcrn);
        out_statistics->struct_size = caller_size;
        return AUDIOCPP_OK;
    });
}

void audiocpp_hgtcrn_free(audiocpp_hgtcrn * hgtcrn) { delete hgtcrn; }

// HMI LOGGING / TELEMETRY: process-wide leases and completed-operation snapshots.
struct audiocpp_diagnostics {
    engine::debug::DiagnosticsLease lease;
    explicit audiocpp_diagnostics(const std::string & path) : lease(path) {}
};

uint32_t audiocpp_diagnostics_capabilities(void) {
    return 1u | (engine::debug::nvtx_compiled() ? 2u : 0u) | 4u;
}

audiocpp_status audiocpp_diagnostics_create(const char * file_path, audiocpp_diagnostics ** out_diagnostics) {
    if (file_path == nullptr || *file_path == '\0' || out_diagnostics == nullptr)
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "diagnostic file and output must be non-null");
    *out_diagnostics = nullptr;
    return guard([&] {
        auto handle = std::make_unique<audiocpp_diagnostics>(file_path);
        *out_diagnostics = handle.release();
        return AUDIOCPP_OK;
    });
}

void audiocpp_diagnostics_free(audiocpp_diagnostics * diagnostics) {
    delete diagnostics;
}

void audiocpp_diagnostics_statistics(const audiocpp_diagnostics * diagnostics,
                                    uint64_t * out_lines, uint64_t * out_write_failures) {
    uint64_t lines = 0, failures = 0;
    if (diagnostics) diagnostics->lease.statistics(lines, failures);
    if (out_lines) *out_lines = lines;
    if (out_write_failures) *out_write_failures = failures;
}

audiocpp_status audiocpp_diagnostics_get_snapshot(
    const audiocpp_diagnostics * diagnostics, audiocpp_diagnostic_snapshot * out_snapshot) {
    if (diagnostics == nullptr || out_snapshot == nullptr) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "diagnostics and snapshot must be non-null");
    }
    if (out_snapshot->struct_size < sizeof(audiocpp_diagnostic_snapshot)) {
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "diagnostic snapshot struct_size is too small");
    }
    return guard([&] {
        const auto measured = diagnostics->lease.statistics();
        audiocpp_diagnostic_snapshot snapshot{};
        snapshot.struct_size = out_snapshot->struct_size;
        snapshot.version = AUDIOCPP_DIAGNOSTIC_SNAPSHOT_VERSION;
        snapshot.lines = measured.lines;
        snapshot.write_failures = measured.failures;
        snapshot.completed_operations = measured.completed_operations;
        snapshot.successful_operations = measured.successful_operations;
        snapshot.failed_operations = measured.failed_operations;
        snapshot.suppressed_operations = measured.suppressed_operations;
        snapshot.top_level_operations = measured.top_level_operations;
        snapshot.operation_wall_ms = measured.operation_wall_ms;
        snapshot.top_level_wall_ms = measured.top_level_wall_ms;
        snapshot.model_load_wall_ms = measured.model_load_wall_ms;
        snapshot.session_create_wall_ms = measured.session_create_wall_ms;
        snapshot.session_run_wall_ms = measured.session_run_wall_ms;
        snapshot.stream_wall_ms = measured.stream_wall_ms;
        *out_snapshot = snapshot;
        return AUDIOCPP_OK;
    });
}
