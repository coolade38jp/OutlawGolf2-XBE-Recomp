// mixer output through an SDL audio stream
#include <SDL3/SDL.h>

#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr uint32_t kFrames = kRate / 100;  // 10 ms blocks, as the XAudio2 output queued them

struct Pump {
    std::function<void(float*, uint32_t)> render;
    std::vector<float> buf = std::vector<float>(kFrames * 2);
};

void SDLCALL Feed(void* user, SDL_AudioStream* stream, int additional, int) {
    auto& p = *static_cast<Pump*>(user);
    int want = additional > 0 ? additional : int(kFrames * 2 * sizeof(float));
    // whole 10 ms blocks, as the mixer renders them
    while (want >= int(kFrames * 2 * sizeof(float))) {
        p.render(p.buf.data(), kFrames);
        SDL_PutAudioStreamData(stream, p.buf.data(), int(kFrames * 2 * sizeof(float)));
        want -= int(kFrames * 2 * sizeof(float));
    }
}

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: SDL audio unavailable, no sound: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kRate)};
    auto* pump = new Pump{std::move(render)};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, pump);
    if (!stream) {
        LOGE("audio: no output device, no sound: %s", SDL_GetError());
        delete pump;
        return;
    }
    LOGI("audio: stereo output through %s", SDL_GetCurrentAudioDriver());
    SDL_ResumeAudioStreamDevice(stream);
}

}  // namespace audio
