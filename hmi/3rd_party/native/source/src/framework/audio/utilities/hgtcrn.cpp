/*
 * Native adaptation of https://github.com/Max1Wz/H-GTCRN, masking_on_noisy.
 * Copyright (c) 2025 Max1Wz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "engine/framework/audio/utilities/hgtcrn.h"

#include "engine/framework/audio/fft.h"
#include "engine/framework/audio/utilities/gtcrn.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
#include <utility>

namespace engine::audio {
namespace {

using Complex = std::complex<float>;
using WideComplex = std::complex<double>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kFft = 512;
constexpr std::size_t kHop = 256;
constexpr std::size_t kBins = 257;
constexpr std::size_t kTaps = 18;
constexpr std::size_t kDelay = 2;
constexpr std::size_t kWpeSize = 2 * kTaps;
constexpr std::size_t kMinimumFrames = 4608;
constexpr std::size_t kMaximumFrames = 480000;
constexpr float kPi = 3.14159265358979323846F;

class MeasureTime {
public:
    explicit MeasureTime(double & result) noexcept : result_(result), start_(Clock::now()) {}
    ~MeasureTime() { result_ += std::chrono::duration<double, std::milli>(Clock::now() - start_).count(); }
private:
    double & result_;
    Clock::time_point start_;
};

std::size_t reflected(std::int64_t index, std::size_t length) noexcept {
    const auto period = static_cast<std::int64_t>(2 * (length - 1));
    index %= period;
    if (index < 0) index += period;
    return static_cast<std::size_t>(index < static_cast<std::int64_t>(length) ? index : period - index);
}

Complex narrow(WideComplex value) {
    const Complex result{static_cast<float>(value.real()), static_cast<float>(value.imag())};
    if (!std::isfinite(result.real()) || !std::isfinite(result.imag())) {
        throw std::runtime_error("H-GTCRN numerical operation produced non-finite values");
    }
    return result;
}

// Partial-pivoted elimination for the small complex WPE/IVA systems. No new
// linear algebra dependency and no unregularized inverse of a covariance.
void solve(std::vector<WideComplex> & matrix, std::vector<WideComplex> & rhs,
           std::size_t n, std::size_t columns) {
    for (std::size_t pivot = 0; pivot < n; ++pivot) {
        std::size_t row = pivot;
        double largest = std::abs(matrix[pivot * n + pivot]);
        for (std::size_t candidate = pivot + 1; candidate < n; ++candidate) {
            const double magnitude = std::abs(matrix[candidate * n + pivot]);
            if (magnitude > largest) {
                largest = magnitude;
                row = candidate;
            }
        }
        if (!std::isfinite(largest) || largest <= std::numeric_limits<double>::min()) {
            throw std::runtime_error("H-GTCRN regularized spatial covariance is singular");
        }
        if (row != pivot) {
            for (std::size_t column = pivot; column < n; ++column) {
                std::swap(matrix[row * n + column], matrix[pivot * n + column]);
            }
            for (std::size_t column = 0; column < columns; ++column) {
                std::swap(rhs[row * columns + column], rhs[pivot * columns + column]);
            }
        }
        for (std::size_t target = pivot + 1; target < n; ++target) {
            const auto factor = matrix[target * n + pivot] / matrix[pivot * n + pivot];
            for (std::size_t column = pivot + 1; column < n; ++column) {
                matrix[target * n + column] -= factor * matrix[pivot * n + column];
            }
            for (std::size_t column = 0; column < columns; ++column) {
                rhs[target * columns + column] -= factor * rhs[pivot * columns + column];
            }
        }
    }
    for (std::size_t reverse = n; reverse > 0; --reverse) {
        const auto row = reverse - 1;
        for (std::size_t column = 0; column < columns; ++column) {
            auto value = rhs[row * columns + column];
            for (std::size_t upper = row + 1; upper < n; ++upper) {
                value -= matrix[row * n + upper] * rhs[upper * columns + column];
            }
            rhs[row * columns + column] = value / matrix[row * n + row];
        }
    }
}

// Channel-major, then frequency, then time. This matches the official X[B,M,F,T].
std::size_t index(std::size_t channel, std::size_t frequency, std::size_t frame, std::size_t count) noexcept {
    return (channel * kBins + frequency) * count + frame;
}

void observe(const HgtcrnStageObserver * observer, const char * stage,
             const std::vector<Complex> & values, std::size_t channels, std::size_t frames) {
    if (observer != nullptr && observer->callback != nullptr) {
        observer->callback(observer->context, stage, values.data(), channels, kBins, frames);
    }
}

std::vector<Complex> wpe(const std::vector<Complex> & original, std::size_t count) {
    double frequency_max_sum = 0.0;
    for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
        double maximum = 0.0;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t frame = 0; frame < count; ++frame) {
                maximum = std::max(maximum, static_cast<double>(std::norm(original[index(channel, frequency, frame, count)])));
            }
        }
        frequency_max_sum += maximum;
    }
    // Official nested max changes rank: [B,M,F,T] --max(-1)--> [B,M,F],
    // then max(-2) reduces M, not F. The final mean averages frequency bins.
    const double epsilon = 0.001 * frequency_max_sum / static_cast<double>(kBins);
    if (!(epsilon > 0.0) || !std::isfinite(epsilon)) throw std::runtime_error("H-GTCRN invalid WPE input energy");
    std::vector<Complex> output(original.size());
    std::vector<Complex> delayed(kWpeSize * count);
    std::vector<double> inverse_power(count);
    std::vector<WideComplex> covariance(kWpeSize * kWpeSize);
    std::vector<WideComplex> coefficients(kWpeSize * 2);
    for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
        std::fill(delayed.begin(), delayed.end(), Complex{});
        for (std::size_t tap = 0; tap < kTaps; ++tap) {
            const auto shift = kDelay + tap;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                for (std::size_t frame = shift; frame < count; ++frame) {
                    delayed[(2 * tap + channel) * count + frame] = original[index(channel, frequency, frame - shift, count)];
                }
            }
        }
        for (std::size_t frame = 0; frame < count; ++frame) {
            const double power = 0.5 * (static_cast<double>(std::norm(original[index(0, frequency, frame, count)])) +
                                        static_cast<double>(std::norm(original[index(1, frequency, frame, count)])));
            inverse_power[frame] = 1.0 / std::max(power, epsilon);
        }
        for (std::size_t row = 0; row < kWpeSize; ++row) {
            for (std::size_t column = 0; column <= row; ++column) {
                WideComplex sum{};
                for (std::size_t frame = 0; frame < count; ++frame) {
                    sum += WideComplex(delayed[row * count + frame]) *
                           std::conj(WideComplex(delayed[column * count + frame])) * inverse_power[frame];
                }
                covariance[row * kWpeSize + column] = sum;
                covariance[column * kWpeSize + row] = std::conj(sum);
            }
            covariance[row * kWpeSize + row] += epsilon;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                WideComplex sum{};
                for (std::size_t frame = 0; frame < count; ++frame) {
                    sum += WideComplex(delayed[row * count + frame]) *
                           std::conj(WideComplex(original[index(channel, frequency, frame, count)])) * inverse_power[frame];
                }
                coefficients[2 * row + channel] = sum;
            }
        }
        solve(covariance, coefficients, kWpeSize, 2);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t frame = 0; frame < count; ++frame) {
                WideComplex value(original[index(channel, frequency, frame, count)]);
                for (std::size_t tap = 0; tap < kWpeSize; ++tap) {
                    value -= std::conj(coefficients[2 * tap + channel]) * WideComplex(delayed[tap * count + frame]);
                }
                output[index(channel, frequency, frame, count)] = narrow(value);
            }
        }
    }
    return output;
}

std::vector<Complex> auxiva(const std::vector<Complex> & input, std::size_t count) {
    constexpr double epsilon = 1.0e-10;
    std::vector<WideComplex> demixer(kBins * 4);
    for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
        demixer[frequency * 4] = 1.0;
        demixer[frequency * 4 + 3] = 1.0;
    }
    std::vector<Complex> output(input.size());
    std::vector<double> inverse_norm(2 * count);
    std::vector<WideComplex> matrix(4);
    std::vector<WideComplex> rhs(2);
    const auto demix = [&] {
        for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
            for (std::size_t source = 0; source < 2; ++source) {
                for (std::size_t frame = 0; frame < count; ++frame) {
                    const auto value = demixer[frequency * 4 + source * 2] * WideComplex(input[index(0, frequency, frame, count)]) +
                                       demixer[frequency * 4 + source * 2 + 1] * WideComplex(input[index(1, frequency, frame, count)]);
                    output[index(source, frequency, frame, count)] = narrow(value);
                }
            }
        }
    };
    for (std::size_t iteration = 0; iteration < 20; ++iteration) {
        demix();
        for (std::size_t source = 0; source < 2; ++source) {
            for (std::size_t frame = 0; frame < count; ++frame) {
                double sum = 0.0;
                for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
                    sum += static_cast<double>(std::norm(output[index(source, frequency, frame, count)]));
                }
                inverse_norm[source * count + frame] = 1.0 / std::max(2.0 * std::sqrt(sum), epsilon);
            }
        }
        for (std::size_t source = 0; source < 2; ++source) {
            for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
                std::array<WideComplex, 4> covariance{};
                for (std::size_t row = 0; row < 2; ++row) {
                    for (std::size_t column = 0; column < 2; ++column) {
                        WideComplex sum{};
                        for (std::size_t frame = 0; frame < count; ++frame) {
                            sum += WideComplex(input[index(row, frequency, frame, count)]) *
                                   std::conj(WideComplex(input[index(column, frequency, frame, count)])) * inverse_norm[source * count + frame];
                        }
                        covariance[row * 2 + column] = sum / static_cast<double>(count);
                    }
                }
                for (std::size_t row = 0; row < 2; ++row) {
                    for (std::size_t column = 0; column < 2; ++column) {
                        matrix[row * 2 + column] = demixer[frequency * 4 + row * 2] * covariance[column] +
                                                  demixer[frequency * 4 + row * 2 + 1] * covariance[2 + column];
                    }
                    matrix[row * 2 + row] += epsilon;
                    rhs[row] = row == source ? 1.0 : 0.0;
                }
                solve(matrix, rhs, 2, 1);
                std::array<WideComplex, 2> row{std::conj(rhs[0]), std::conj(rhs[1])};
                WideComplex denominator{};
                for (std::size_t a = 0; a < 2; ++a) {
                    for (std::size_t b = 0; b < 2; ++b) denominator += row[a] * covariance[a * 2 + b] * std::conj(row[b]);
                }
                const auto scale = std::sqrt(denominator + epsilon);
                if (std::abs(scale) <= std::numeric_limits<double>::min()) throw std::runtime_error("H-GTCRN IVA normalization is singular");
                for (std::size_t channel = 0; channel < 2; ++channel) demixer[frequency * 4 + source * 2 + channel] = row[channel] / scale;
            }
        }
    }
    demix();
    for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
        for (std::size_t source = 0; source < 2; ++source) {
            WideComplex numerator{};
            double denominator = 0.0;
            for (std::size_t frame = 0; frame < count; ++frame) {
                const auto value = output[index(source, frequency, frame, count)];
                numerator += std::conj(WideComplex(input[index(0, frequency, frame, count)])) * WideComplex(value);
                denominator += static_cast<double>(std::norm(value));
            }
            const auto projection = denominator > 0.0 ? std::conj(numerator / denominator) : WideComplex(1.0, 0.0);
            for (std::size_t frame = 0; frame < count; ++frame) {
                output[index(source, frequency, frame, count)] = narrow(WideComplex(output[index(source, frequency, frame, count)]) * projection);
            }
        }
    }
    return output;
}

}  // namespace

struct HgtcrnModel::State {
    GTCRNModel model;
    std::unique_ptr<GTCRNStreamingSession> session;
    std::shared_ptr<RealFFTPlan> fft;
    std::array<float, kFft> window{};
    std::size_t max_frames;
    HgtcrnStatistics measured;

    State(const std::filesystem::path & checkpoint, const core::BackendConfig & backend, std::size_t maximum)
        : max_frames(maximum == 0 ? kMaximumFrames : maximum) {
        if (max_frames < kMinimumFrames || max_frames > kMaximumFrames) throw std::invalid_argument("H-GTCRN max_frames must be between 4608 and 480000");
        if (backend.type != core::BackendType::Cpu || backend.threads <= 0 || backend.threads > 256) {
            throw std::invalid_argument("H-GTCRN full-segment validation requires CPU with 1 to 256 threads");
        }
        model = GTCRNModel::load_hgtcrn_from_safetensors(checkpoint, backend);
        session = model.create_streaming_session();
        fft = get_real_fft_plan(kFft);
        for (std::size_t i = 0; i < kFft; ++i) window[i] = 0.5F - 0.5F * std::cos(2.0F * kPi * static_cast<float>(i) / static_cast<float>(kFft));
    }

    std::vector<Complex> stft(const float * samples, std::size_t frames, std::size_t count) {
        std::vector<Complex> spectrum(2 * kBins * count);
        std::array<float, kFft> frame{};
        std::array<Complex, kBins> bins{};
        const TensorShape shape{kFft};
        const TensorStrideBytes real_stride{static_cast<std::ptrdiff_t>(sizeof(float))};
        const TensorStrideBytes complex_stride{static_cast<std::ptrdiff_t>(sizeof(Complex))};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t time = 0; time < count; ++time) {
                const auto start = static_cast<std::int64_t>(time * kHop) - static_cast<std::int64_t>(kHop);
                for (std::size_t i = 0; i < kFft; ++i) frame[i] = samples[2 * reflected(start + static_cast<std::int64_t>(i), frames) + channel] * window[i];
                fft->forward(shape, real_stride, complex_stride, 0, frame.data(), bins.data(), 1.0F, 1);
                for (std::size_t frequency = 0; frequency < kBins; ++frequency) spectrum[index(channel, frequency, time, count)] = bins[frequency];
            }
        }
        return spectrum;
    }

    std::vector<float> istft(const std::vector<Complex> & spectrum, std::size_t frames, std::size_t count) {
        std::vector<float> overlap(frames + kFft);
        std::vector<float> normalization(frames + kFft);
        std::array<float, kFft> frame{};
        std::array<Complex, kBins> bins{};
        const TensorShape shape{kFft};
        const TensorStrideBytes real_stride{static_cast<std::ptrdiff_t>(sizeof(float))};
        const TensorStrideBytes complex_stride{static_cast<std::ptrdiff_t>(sizeof(Complex))};
        for (std::size_t time = 0; time < count; ++time) {
            for (std::size_t frequency = 0; frequency < kBins; ++frequency) bins[frequency] = spectrum[frequency * count + time];
            fft->inverse(shape, complex_stride, real_stride, 0, bins.data(), frame.data(), 1.0F / static_cast<float>(kFft), 1);
            for (std::size_t i = 0; i < kFft; ++i) {
                overlap[time * kHop + i] += frame[i] * window[i];
                normalization[time * kHop + i] += window[i] * window[i];
            }
        }
        std::vector<float> output(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const auto padded = i + kHop;
            const float denominator = normalization[padded] > 1.0e-8F ? normalization[padded] : 1.0F;
            output[i] = overlap[padded] / denominator;
            if (!std::isfinite(output[i])) throw std::runtime_error("H-GTCRN returned non-finite waveform");
        }
        return output;
    }
};

HgtcrnModel::HgtcrnModel(const std::filesystem::path & checkpoint, const core::BackendConfig & backend, std::size_t max_frames)
    : state_(std::make_unique<State>(checkpoint, backend, max_frames)) {}
HgtcrnModel::~HgtcrnModel() = default;

std::vector<float> HgtcrnModel::run(const float * samples, std::size_t frames, std::uint64_t request_id,
                                  const HgtcrnStageObserver * observer) {
    auto & state = *state_;
    state.measured = {};
    auto & measured = state.measured;
    measured.request_id = request_id;
    measured.input_frames = frames;
    MeasureTime total(measured.total_ms);
    try {
        if (samples == nullptr || frames < kMinimumFrames || frames > state.max_frames) {
            throw std::invalid_argument("H-GTCRN requires 4608 to max_frames synchronous dual-channel frames");
        }
        std::array<double, 2> energy{};
        std::array<double, 2> sum{};
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const double a = samples[2 * frame];
            const double b = samples[2 * frame + 1];
            if (!std::isfinite(a) || !std::isfinite(b)) throw std::invalid_argument("H-GTCRN PCM must be finite");
            energy[0] += a * a;
            energy[1] += b * b;
            sum[0] += a;
            sum[1] += b;
        }
        const double count = static_cast<double>(frames);
        const double mean0 = sum[0] / count;
        const double mean1 = sum[1] / count;
        double covariance = 0.0;
        double variance0 = 0.0;
        double variance1 = 0.0;
        // Center explicitly instead of subtracting nearly equal energy sums.
        // A copied channel plus a DC offset still has rank-one AC content.
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const double a = static_cast<double>(samples[2 * frame]) - mean0;
            const double b = static_cast<double>(samples[2 * frame + 1]) - mean1;
            covariance += a * b;
            variance0 += a * a;
            variance1 += b * b;
        }
        const double spatial_scale = variance0 * variance1;
        const double normalization = std::sqrt(spatial_scale);
        if (!std::isfinite(energy[0]) || !std::isfinite(energy[1]) ||
            !std::isfinite(covariance) || !std::isfinite(variance0) || !std::isfinite(variance1) ||
            !std::isfinite(spatial_scale) || !std::isfinite(normalization)) {
            throw std::invalid_argument("H-GTCRN microphone covariance must be finite");
        }
        measured.channel_correlation = normalization > 0.0 ? std::clamp(covariance / normalization, -1.0, 1.0) : 0.0;
        measured.channel_rms_ratio = energy[0] > 0.0 ? std::sqrt(energy[1] / energy[0]) : 0.0;
        if (energy[0] == 0.0 && energy[1] == 0.0) {
            std::vector<float> output(frames, 0.0F);
            measured.output_frames = frames;
            measured.succeeded = true;
            return output;
        }
        // A stuck DC channel or centered linearly dependent channels contain no
        // spatial signal for IVA. Keep the original audio through host fallback.
        constexpr double relative_epsilon = 1.0e-12;
        const double determinant = spatial_scale - covariance * covariance;
        if (!std::isfinite(determinant) || variance0 <= energy[0] * relative_epsilon ||
            variance1 <= energy[1] * relative_epsilon || determinant <= spatial_scale * relative_epsilon) {
            throw std::invalid_argument("H-GTCRN needs two independent microphone channels; spatial covariance is rank deficient");
        }
        // A partial final hop has a weak Hann envelope if reconstructed on its
        // own. Reflect real dual-channel context to a whole hop before ALL
        // stages, then crop the reconstructed waveform to the original length.
        const auto right_padding = (kHop - frames % kHop) % kHop;
        const auto analysis_frames = frames + right_padding;
        measured.right_padding_frames = static_cast<std::uint32_t>(right_padding);
        std::vector<float> padded_input;
        const float * analysis_samples = samples;
        if (right_padding != 0) {
            padded_input.resize(2 * analysis_frames);
            std::copy_n(samples, 2 * frames, padded_input.data());
            for (std::size_t frame = frames; frame < analysis_frames; ++frame) {
                const auto source = reflected(static_cast<std::int64_t>(frame), frames);
                padded_input[2 * frame] = samples[2 * source];
                padded_input[2 * frame + 1] = samples[2 * source + 1];
            }
            analysis_samples = padded_input.data();
        }
        const auto spectrum_frames = 1 + analysis_frames / kHop;
        measured.stft_frames = spectrum_frames;
        std::vector<Complex> original;
        {
            MeasureTime timer(measured.stft_ms);
            original = state.stft(analysis_samples, analysis_frames, spectrum_frames);
        }
        observe(observer, "stft", original, 2, spectrum_frames);
        std::vector<Complex> dereverberated;
        {
            MeasureTime timer(measured.wpe_ms);
            dereverberated = wpe(original, spectrum_frames);
        }
        observe(observer, "wpe", dereverberated, 2, spectrum_frames);
        std::vector<Complex> separated;
        {
            MeasureTime timer(measured.iva_ms);
            separated = auxiva(dereverberated, spectrum_frames);
            measured.iva_iterations = 20;
        }
        observe(observer, "iva", separated, 2, spectrum_frames);
        std::array<double, 2> separated_energy{};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
                for (std::size_t frame = 0; frame < spectrum_frames; ++frame) {
                    separated_energy[channel] += static_cast<double>(std::norm(separated[index(channel, frequency, frame, spectrum_frames)]));
                }
            }
        }
        // Preserve the official formula: selected source has LOWER energy.
        const std::size_t selected = separated_energy[0] < separated_energy[1] ? 0 : 1;
        measured.iva_speech_channel = static_cast<std::uint32_t>(selected);
        std::vector<Complex> enhanced(kBins * spectrum_frames);
        {
            MeasureTime timer(measured.network_ms);
            state.session->reset();
            std::array<float, 6 * kBins> features{};
            std::array<float, 2 * kBins> reference{};
            std::array<float, 2 * kBins> output{};
            for (std::size_t frame = 0; frame < spectrum_frames; ++frame) {
                for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
                    for (std::size_t channel = 0; channel < 2; ++channel) {
                        const auto value = original[index(channel, frequency, frame, spectrum_frames)];
                        features[(2 * channel) * kBins + frequency] = value.real();
                        features[(2 * channel + 1) * kBins + frequency] = value.imag();
                    }
                    features[4 * kBins + frequency] = std::log10(std::max(std::abs(separated[index(selected, frequency, frame, spectrum_frames)]), 1.0e-12F));
                    features[5 * kBins + frequency] = std::log10(std::max(std::abs(separated[index(1 - selected, frequency, frame, spectrum_frames)]), 1.0e-12F));
                    reference[2 * frequency] = features[frequency];
                    reference[2 * frequency + 1] = features[kBins + frequency];
                }
                state.session->process_hgtcrn_frame(features.data(), reference.data(), output.data());
                for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
                    enhanced[frequency * spectrum_frames + frame] = {output[2 * frequency], output[2 * frequency + 1]};
                }
            }
        }
        observe(observer, "network", enhanced, 1, spectrum_frames);
        std::vector<float> output;
        {
            MeasureTime timer(measured.istft_ms);
            output = state.istft(enhanced, analysis_frames, spectrum_frames);
            output.resize(frames);
        }
        measured.output_frames = output.size();
        measured.succeeded = true;
        return output;
    } catch (...) {
        measured.failed = true;
        throw;
    }
}

HgtcrnStatistics HgtcrnModel::statistics() const noexcept { return state_->measured; }

}  // namespace engine::audio
