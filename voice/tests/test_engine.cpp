#include "check.hpp"
#include "engine/asr_session.hpp"
#include "engine/tts_session.hpp"
#include "audio/audio_render.hpp"
HMI_TEST(engine_wrappers_reject_unopened_state)
{
    hmi::engine::Client client;
    hmi::engine::AsrSession asr;
    std::string error;
    hmi::engine::Transcript transcript;
    std::vector<hmi::engine::Transcript> partials;
    CHECK(!asr.ready());
    CHECK(!asr.begin(&error));
    CHECK(!error.empty());
    CHECK(!asr.push(nullptr, 0, partials, &error));
    CHECK(!asr.finish(transcript, &error));
    CHECK(!asr.open(client, {}, &error));
    CHECK(transcript.confidence == -1.0f);
    asr.reset(); asr.close(); asr.close();
    hmi::engine::TtsSession tts;
    hmi::audio::AudioBuffer audio;
    CHECK(!tts.busy());
    CHECK(!tts.speak("hello"));
    CHECK(!tts.open(client, {}, &error));
    CHECK(!tts.synthesize("hello", audio, &error));
    CHECK(!tts.speak("\xff"));
    CHECK(!tts.speak(" \n"));
    tts.stop(); tts.stop();
    hmi::audio::AudioRenderer renderer;
    CHECK(!renderer.play(audio, &error));
    audio.samples = {0.0f}; audio.sample_rate = 0;
    CHECK(!renderer.play(audio, &error));
    renderer.stop(); renderer.stop();
}
