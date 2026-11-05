#include "audio_frontend.hpp"

#include "engine/framework/assets/tensor_source.h"
#include "engine/framework/audio/fft.h"
#include "engine/framework/audio/utilities/gtcrn.h"
#include "engine/framework/core/execution_context.h"
#include "engine/framework/debug/trace.h"
#include "engine/models/silero_vad/assets.h"
#include "engine/models/silero_vad/runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace engine::capi {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kFft = 512;
constexpr std::size_t kHop = 256;
constexpr std::size_t kBins = 257;
constexpr std::size_t kMaximumFrames = 480000;
constexpr float kPi = 3.14159265358979323846F;

double milliseconds(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

class AccumulateTime {
public:
    explicit AccumulateTime(double & total) noexcept : total_(total), start_(Clock::now()) {}
    ~AccumulateTime() { total_ += milliseconds(start_); }
private:
    double & total_;
    Clock::time_point start_;
};

bool enabled_path(const char * path) noexcept {
    return path != nullptr && path[0] != '\0';
}

std::size_t reflect_index(std::int64_t index, std::size_t length) noexcept {
    if (length <= 1) return 0;
    const auto period = static_cast<std::int64_t>(2 * (length - 1));
    index %= period;
    if (index < 0) index += period;
    return static_cast<std::size_t>(index < static_cast<std::int64_t>(length) ? index : period - index);
}

}  // namespace

struct AudioFrontend::State {
    std::unique_ptr<audio::GTCRNModel> gtcrn_model;
    std::unique_ptr<audio::GTCRNStreamingSession> gtcrn;
    std::unique_ptr<core::ExecutionContext> vad_context;
    std::unique_ptr<models::silero_vad::SileroRuntime> vad;
    models::silero_vad::SileroVADConfig vad_config;
    std::shared_ptr<audio::RealFFTPlan> fft;
    audio::TensorShape fft_shape{kFft};
    audio::TensorStrideBytes real_stride{static_cast<std::ptrdiff_t>(sizeof(float))};
    audio::TensorStrideBytes complex_stride{static_cast<std::ptrdiff_t>(sizeof(std::complex<float>))};
    std::array<float, kFft> window{};
    std::array<float, kFft> fft_input{};
    std::array<float, kFft> fft_output{};
    std::array<std::complex<float>, kBins> spectrum{};
    std::array<float, kBins * 2> model_input{};
    std::array<float, kBins * 2> model_output{};
    // Retain at most thirty seconds for exact centered reflection at both ends.
    std::vector<float> input;
    std::vector<float> overlap;
    std::vector<float> window_sums;
    runtime::AudioChunk vad_chunk;
    std::size_t vad_pending = 0;
    std::size_t next_frame = 0;
    std::size_t max_frames = kMaximumFrames;
    audiocpp_audio_frontend_statistics measured{};

    explicit State(const audiocpp_audio_frontend_config & config) {
        if (config.struct_size < sizeof(config)) {
            throw std::invalid_argument("audio frontend config struct_size is too small");
        }
        if (config.cpu_threads > 256) {
            throw std::invalid_argument("audio frontend cpu_threads must not exceed 256");
        }
        if (!std::isfinite(config.vad_threshold) || config.vad_threshold < 0.0F || config.vad_threshold >= 1.0F) {
            throw std::invalid_argument("audio frontend vad_threshold must be zero or between zero and one");
        }
        max_frames = config.max_frames == 0 ? kMaximumFrames : config.max_frames;
        if (max_frames > kMaximumFrames) {
            throw std::invalid_argument("audio frontend max_frames exceeds thirty seconds");
        }
        core::BackendConfig backend;
        backend.type = core::BackendType::Cpu;
        backend.threads = config.cpu_threads == 0 ? 1 : static_cast<int>(config.cpu_threads);
        if (enabled_path(config.gtcrn_model_path)) {
            const auto start = Clock::now();
            gtcrn_model = std::make_unique<audio::GTCRNModel>(
                audio::GTCRNModel::load_from_safetensors(std::filesystem::u8path(config.gtcrn_model_path), backend));
            gtcrn = gtcrn_model->create_streaming_session();
            fft = audio::get_real_fft_plan(kFft);
            input.reserve(max_frames);
            overlap.resize(max_frames + kFft);
            window_sums.resize(max_frames + kFft);
            for (std::size_t i = 0; i < kFft; ++i) {
                const float hann = 0.5F - 0.5F * std::cos(2.0F * kPi * static_cast<float>(i) / static_cast<float>(kFft));
                window[i] = std::sqrt(std::max(0.0F, hann));
            }
            debug::timing_log_scalar("audio_frontend.gtcrn.load_ms", milliseconds(start));
        }
        vad_config.threshold = config.vad_threshold == 0.0F ? 0.35F : config.vad_threshold;
        if (enabled_path(config.vad_model_path)) {
            const auto start = Clock::now();
            const auto paths = models::silero_vad::resolve_silero_assets(std::filesystem::u8path(config.vad_model_path));
            auto weights = models::silero_vad::load_silero_weights_cached(paths.checkpoint_path);
            vad_context = std::make_unique<core::ExecutionContext>(backend);
            vad = std::make_unique<models::silero_vad::SileroRuntime>(
                std::move(weights), *vad_context, assets::TensorStorageType::F32);
            vad->prepare(16000);
            vad_chunk.sample_rate = 16000;
            vad_chunk.channels = 1;
            vad_chunk.samples.resize(kFft);
            debug::timing_log_scalar("audio_frontend.vad.load_ms", milliseconds(start));
        }
        reset(0);
    }

