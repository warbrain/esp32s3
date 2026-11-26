#pragma once

#include "audiocpp.h"

// Additive, optional HMI exports. Core ABI remains the upstream native SDK ABI.
// Legacy HMI packages declare these extensions directly in audiocpp.h.
#ifndef AUDIOCPP_AUDIO_FRONTEND_STATISTICS_VERSION

#ifdef __cplusplus
extern "C" {
#endif

/* HMI LOGGING / TELEMETRY: a local rotating sink, counters and timing. */
typedef struct audiocpp_diagnostics audiocpp_diagnostics;

/* Optional process-wide timing output lease. Equal paths share one rotating
 * file; freeing one lease leaves other leases and legacy logging unchanged.
 * Tensor traces are not enabled. Operations aggregate timing scalars rather
 * than writing a line for every decoder step. UTF-8 file paths are accepted.
 * capability bit 0 = aggregate timing, bit 1 = compiled NVTX range markers,
 * bit 2 = completed-operation snapshots;
 * NVTX markers do not imply that a profiler is attached or counters available. */
AUDIOCPP_API uint32_t audiocpp_diagnostics_capabilities(void);
AUDIOCPP_API audiocpp_status audiocpp_diagnostics_create(const char * file_path,
                                                       audiocpp_diagnostics ** out_diagnostics);
AUDIOCPP_API void audiocpp_diagnostics_free(audiocpp_diagnostics * diagnostics);
/* Counters cover the shared process/file sink. Optional outputs; NULL handle
 * reports zero. Closing a lease does not clear counters used by other leases. */
AUDIOCPP_API void audiocpp_diagnostics_statistics(const audiocpp_diagnostics * diagnostics,
                                                 uint64_t * out_lines, uint64_t * out_write_failures);

#define AUDIOCPP_DIAGNOSTIC_SNAPSHOT_VERSION 1u
/* Initialize struct_size to sizeof(audiocpp_diagnostic_snapshot). This POD is
 * caller-owned; no pointer in it borrows SDK memory. Larger structures are
 * accepted and bytes after this version's prefix are untouched; a smaller
 * struct_size is rejected without writing output. version identifies the
 * returned prefix. The caller's struct_size is preserved.
 *
 * Counts cover completed operations for the shared process/file sink lifetime,
 * including fast successful operations omitted from log lines. In-flight work
 * is excluded until it finishes. Write failures and SDK operation failures are
 * separate. Queries only copy counters under the diagnostics mutex: no file I/O,
 * model work, or waiting for an SDK operation. Do not free the lease concurrently.
 *
 * operation_wall_ms and category buckets sum inclusive operation durations;
 * nested or concurrent scopes overlap. top_level_wall_ms excludes nested scopes
 * but still overlaps across threads. Neither value is process elapsed time or
 * GPU kernel time. stream_wall_ms covers stream_start/push/finish. */
typedef struct audiocpp_diagnostic_snapshot {
    uint32_t struct_size;
    uint32_t version;
    uint64_t lines;
    uint64_t write_failures;
    uint64_t completed_operations;
    uint64_t successful_operations;
    uint64_t failed_operations;
    uint64_t suppressed_operations;
    uint64_t top_level_operations;
    double operation_wall_ms;
    double top_level_wall_ms;
    double model_load_wall_ms;
    double session_create_wall_ms;
    double session_run_wall_ms;
    double stream_wall_ms;
} audiocpp_diagnostic_snapshot;

/* Both pointers must be non-NULL. No counters are reset by this call. */
AUDIOCPP_API audiocpp_status audiocpp_diagnostics_get_snapshot(
    const audiocpp_diagnostics * diagnostics, audiocpp_diagnostic_snapshot * out_snapshot);

/* HMI AUDIO: streamed PCM preprocessor and optional dual-mic enhancement. */
typedef struct audiocpp_audio_frontend audiocpp_audio_frontend;

typedef struct audiocpp_audio_frontend_config {
    uint32_t struct_size;
    uint32_t cpu_threads;            /* 0 selects one CPU thread. */
    const char * gtcrn_model_path;   /* UTF-8; NULL/empty disables denoising. */
    const char * vad_model_path;     /* UTF-8; NULL/empty disables Silero VAD. */
    float vad_threshold;            /* 0 selects 0.35; otherwise (0, 1). */
    uint32_t max_frames;            /* 0 selects 480000; never greater than 480000. */
} audiocpp_audio_frontend_config;

#define AUDIOCPP_AUDIO_FRONTEND_STATISTICS_VERSION 1u
#define AUDIOCPP_AUDIO_FRONTEND_GTCRN_ENABLED 1u
#define AUDIOCPP_AUDIO_FRONTEND_VAD_ENABLED 2u
#define AUDIOCPP_AUDIO_FRONTEND_FINISHED 4u
#define AUDIOCPP_AUDIO_FRONTEND_FAILED 8u

/* Initialize struct_size before querying. Counts and times reset per request.
 * voiced_chunks counts probability >= threshold; uncertain_chunks counts
 * probability in [threshold / 2, threshold). Neither trims returned PCM.
 * voiced_frames excludes zero padding in a final partial VAD chunk.
 * latest/max probabilities are zero when VAD has not processed a chunk.
 * pending_frames is input_frames - output_frames. Times sum processing wall
 * time and exclude time waiting for input; they are not GPU kernel durations.
 * Larger caller structures keep their trailing bytes. */
typedef struct audiocpp_audio_frontend_statistics {
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    uint64_t request_id;
    uint64_t input_frames;
    uint64_t output_frames;
    uint64_t gtcrn_frames;
    uint64_t vad_chunks;
    uint64_t voiced_chunks;
    uint64_t uncertain_chunks;
    uint64_t voiced_frames;
    uint64_t pending_frames;
    float latest_vad_probability;
    float max_vad_probability;
    uint32_t speech_seen;
    uint32_t speech_active;
    /* HMI TELEMETRY: per-request processing times. */
    double gtcrn_ms;
    double vad_ms;
    double wall_ms;
} audiocpp_audio_frontend_statistics;

/* Models are owned by the frontend and reused by reset. Creation fails if an
 * enabled model cannot load. reset starts a new request, including after an
 * inference failure. push accepts finite PCM only and never activates capture.
 * Results are OWNED: free each non-NULL result with audiocpp_result_free().
 * A successful call may return NULL while lookahead is buffered. Borrowed audio
 * from a returned result is 16 kHz mono and survives subsequent frontend calls.
 * finish flushes lookahead exactly once (later finishes return NULL): total
 * output frames equal total input
 * frames, including short recordings and final tails. No VAD trimming occurs.
 * Handles are single-threaded; free(NULL) is a no-op. */
AUDIOCPP_API audiocpp_status audiocpp_audio_frontend_create(
    const audiocpp_audio_frontend_config * config, audiocpp_audio_frontend ** out_frontend);
AUDIOCPP_API audiocpp_status audiocpp_audio_frontend_reset(
    audiocpp_audio_frontend * frontend, uint64_t request_id);
AUDIOCPP_API audiocpp_status audiocpp_audio_frontend_push(
    audiocpp_audio_frontend * frontend, const float * samples, size_t frames,
    audiocpp_result ** out_result);
AUDIOCPP_API audiocpp_status audiocpp_audio_frontend_finish(
    audiocpp_audio_frontend * frontend, audiocpp_result ** out_result);
AUDIOCPP_API audiocpp_status audiocpp_audio_frontend_get_statistics(
    const audiocpp_audio_frontend * frontend, audiocpp_audio_frontend_statistics * out_statistics);
AUDIOCPP_API void audiocpp_audio_frontend_free(audiocpp_audio_frontend * frontend);

/* ------------------------------------------------------------------ */
/* H-GTCRN full-segment dual-channel enhancement (masking on noisy)     */
/* ------------------------------------------------------------------ */

typedef struct audiocpp_hgtcrn audiocpp_hgtcrn;
typedef struct audiocpp_hgtcrn_config {
    uint32_t struct_size;
    uint32_t cpu_threads;          /* 0 selects one CPU thread. */
    const char * model_path;       /* Converted H-GTCRN masking-on-noisy weights. */
    uint32_t max_frames;           /* 0 selects 480000; at most thirty seconds. */
    uint32_t reserved;             /* Must be zero. */
} audiocpp_hgtcrn_config;

#define AUDIOCPP_HGTCRN_STATISTICS_VERSION 1u
#define AUDIOCPP_HGTCRN_READY 1u
#define AUDIOCPP_HGTCRN_SUCCEEDED 2u
#define AUDIOCPP_HGTCRN_FAILED 4u
typedef struct audiocpp_hgtcrn_statistics {
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint32_t channels;
    uint64_t request_id;
    uint64_t input_frames;
    uint64_t output_frames;
    uint64_t stft_frames;
    uint32_t wpe_taps;
    uint32_t iva_iterations;
    uint32_t iva_speech_channel;
    uint32_t right_padding_frames;
    /* HMI TELEMETRY: signal measurements and processing times. */
    double channel_correlation;
    double channel_rms_ratio;
    double stft_ms;
    double wpe_ms;
    double iva_ms;
    double network_ms;
    double istft_ms;
    double total_ms;
} audiocpp_hgtcrn_statistics;

/* Separate full-segment API: it does not claim streaming WPE/AuxIVA support.
 * samples contains frames * 2 finite floats, interleaved synchronous channels,
 * at 16000 Hz. frames is per channel, not the number of floats; 4608 frames
 * minimum are required by the reference WPE context. No mono
 * duplication or ASR/TTS implementation is introduced. The result is OWNED
 * 16000 Hz mono PCM; free it with audiocpp_result_free(). Input duration is
 * preserved. A partial final hop is reflected to the next whole 256-frame hop
 * before processing and cropped back afterward; right_padding_frames records
 * this context, never returned as extra audio. Each run resets neural state;
 * a failed run does not poison the
 * next request. Statistics describe the most recent run, excluding model load.
 * Handles are single-threaded. Query POD sizing follows the frontend contract. */
AUDIOCPP_API audiocpp_status audiocpp_hgtcrn_create(
    const audiocpp_hgtcrn_config * config, audiocpp_hgtcrn ** out_hgtcrn);
AUDIOCPP_API audiocpp_status audiocpp_hgtcrn_run(
    audiocpp_hgtcrn * hgtcrn, const float * samples, size_t frames,
    uint64_t request_id, audiocpp_result ** out_result);
AUDIOCPP_API audiocpp_status audiocpp_hgtcrn_get_statistics(
    const audiocpp_hgtcrn * hgtcrn, audiocpp_hgtcrn_statistics * out_statistics);
AUDIOCPP_API void audiocpp_hgtcrn_free(audiocpp_hgtcrn * hgtcrn);

#ifdef __cplusplus
}
#endif

#endif
