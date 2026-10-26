#ifndef XBRT_SHIM_SDL_H
#define XBRT_SHIM_SDL_H

#include <stdbool.h>
#include <stdint.h>

#define SDL_INIT_AUDIO 0x10u
#define SDL_AUDIO_S16LE 0x8010u
#define SDL_AUDIO_BYTESIZE(fmt) (((fmt) & 0xFFu) / 8)
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)

typedef uint32_t SDL_AudioDeviceID;
typedef unsigned SDL_AudioFormat;
typedef struct SDL_AudioSpec {
    SDL_AudioFormat format;
    int channels;
    int freq;
} SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;

bool SDL_Init(uint32_t flags);
const char *SDL_GetError(void);
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID dev, const SDL_AudioSpec *spec, void *callback,
                                           void *userdata);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream);
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID dev, SDL_AudioSpec *spec, int *sample_frames);
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID dev);
void SDL_DestroyAudioStream(SDL_AudioStream *stream);
bool SDL_SetAudioStreamGain(SDL_AudioStream *stream, float gain);
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len);
int SDL_GetAudioStreamQueued(SDL_AudioStream *stream);
int SDL_GetNumLogicalCPUCores(void);
uint64_t SDL_GetTicks(void);

#endif