    void reset(std::uint64_t request_id) {
        measured = {};
        measured.struct_size = static_cast<std::uint32_t>(sizeof(measured));
        measured.version = AUDIOCPP_AUDIO_FRONTEND_STATISTICS_VERSION;
        measured.request_id = request_id;
        if (gtcrn) measured.flags |= AUDIOCPP_AUDIO_FRONTEND_GTCRN_ENABLED;
        if (vad) measured.flags |= AUDIOCPP_AUDIO_FRONTEND_VAD_ENABLED;
        input.clear();
        std::fill(overlap.begin(), overlap.end(), 0.0F);
        std::fill(window_sums.begin(), window_sums.end(), 0.0F);
        next_frame = 0;
        vad_pending = 0;
        vad_chunk.start_sample = 0;
        try {
            if (gtcrn) gtcrn->reset();
            if (vad) vad->reset();
        } catch (...) {
            measured.flags |= AUDIOCPP_AUDIO_FRONTEND_FAILED;
            throw;
        }
    }

    void require_mutable() const {
        if ((measured.flags & AUDIOCPP_AUDIO_FRONTEND_FAILED) != 0) {
            throw std::runtime_error("audio frontend failed; reset is required");
        }
        if ((measured.flags & AUDIOCPP_AUDIO_FRONTEND_FINISHED) != 0) {
            throw std::invalid_argument("audio frontend request is already finished");
        }
    }

    void run_vad_chunk(std::size_t actual_frames) {
        AccumulateTime timer(measured.vad_ms);
        const auto event = vad->process_chunk(vad_chunk, vad_config);
        (void)event;
        const float probability = vad->last_probability();
        if (!std::isfinite(probability)) throw std::runtime_error("audio frontend VAD returned non-finite probability");
        measured.latest_vad_probability = probability;
        measured.max_vad_probability = std::max(measured.max_vad_probability, probability);
        ++measured.vad_chunks;
        if (probability >= vad_config.threshold) {
            ++measured.voiced_chunks;
            measured.voiced_frames += actual_frames;
            measured.speech_seen = 1;
        } else if (probability >= vad_config.threshold * 0.5F) {
            ++measured.uncertain_chunks;
        }
        measured.speech_active = vad->speech_active() ? 1U : 0U;
        vad_chunk.start_sample += static_cast<std::int64_t>(kFft);
        vad_pending = 0;
    }

    void analyze(const std::vector<float> & output, bool final) {
        if (!vad) return;
        std::size_t offset = 0;
        while (offset < output.size()) {
            const auto count = std::min(kFft - vad_pending, output.size() - offset);
            std::copy_n(output.data() + offset, count, vad_chunk.samples.data() + vad_pending);
            vad_pending += count;
            offset += count;
            if (vad_pending == kFft) run_vad_chunk(kFft);
        }
        if (final && vad_pending > 0) {
            const auto actual_frames = vad_pending;
            std::fill(vad_chunk.samples.begin() + static_cast<std::ptrdiff_t>(vad_pending), vad_chunk.samples.end(), 0.0F);
            run_vad_chunk(actual_frames);
        }
        if (final) {
            AccumulateTime timer(measured.vad_ms);
            (void)vad->finalize_stream(vad_config);
            measured.speech_active = 0;
        }
    }

    void process_frame() {
        const auto start_sample = static_cast<std::int64_t>(next_frame * kHop) - static_cast<std::int64_t>(kHop);
        for (std::size_t i = 0; i < kFft; ++i) {
            const auto index = reflect_index(start_sample + static_cast<std::int64_t>(i), input.size());
            fft_input[i] = input[index] * window[i];
        }
        fft->forward(fft_shape, real_stride, complex_stride, 0, fft_input.data(), spectrum.data(), 1.0F, 1);
        for (std::size_t i = 0; i < kBins; ++i) {
            model_input[2 * i] = spectrum[i].real();
            model_input[2 * i + 1] = spectrum[i].imag();
        }
        gtcrn->process_stft_frame(model_input.data(), model_output.data());
        for (std::size_t i = 0; i < kBins; ++i) {
            if (!std::isfinite(model_output[2 * i]) || !std::isfinite(model_output[2 * i + 1])) {
                throw std::runtime_error("audio frontend GTCRN returned non-finite audio");
            }
            spectrum[i] = {model_output[2 * i], model_output[2 * i + 1]};
        }
        fft->inverse(fft_shape, complex_stride, real_stride, 0, spectrum.data(), fft_output.data(), 1.0F / static_cast<float>(kFft), 1);
        const auto offset = next_frame * kHop;
        for (std::size_t i = 0; i < kFft; ++i) {
            overlap[offset + i] += fft_output[i] * window[i];
            window_sums[offset + i] += window[i] * window[i];
        }
        ++next_frame;
        ++measured.gtcrn_frames;
    }

    std::vector<float> denoise(bool final) {
        AccumulateTime timer(measured.gtcrn_ms);
        if (input.empty()) return {};
        if (final) {
            const auto frame_count = 1 + input.size() / kHop;
            while (next_frame < frame_count) process_frame();
        } else {
            // The first reflected frame needs sample 256, and later frames need
            // their complete right window. Never reflect at a live chunk edge.
            while (input.size() > kHop && next_frame * kHop + kHop <= input.size()) process_frame();
        }
        const auto finalized = next_frame > 0 ? (next_frame - 1) * kHop : 0;
        const auto end = final ? input.size() : std::min(input.size(), finalized);
        const auto begin = static_cast<std::size_t>(measured.output_frames);
        std::vector<float> output(end - begin);
        for (std::size_t i = begin; i < end; ++i) {
            const auto padded = i + kHop;
            const float denominator = window_sums[padded] > 1.0e-8F ? window_sums[padded] : 1.0F;
            const float value = overlap[padded] / denominator;
            if (!std::isfinite(value)) throw std::runtime_error("audio frontend inverse FFT returned non-finite audio");
            output[i - begin] = value;
        }
        return output;
    }
};

AudioFrontend::AudioFrontend(const audiocpp_audio_frontend_config & config) : state_(std::make_unique<State>(config)) {}
AudioFrontend::~AudioFrontend() = default;

void AudioFrontend::reset(std::uint64_t request_id) {
    state_->reset(request_id);
}

std::vector<float> AudioFrontend::push(const float * samples, std::size_t frames) {
    auto & state = *state_;
    state.require_mutable();
    if (frames > 0 && samples == nullptr) throw std::invalid_argument("audio frontend samples must be non-null");
    if (frames > state.max_frames - static_cast<std::size_t>(state.measured.input_frames)) {
        throw std::invalid_argument("audio frontend request exceeds max_frames");
    }
    for (std::size_t i = 0; i < frames; ++i) {
        if (!std::isfinite(samples[i])) throw std::invalid_argument("audio frontend PCM must be finite");
    }
    AccumulateTime timer(state.measured.wall_ms);
    try {
        std::vector<float> output;
        if (state.gtcrn) {
            if (frames > 0) state.input.insert(state.input.end(), samples, samples + frames);
            state.measured.input_frames += frames;
            output = state.denoise(false);
        } else {
            if (frames > 0) output.assign(samples, samples + frames);
            state.measured.input_frames += frames;
        }
        state.analyze(output, false);
        state.measured.output_frames += output.size();
        return output;
    } catch (...) {
        state.measured.flags |= AUDIOCPP_AUDIO_FRONTEND_FAILED;
        throw;
    }
}

std::vector<float> AudioFrontend::finish() {
    auto & state = *state_;
    if ((state.measured.flags & AUDIOCPP_AUDIO_FRONTEND_FINISHED) != 0) return {};
    state.require_mutable();
    AccumulateTime timer(state.measured.wall_ms);
    try {
        auto output = state.gtcrn ? state.denoise(true) : std::vector<float>{};
        state.analyze(output, true);
        state.measured.output_frames += output.size();
        if (state.measured.output_frames != state.measured.input_frames) {
            throw std::runtime_error("audio frontend did not preserve the input duration");
        }
        state.measured.flags |= AUDIOCPP_AUDIO_FRONTEND_FINISHED;
        return output;
    } catch (...) {
        state.measured.flags |= AUDIOCPP_AUDIO_FRONTEND_FAILED;
        throw;
    }
}

audiocpp_audio_frontend_statistics AudioFrontend::statistics() const noexcept {
    auto measured = state_->measured;
    measured.pending_frames = measured.input_frames - measured.output_frames;
    return measured;
}

}  // namespace engine::capi
