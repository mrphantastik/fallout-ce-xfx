#include "audio_engine.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx-presets.h>
#include <AL/efx.h>

#include "plib/gnw/debug.h"

namespace fallout {

#define AUDIO_ENGINE_SOUND_BUFFERS 32
#define AUDIO_ENGINE_STREAM_QUEUE_BUFFERS 6
#define AUDIO_ENGINE_STREAM_CHUNK_BYTES 2048

struct AudioEngineSoundBuffer {
    bool active;
    unsigned int size;
    int bitsPerSample;
    int channels;
    int rate;
    unsigned char* data;
    int volume;
    int pan;
    bool playing;
    bool looping;
    bool paused;
    unsigned int playPos;
    unsigned int feedPos;

    ALuint alSource;
    ALuint alBuffers[AUDIO_ENGINE_STREAM_QUEUE_BUFFERS];
    unsigned int queuedChunkBytes[AUDIO_ENGINE_STREAM_QUEUE_BUFFERS];
    int queueHead;
    int queueTail;
    int queuedCount;

    ALuint directFilter;
    ALuint auxSendFilter;

    bool spatialized;
    bool eaxSendEnabled;
    float posX;
    float posY;
    float posZ;
    float directGain;
    float directGainHF;
    float sendGain;
    float sendGainHF;

    std::recursive_mutex mutex;
};

extern bool GNW95_isActive;

static ALCdevice* gAlDevice = nullptr;
static ALCcontext* gAlContext = nullptr;
static AudioEngineSoundBuffer gAudioEngineSoundBuffers[AUDIO_ENGINE_SOUND_BUFFERS];

static int gSurroundMode = AUDIO_SURROUND_MODE_AUTO;
static bool gEaxEnabled = true;
static bool gEfxSupported = false;
static bool gAudioPaused = false;

static ALuint gEfxAuxSlot = 0;
static ALuint gEfxReverbEffect = 0;

static std::mutex gEaxMutex;
static int gCurrentEaxPresetId = AUDIO_EAX_ENVIRONMENT_GENERIC;
static EFXEAXREVERBPROPERTIES gCurrentEaxProps = EFX_REVERB_PRESET_GENERIC;
static EFXEAXREVERBPROPERTIES gTargetEaxProps = EFX_REVERB_PRESET_GENERIC;
static float gCurrentWetGain = 0.65f;
static float gTargetWetGain = 0.65f;
static bool gEaxDirty = false;

static std::atomic<bool> gWorkerRunning{ false };
static std::thread gWorkerThread;

static const EFXEAXREVERBPROPERTIES kEax2Presets[AUDIO_EAX_ENVIRONMENT_COUNT] = {
    EFX_REVERB_PRESET_GENERIC,
    EFX_REVERB_PRESET_PADDEDCELL,
    EFX_REVERB_PRESET_ROOM,
    EFX_REVERB_PRESET_BATHROOM,
    EFX_REVERB_PRESET_LIVINGROOM,
    EFX_REVERB_PRESET_STONEROOM,
    EFX_REVERB_PRESET_AUDITORIUM,
    EFX_REVERB_PRESET_CONCERTHALL,
    EFX_REVERB_PRESET_CAVE,
    EFX_REVERB_PRESET_ARENA,
    EFX_REVERB_PRESET_HANGAR,
    EFX_REVERB_PRESET_CARPETEDHALLWAY,
    EFX_REVERB_PRESET_HALLWAY,
    EFX_REVERB_PRESET_STONECORRIDOR,
    EFX_REVERB_PRESET_ALLEY,
    EFX_REVERB_PRESET_FOREST,
    EFX_REVERB_PRESET_CITY,
    EFX_REVERB_PRESET_MOUNTAINS,
    EFX_REVERB_PRESET_QUARRY,
    EFX_REVERB_PRESET_PLAIN,
    EFX_REVERB_PRESET_PARKINGLOT,
    EFX_REVERB_PRESET_SEWERPIPE,
    EFX_REVERB_PRESET_UNDERWATER,
    EFX_REVERB_PRESET_DRUGGED,
    EFX_REVERB_PRESET_DIZZY,
    EFX_REVERB_PRESET_PSYCHOTIC,
};

static const char* kEax2PresetNames[AUDIO_EAX_ENVIRONMENT_COUNT] = {
    "Generic",
    "Padded Cell",
    "Room",
    "Bathroom",
    "Living Room",
    "Stone Room",
    "Auditorium",
    "Concert Hall",
    "Cave",
    "Arena",
    "Hangar",
    "Carpeted Hallway",
    "Hallway",
    "Stone Corridor",
    "Alley",
    "Forest",
    "City",
    "Mountains",
    "Quarry",
    "Plain",
    "Parking Lot",
    "Sewer Pipe",
    "Underwater",
    "Drugged",
    "Dizzy",
    "Psychotic",
};

static inline float clampFloat(float value, float minVal, float maxVal)
{
    if (value < minVal) {
        return minVal;
    }
    if (value > maxVal) {
        return maxVal;
    }
    return value;
}

static inline float lerpFloat(float a, float b, float t)
{
    return a + (b - a) * t;
}

static bool audioEngineIsInitialized()
{
    return gAlDevice != nullptr && gAlContext != nullptr;
}

static bool soundBufferIsValid(int soundBufferIndex)
{
    return soundBufferIndex >= 0 && soundBufferIndex < AUDIO_ENGINE_SOUND_BUFFERS;
}

static void applyEaxPropertiesToEffect(ALuint effect, const EFXEAXREVERBPROPERTIES& props)
{
    alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
    alEffectf(effect, AL_EAXREVERB_DENSITY, clampFloat(props.flDensity, AL_EAXREVERB_MIN_DENSITY, AL_EAXREVERB_MAX_DENSITY));
    alEffectf(effect, AL_EAXREVERB_DIFFUSION, clampFloat(props.flDiffusion, AL_EAXREVERB_MIN_DIFFUSION, AL_EAXREVERB_MAX_DIFFUSION));
    alEffectf(effect, AL_EAXREVERB_GAIN, clampFloat(props.flGain, AL_EAXREVERB_MIN_GAIN, AL_EAXREVERB_MAX_GAIN));
    alEffectf(effect, AL_EAXREVERB_GAINHF, clampFloat(props.flGainHF, AL_EAXREVERB_MIN_GAINHF, AL_EAXREVERB_MAX_GAINHF));
    alEffectf(effect, AL_EAXREVERB_GAINLF, clampFloat(props.flGainLF, AL_EAXREVERB_MIN_GAINLF, AL_EAXREVERB_MAX_GAINLF));
    alEffectf(effect, AL_EAXREVERB_DECAY_TIME, clampFloat(props.flDecayTime, AL_EAXREVERB_MIN_DECAY_TIME, AL_EAXREVERB_MAX_DECAY_TIME));
    alEffectf(effect, AL_EAXREVERB_DECAY_HFRATIO, clampFloat(props.flDecayHFRatio, AL_EAXREVERB_MIN_DECAY_HFRATIO, AL_EAXREVERB_MAX_DECAY_HFRATIO));
    alEffectf(effect, AL_EAXREVERB_DECAY_LFRATIO, clampFloat(props.flDecayLFRatio, AL_EAXREVERB_MIN_DECAY_LFRATIO, AL_EAXREVERB_MAX_DECAY_LFRATIO));
    alEffectf(effect, AL_EAXREVERB_REFLECTIONS_GAIN, clampFloat(props.flReflectionsGain, AL_EAXREVERB_MIN_REFLECTIONS_GAIN, AL_EAXREVERB_MAX_REFLECTIONS_GAIN));
    alEffectf(effect, AL_EAXREVERB_REFLECTIONS_DELAY, clampFloat(props.flReflectionsDelay, AL_EAXREVERB_MIN_REFLECTIONS_DELAY, AL_EAXREVERB_MAX_REFLECTIONS_DELAY));
    alEffectfv(effect, AL_EAXREVERB_REFLECTIONS_PAN, props.flReflectionsPan);
    alEffectf(effect, AL_EAXREVERB_LATE_REVERB_GAIN, clampFloat(props.flLateReverbGain, AL_EAXREVERB_MIN_LATE_REVERB_GAIN, AL_EAXREVERB_MAX_LATE_REVERB_GAIN));
    alEffectf(effect, AL_EAXREVERB_LATE_REVERB_DELAY, clampFloat(props.flLateReverbDelay, AL_EAXREVERB_MIN_LATE_REVERB_DELAY, AL_EAXREVERB_MAX_LATE_REVERB_DELAY));
    alEffectfv(effect, AL_EAXREVERB_LATE_REVERB_PAN, props.flLateReverbPan);
    alEffectf(effect, AL_EAXREVERB_ECHO_TIME, clampFloat(props.flEchoTime, AL_EAXREVERB_MIN_ECHO_TIME, AL_EAXREVERB_MAX_ECHO_TIME));
    alEffectf(effect, AL_EAXREVERB_ECHO_DEPTH, clampFloat(props.flEchoDepth, AL_EAXREVERB_MIN_ECHO_DEPTH, AL_EAXREVERB_MAX_ECHO_DEPTH));
    alEffectf(effect, AL_EAXREVERB_MODULATION_TIME, clampFloat(props.flModulationTime, AL_EAXREVERB_MIN_MODULATION_TIME, AL_EAXREVERB_MAX_MODULATION_TIME));
    alEffectf(effect, AL_EAXREVERB_MODULATION_DEPTH, clampFloat(props.flModulationDepth, AL_EAXREVERB_MIN_MODULATION_DEPTH, AL_EAXREVERB_MAX_MODULATION_DEPTH));
    alEffectf(effect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, clampFloat(props.flAirAbsorptionGainHF, AL_EAXREVERB_MIN_AIR_ABSORPTION_GAINHF, AL_EAXREVERB_MAX_AIR_ABSORPTION_GAINHF));
    alEffectf(effect, AL_EAXREVERB_HFREFERENCE, clampFloat(props.flHFReference, AL_EAXREVERB_MIN_HFREFERENCE, AL_EAXREVERB_MAX_HFREFERENCE));
    alEffectf(effect, AL_EAXREVERB_LFREFERENCE, clampFloat(props.flLFReference, AL_EAXREVERB_MIN_LFREFERENCE, AL_EAXREVERB_MAX_LFREFERENCE));
    alEffectf(effect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, clampFloat(props.flRoomRolloffFactor, AL_EAXREVERB_MIN_ROOM_ROLLOFF_FACTOR, AL_EAXREVERB_MAX_ROOM_ROLLOFF_FACTOR));
    alEffecti(effect, AL_EAXREVERB_DECAY_HFLIMIT, props.iDecayHFLimit ? AL_TRUE : AL_FALSE);
}

static bool interpolateEaxProps(EFXEAXREVERBPROPERTIES& current, const EFXEAXREVERBPROPERTIES& target, float step)
{
    float maxDiff = 0.0f;
    auto stepField = [&](float& c, float t) {
        float diff = fabsf(t - c);
        if (diff > maxDiff) {
            maxDiff = diff;
        }
        c = lerpFloat(c, t, step);
    };

    stepField(current.flDensity, target.flDensity);
    stepField(current.flDiffusion, target.flDiffusion);
    stepField(current.flGain, target.flGain);
    stepField(current.flGainHF, target.flGainHF);
    stepField(current.flGainLF, target.flGainLF);
    stepField(current.flDecayTime, target.flDecayTime);
    stepField(current.flDecayHFRatio, target.flDecayHFRatio);
    stepField(current.flDecayLFRatio, target.flDecayLFRatio);
    stepField(current.flReflectionsGain, target.flReflectionsGain);
    stepField(current.flReflectionsDelay, target.flReflectionsDelay);
    stepField(current.flLateReverbGain, target.flLateReverbGain);
    stepField(current.flLateReverbDelay, target.flLateReverbDelay);
    stepField(current.flEchoTime, target.flEchoTime);
    stepField(current.flEchoDepth, target.flEchoDepth);
    stepField(current.flModulationTime, target.flModulationTime);
    stepField(current.flModulationDepth, target.flModulationDepth);
    stepField(current.flAirAbsorptionGainHF, target.flAirAbsorptionGainHF);
    stepField(current.flHFReference, target.flHFReference);
    stepField(current.flLFReference, target.flLFReference);
    stepField(current.flRoomRolloffFactor, target.flRoomRolloffFactor);
    current.iDecayHFLimit = target.iDecayHFLimit;

    return maxDiff > 0.002f;
}

static void applyBufferRoutingAndSpatialState(AudioEngineSoundBuffer* sb)
{
    if (!sb->active || sb->alSource == 0) {
        return;
    }

    float normalizedGain = clampFloat((float)sb->volume / 128.0f, 0.0f, 1.0f);
    alSourcef(sb->alSource, AL_GAIN, normalizedGain);

    if (sb->spatialized && gSurroundMode != AUDIO_SURROUND_MODE_STEREO) {
        alSourcei(sb->alSource, AL_SOURCE_RELATIVE, AL_FALSE);
        alSourcei(sb->alSource, AL_SOURCE_SPATIALIZE_SOFT, AL_TRUE);
        alSource3f(sb->alSource, AL_POSITION, sb->posX, sb->posY, sb->posZ);
        alSourcef(sb->alSource, AL_REFERENCE_DISTANCE, 4.0f);
        alSourcef(sb->alSource, AL_MAX_DISTANCE, 45.0f);
        alSourcef(sb->alSource, AL_ROLLOFF_FACTOR, 0.35f);
    } else if (sb->spatialized) {
        // Stereo panning mode with 3D coordinates mapped across the stereo stage
        alSourcei(sb->alSource, AL_SOURCE_RELATIVE, AL_FALSE);
        alSourcei(sb->alSource, AL_SOURCE_SPATIALIZE_SOFT, AL_TRUE);
        alSource3f(sb->alSource, AL_POSITION, sb->posX, 0.0f, sb->posZ);
        alSourcef(sb->alSource, AL_REFERENCE_DISTANCE, 5.0f);
        alSourcef(sb->alSource, AL_MAX_DISTANCE, 45.0f);
        alSourcef(sb->alSource, AL_ROLLOFF_FACTOR, 0.25f);
    } else {
        // 2D non-positional source (UI, music, speech, movies)
        alSourcei(sb->alSource, AL_SOURCE_RELATIVE, AL_TRUE);
        float panOffset = clampFloat((float)sb->pan / 10000.0f, -1.0f, 1.0f);
        if (panOffset != 0.0f) {
            alSourcei(sb->alSource, AL_SOURCE_SPATIALIZE_SOFT, AL_TRUE);
            alSource3f(sb->alSource, AL_POSITION, panOffset, 0.0f, -1.0f);
        } else {
            alSourcei(sb->alSource, AL_SOURCE_SPATIALIZE_SOFT, AL_FALSE);
            alSource3f(sb->alSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
        }
        alSourcef(sb->alSource, AL_ROLLOFF_FACTOR, 0.0f);
    }

    if (gEfxSupported) {
        if (sb->directFilter != 0) {
            alFilteri(sb->directFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
            alFilterf(sb->directFilter, AL_LOWPASS_GAIN, clampFloat(sb->directGain, AL_LOWPASS_MIN_GAIN, AL_LOWPASS_MAX_GAIN));
            alFilterf(sb->directFilter, AL_LOWPASS_GAINHF, clampFloat(sb->directGainHF, AL_LOWPASS_MIN_GAINHF, AL_LOWPASS_MAX_GAINHF));
            alSourcei(sb->alSource, AL_DIRECT_FILTER, sb->directFilter);
        }

        if (gEaxEnabled && sb->eaxSendEnabled && gEfxAuxSlot != 0) {
            if (sb->auxSendFilter != 0) {
                alFilteri(sb->auxSendFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
                alFilterf(sb->auxSendFilter, AL_LOWPASS_GAIN, clampFloat(sb->sendGain, AL_LOWPASS_MIN_GAIN, AL_LOWPASS_MAX_GAIN));
                alFilterf(sb->auxSendFilter, AL_LOWPASS_GAINHF, clampFloat(sb->sendGainHF, AL_LOWPASS_MIN_GAINHF, AL_LOWPASS_MAX_GAINHF));
                alSource3i(sb->alSource, AL_AUXILIARY_SEND_FILTER, (ALint)gEfxAuxSlot, 0, (ALint)sb->auxSendFilter);
            } else {
                alSource3i(sb->alSource, AL_AUXILIARY_SEND_FILTER, (ALint)gEfxAuxSlot, 0, AL_FILTER_NULL);
            }
        } else {
            alSource3i(sb->alSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        }
    }
}

static void unqueueAllBuffersLocked(AudioEngineSoundBuffer* sb)
{
    if (sb->alSource == 0) {
        return;
    }

    alSourceStop(sb->alSource);

    ALint queued = 0;
    alGetSourcei(sb->alSource, AL_BUFFERS_QUEUED, &queued);
    while (queued > 0) {
        ALuint removed = 0;
        alSourceUnqueueBuffers(sb->alSource, 1, &removed);
        queued--;
    }

    sb->queueHead = 0;
    sb->queueTail = 0;
    sb->queuedCount = 0;
    memset(sb->queuedChunkBytes, 0, sizeof(sb->queuedChunkBytes));
}

static bool enqueueNextChunkLocked(AudioEngineSoundBuffer* sb)
{
    if (!sb->active || sb->data == nullptr || sb->size == 0) {
        return false;
    }

    if (sb->queuedCount >= AUDIO_ENGINE_STREAM_QUEUE_BUFFERS) {
        return false;
    }

    if (!sb->looping && sb->feedPos >= sb->size) {
        return false;
    }

    unsigned int frameAlign = (unsigned int)((sb->bitsPerSample / 8) * sb->channels);
    if (frameAlign == 0) {
        frameAlign = 1;
    }

    unsigned int chunkTarget = AUDIO_ENGINE_STREAM_CHUNK_BYTES;
    chunkTarget = (chunkTarget / frameAlign) * frameAlign;
    if (chunkTarget == 0) {
        chunkTarget = frameAlign;
    }

    unsigned char rawChunk[AUDIO_ENGINE_STREAM_CHUNK_BYTES];
    unsigned int bytesCollected = 0;

    while (bytesCollected < chunkTarget) {
        if (sb->feedPos >= sb->size) {
            if (sb->looping) {
                sb->feedPos %= sb->size;
            } else {
                break;
            }
        }

        unsigned int avail = sb->size - sb->feedPos;
        unsigned int toCopy = chunkTarget - bytesCollected;
        if (toCopy > avail) {
            toCopy = avail;
        }

        memcpy(rawChunk + bytesCollected, sb->data + sb->feedPos, toCopy);
        sb->feedPos += toCopy;
        bytesCollected += toCopy;

        if (!sb->looping && sb->feedPos >= sb->size) {
            break;
        }
    }

    if (bytesCollected == 0) {
        return false;
    }

    int slot = sb->queueTail;
    ALuint alBuf = sb->alBuffers[slot];
    ALenum format = (sb->channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;

    if (sb->bitsPerSample == 8) {
        // Fallout 8-bit buffers are signed 8-bit PCM; convert to 16-bit signed PCM for OpenAL
        int16_t pcm16[AUDIO_ENGINE_STREAM_CHUNK_BYTES];
        const int8_t* src8 = reinterpret_cast<const int8_t*>(rawChunk);
        for (unsigned int i = 0; i < bytesCollected; i++) {
            pcm16[i] = static_cast<int16_t>(src8[i]) << 8;
        }
        alBufferData(alBuf, format, pcm16, (ALsizei)(bytesCollected * sizeof(int16_t)), sb->rate);
    } else {
        alBufferData(alBuf, format, rawChunk, (ALsizei)bytesCollected, sb->rate);
    }

    alSourceQueueBuffers(sb->alSource, 1, &alBuf);
    sb->queuedChunkBytes[slot] = bytesCollected;
    sb->queueTail = (sb->queueTail + 1) % AUDIO_ENGINE_STREAM_QUEUE_BUFFERS;
    sb->queuedCount++;

    return true;
}

static void pumpSoundBufferLocked(AudioEngineSoundBuffer* sb)
{
    if (!sb->active || !sb->playing || sb->alSource == 0) {
        return;
    }

    ALint processed = 0;
    alGetSourcei(sb->alSource, AL_BUFFERS_PROCESSED, &processed);
    while (processed > 0 && sb->queuedCount > 0) {
        ALuint unqueuedBuf = 0;
        alSourceUnqueueBuffers(sb->alSource, 1, &unqueuedBuf);

        unsigned int finishedBytes = sb->queuedChunkBytes[sb->queueHead];
        sb->queuedChunkBytes[sb->queueHead] = 0;
        sb->queueHead = (sb->queueHead + 1) % AUDIO_ENGINE_STREAM_QUEUE_BUFFERS;
        sb->queuedCount--;

        sb->playPos += finishedBytes;
        if (sb->size > 0) {
            if (sb->looping) {
                sb->playPos %= sb->size;
            } else if (sb->playPos > sb->size) {
                sb->playPos = sb->size;
            }
        }

        processed--;
    }

    while (sb->queuedCount < AUDIO_ENGINE_STREAM_QUEUE_BUFFERS) {
        if (!enqueueNextChunkLocked(sb)) {
            break;
        }
    }

    ALint state = AL_INITIAL;
    alGetSourcei(sb->alSource, AL_SOURCE_STATE, &state);

    if (!GNW95_isActive || gAudioPaused || sb->paused) {
        if (state == AL_PLAYING) {
            alSourcePause(sb->alSource);
        }
        return;
    }

    if (sb->queuedCount > 0) {
        if (state != AL_PLAYING) {
            alSourcePlay(sb->alSource);
        }
    } else {
        if (!sb->looping && sb->feedPos >= sb->size) {
            sb->playing = false;
            sb->playPos = 0;
            sb->feedPos = 0;
        }
    }
}

static void audioEngineWorkerProc()
{
    while (gWorkerRunning.load()) {
        if (gEfxSupported && gEaxEnabled) {
            std::lock_guard<std::mutex> eaxLock(gEaxMutex);
            bool stillMorphing = interpolateEaxProps(gCurrentEaxProps, gTargetEaxProps, 0.12f);
            float wetDiff = fabsf(gTargetWetGain - gCurrentWetGain);
            gCurrentWetGain = lerpFloat(gCurrentWetGain, gTargetWetGain, 0.12f);

            if (gEaxDirty || stillMorphing || wetDiff > 0.002f) {
                applyEaxPropertiesToEffect(gEfxReverbEffect, gCurrentEaxProps);
                alAuxiliaryEffectSloti(gEfxAuxSlot, AL_EFFECTSLOT_EFFECT, (ALint)gEfxReverbEffect);
                alAuxiliaryEffectSlotf(gEfxAuxSlot, AL_EFFECTSLOT_GAIN, clampFloat(gCurrentWetGain, 0.0f, 1.0f));
                gEaxDirty = stillMorphing || (wetDiff > 0.002f);
            }
        }

        for (int index = 0; index < AUDIO_ENGINE_SOUND_BUFFERS; index++) {
            AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[index]);
            std::lock_guard<std::recursive_mutex> lock(sb->mutex);
            if (sb->active && sb->playing) {
                pumpSoundBufferLocked(sb);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void audioEngineSetConfig(int surroundMode, bool eaxEnabled)
{
    if (surroundMode < AUDIO_SURROUND_MODE_STEREO || surroundMode > AUDIO_SURROUND_MODE_HRTF) {
        surroundMode = AUDIO_SURROUND_MODE_AUTO;
    }
    gSurroundMode = surroundMode;
    gEaxEnabled = eaxEnabled;
}

int audioEngineGetSurroundMode()
{
    return gSurroundMode;
}

bool audioEngineIsEaxEnabled()
{
    return gEaxEnabled && gEfxSupported;
}

bool audioEngineInit()
{
    if (audioEngineIsInitialized()) {
        return true;
    }

    gAlDevice = alcOpenDevice(nullptr);
    if (gAlDevice == nullptr) {
        debug_printf("audioEngineInit: alcOpenDevice failed.\n");
        return false;
    }

    std::vector<ALCint> contextAttrs;
    contextAttrs.push_back(ALC_FREQUENCY);
    contextAttrs.push_back(44100);
    contextAttrs.push_back(ALC_MAX_AUXILIARY_SENDS);
    contextAttrs.push_back(2);

    if (alcIsExtensionPresent(gAlDevice, "ALC_SOFT_HRTF")) {
        contextAttrs.push_back(ALC_HRTF_SOFT);
        if (gSurroundMode == AUDIO_SURROUND_MODE_HRTF) {
            contextAttrs.push_back(ALC_TRUE);
        } else if (gSurroundMode == AUDIO_SURROUND_MODE_STEREO) {
            contextAttrs.push_back(ALC_FALSE);
        } else {
            contextAttrs.push_back(ALC_DONT_CARE_SOFT);
        }
    }
    contextAttrs.push_back(0);

    gAlContext = alcCreateContext(gAlDevice, contextAttrs.data());
    if (gAlContext == nullptr) {
        gAlContext = alcCreateContext(gAlDevice, nullptr);
    }

    if (gAlContext == nullptr || !alcMakeContextCurrent(gAlContext)) {
        debug_printf("audioEngineInit: alcCreateContext/alcMakeContextCurrent failed.\n");
        if (gAlContext != nullptr) {
            alcDestroyContext(gAlContext);
            gAlContext = nullptr;
        }
        alcCloseDevice(gAlDevice);
        gAlDevice = nullptr;
        return false;
    }

    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    alDopplerFactor(0.25f);
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    const ALfloat listenerOri[6] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
    alListenerfv(AL_ORIENTATION, listenerOri);

    gEfxSupported = alcIsExtensionPresent(gAlDevice, "ALC_EXT_EFX") != AL_FALSE;
    if (gEfxSupported) {
        alGetError();
        alGenEffects(1, &gEfxReverbEffect);
        alGenAuxiliaryEffectSlots(1, &gEfxAuxSlot);
        if (alGetError() == AL_NO_ERROR && gEfxReverbEffect != 0 && gEfxAuxSlot != 0) {
            gCurrentEaxPresetId = AUDIO_EAX_ENVIRONMENT_GENERIC;
            gCurrentEaxProps = EFX_REVERB_PRESET_GENERIC;
            gTargetEaxProps = EFX_REVERB_PRESET_GENERIC;
            applyEaxPropertiesToEffect(gEfxReverbEffect, gCurrentEaxProps);
            alAuxiliaryEffectSloti(gEfxAuxSlot, AL_EFFECTSLOT_EFFECT, (ALint)gEfxReverbEffect);
            alAuxiliaryEffectSlotf(gEfxAuxSlot, AL_EFFECTSLOT_GAIN, gEaxEnabled ? gCurrentWetGain : 0.0f);
        } else {
            gEfxSupported = false;
        }
    }

    gAudioPaused = false;
    gWorkerRunning.store(true);
    gWorkerThread = std::thread(audioEngineWorkerProc);

    return true;
}

void audioEngineExit()
{
    if (!audioEngineIsInitialized()) {
        return;
    }

    gWorkerRunning.store(false);
    if (gWorkerThread.joinable()) {
        gWorkerThread.join();
    }

    for (int index = 0; index < AUDIO_ENGINE_SOUND_BUFFERS; index++) {
        audioEngineSoundBufferRelease(index);
    }

    if (gEfxSupported) {
        if (gEfxAuxSlot != 0) {
            alAuxiliaryEffectSloti(gEfxAuxSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
            alDeleteAuxiliaryEffectSlots(1, &gEfxAuxSlot);
            gEfxAuxSlot = 0;
        }
        if (gEfxReverbEffect != 0) {
            alDeleteEffects(1, &gEfxReverbEffect);
            gEfxReverbEffect = 0;
        }
        gEfxSupported = false;
    }

    alcMakeContextCurrent(nullptr);
    alcDestroyContext(gAlContext);
    gAlContext = nullptr;
    alcCloseDevice(gAlDevice);
    gAlDevice = nullptr;
}

void audioEnginePause()
{
    if (!audioEngineIsInitialized()) {
        return;
    }
    gAudioPaused = true;
}

void audioEngineResume()
{
    if (!audioEngineIsInitialized()) {
        return;
    }
    gAudioPaused = false;
}

int audioEngineCreateSoundBuffer(unsigned int size, int bitsPerSample, int channels, int rate)
{
    if (!audioEngineIsInitialized() || size == 0) {
        return -1;
    }

    for (int index = 0; index < AUDIO_ENGINE_SOUND_BUFFERS; index++) {
        AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[index]);
        std::lock_guard<std::recursive_mutex> lock(sb->mutex);

        if (!sb->active) {
            alGetError();
            ALuint source = 0;
            alGenSources(1, &source);
            if (alGetError() != AL_NO_ERROR || source == 0) {
                return -1;
            }

            ALuint buffers[AUDIO_ENGINE_STREAM_QUEUE_BUFFERS] = { 0 };
            alGenBuffers(AUDIO_ENGINE_STREAM_QUEUE_BUFFERS, buffers);
            if (alGetError() != AL_NO_ERROR) {
                alDeleteSources(1, &source);
                return -1;
            }

            sb->directFilter = 0;
            sb->auxSendFilter = 0;
            if (gEfxSupported) {
                ALuint filters[2] = { 0, 0 };
                alGenFilters(2, filters);
                if (alGetError() == AL_NO_ERROR) {
                    sb->directFilter = filters[0];
                    sb->auxSendFilter = filters[1];
                }
            }

            sb->active = true;
            sb->size = size;
            sb->bitsPerSample = bitsPerSample;
            sb->channels = channels;
            sb->rate = rate;
            sb->data = static_cast<unsigned char*>(malloc(size));
            if (sb->data != nullptr) {
                memset(sb->data, 0, size);
            }
            sb->volume = 128;
            sb->pan = 0;
            sb->playing = false;
            sb->looping = false;
            sb->paused = false;
            sb->playPos = 0;
            sb->feedPos = 0;
            sb->alSource = source;
            for (int i = 0; i < AUDIO_ENGINE_STREAM_QUEUE_BUFFERS; i++) {
                sb->alBuffers[i] = buffers[i];
                sb->queuedChunkBytes[i] = 0;
            }
            sb->queueHead = 0;
            sb->queueTail = 0;
            sb->queuedCount = 0;

            sb->spatialized = false;
            sb->eaxSendEnabled = false;
            sb->posX = 0.0f;
            sb->posY = 0.0f;
            sb->posZ = 0.0f;
            sb->directGain = 1.0f;
            sb->directGainHF = 1.0f;
            sb->sendGain = 1.0f;
            sb->sendGainHF = 1.0f;

            applyBufferRoutingAndSpatialState(sb);
            return index;
        }
    }

    return -1;
}

bool audioEngineSoundBufferRelease(int soundBufferIndex)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    sb->playing = false;
    unqueueAllBuffersLocked(sb);

    if (sb->alSource != 0) {
        if (gEfxSupported) {
            alSourcei(sb->alSource, AL_DIRECT_FILTER, AL_FILTER_NULL);
            alSource3i(sb->alSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        }
        alDeleteSources(1, &sb->alSource);
        sb->alSource = 0;
    }

    alDeleteBuffers(AUDIO_ENGINE_STREAM_QUEUE_BUFFERS, sb->alBuffers);
    memset(sb->alBuffers, 0, sizeof(sb->alBuffers));

    if (gEfxSupported) {
        if (sb->directFilter != 0) {
            alDeleteFilters(1, &sb->directFilter);
            sb->directFilter = 0;
        }
        if (sb->auxSendFilter != 0) {
            alDeleteFilters(1, &sb->auxSendFilter);
            sb->auxSendFilter = 0;
        }
    }

    if (sb->data != nullptr) {
        free(sb->data);
        sb->data = nullptr;
    }

    sb->active = false;
    return true;
}

bool audioEngineSoundBufferSetVolume(int soundBufferIndex, int volume)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    sb->volume = std::clamp(volume, 0, 128);
    float normalizedGain = clampFloat((float)sb->volume / 128.0f, 0.0f, 1.0f);
    alSourcef(sb->alSource, AL_GAIN, normalizedGain);

    return true;
}

bool audioEngineSoundBufferGetVolume(int soundBufferIndex, int* volumePtr)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex) || volumePtr == nullptr) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    *volumePtr = sb->volume;
    return true;
}

bool audioEngineSoundBufferSetPan(int soundBufferIndex, int pan)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    sb->pan = pan;
    applyBufferRoutingAndSpatialState(sb);
    return true;
}

bool audioEngineSoundBufferPlay(int soundBufferIndex, unsigned int flags)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    sb->looping = (flags & AUDIO_ENGINE_SOUND_BUFFER_PLAY_LOOPING) != 0;
    sb->paused = false;
    sb->playing = true;

    applyBufferRoutingAndSpatialState(sb);
    pumpSoundBufferLocked(sb);

    return true;
}

bool audioEngineSoundBufferStop(int soundBufferIndex)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    ALint byteOffset = 0;
    alGetSourcei(sb->alSource, AL_BYTE_OFFSET, &byteOffset);
    if (sb->bitsPerSample == 8) {
        byteOffset /= 2;
    }

    unsigned int currentReadPos = sb->playPos + (unsigned int)std::max(0, byteOffset);
    if (sb->size > 0) {
        currentReadPos %= sb->size;
    }

    sb->playing = false;
    unqueueAllBuffersLocked(sb);
    sb->playPos = currentReadPos;
    sb->feedPos = currentReadPos;

    return true;
}

bool audioEngineSoundBufferGetCurrentPosition(int soundBufferIndex, unsigned int* readPosPtr, unsigned int* writePosPtr)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active || sb->size == 0) {
        return false;
    }

    if (sb->playing) {
        pumpSoundBufferLocked(sb);
    }

    ALint byteOffset = 0;
    if (sb->playing && sb->queuedCount > 0) {
        alGetSourcei(sb->alSource, AL_BYTE_OFFSET, &byteOffset);
        if (sb->bitsPerSample == 8) {
            byteOffset /= 2;
        }
    }

    unsigned int currentRead = (sb->playPos + (unsigned int)std::max(0, byteOffset)) % sb->size;

    if (readPosPtr != nullptr) {
        *readPosPtr = currentRead;
    }

    if (writePosPtr != nullptr) {
        unsigned int writePos = currentRead;
        if (sb->playing) {
            writePos = (currentRead + (unsigned int)(sb->rate / 150)) % sb->size;
        }
        *writePosPtr = writePos;
    }

    return true;
}

bool audioEngineSoundBufferSetCurrentPosition(int soundBufferIndex, unsigned int pos)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active || sb->size == 0) {
        return false;
    }

    bool wasPlaying = sb->playing;
    unqueueAllBuffersLocked(sb);

    sb->playPos = pos % sb->size;
    sb->feedPos = sb->playPos;

    if (wasPlaying) {
        sb->playing = true;
        pumpSoundBufferLocked(sb);
    }

    return true;
}

bool audioEngineSoundBufferLock(int soundBufferIndex, unsigned int writePos, unsigned int writeBytes, void** audioPtr1, unsigned int* audioBytes1, void** audioPtr2, unsigned int* audioBytes2, unsigned int flags)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active || audioBytes1 == nullptr) {
        return false;
    }

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_LOCK_FROM_WRITE_POS) != 0) {
        if (!audioEngineSoundBufferGetCurrentPosition(soundBufferIndex, nullptr, &writePos)) {
            return false;
        }
    }

    if ((flags & AUDIO_ENGINE_SOUND_BUFFER_LOCK_ENTIRE_BUFFER) != 0) {
        writeBytes = sb->size;
    }

    if (writePos + writeBytes <= sb->size) {
        *(unsigned char**)audioPtr1 = sb->data + writePos;
        *audioBytes1 = writeBytes;

        if (audioPtr2 != nullptr) {
            *audioPtr2 = nullptr;
        }

        if (audioBytes2 != nullptr) {
            *audioBytes2 = 0;
        }
    } else {
        *(unsigned char**)audioPtr1 = sb->data + writePos;
        *audioBytes1 = sb->size - writePos;

        if (audioPtr2 != nullptr) {
            *(unsigned char**)audioPtr2 = sb->data;
        }

        if (audioBytes2 != nullptr) {
            *audioBytes2 = writeBytes - (sb->size - writePos);
        }
    }

    return true;
}

bool audioEngineSoundBufferUnlock(int soundBufferIndex, void* audioPtr1, unsigned int audioBytes1, void* audioPtr2, unsigned int audioBytes2)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    return true;
}

bool audioEngineSoundBufferGetStatus(int soundBufferIndex, unsigned int* statusPtr)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex) || statusPtr == nullptr) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    if (sb->playing) {
        pumpSoundBufferLocked(sb);
    }

    *statusPtr = 0;
    if (sb->playing) {
        *statusPtr |= AUDIO_ENGINE_SOUND_BUFFER_STATUS_PLAYING;
        if (sb->looping) {
            *statusPtr |= AUDIO_ENGINE_SOUND_BUFFER_STATUS_LOOPING;
        }
    }

    return true;
}

void audioEngineSetListenerPosition(float x, float y, float z)
{
    if (!audioEngineIsInitialized()) {
        return;
    }

    alListener3f(AL_POSITION, x, y, z);
}

bool audioEngineSoundBufferSet3DPosition(int soundBufferIndex, bool spatialized, float x, float y, float z)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    sb->spatialized = spatialized;
    sb->posX = x;
    sb->posY = y;
    sb->posZ = z;
    applyBufferRoutingAndSpatialState(sb);

    return true;
}

bool audioEngineSoundBufferSetOcclusion(int soundBufferIndex, float directGain, float directGainHF, float sendGain, float sendGainHF)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    sb->directGain = clampFloat(directGain, 0.0f, 1.0f);
    sb->directGainHF = clampFloat(directGainHF, 0.0f, 1.0f);
    sb->sendGain = clampFloat(sendGain, 0.0f, 1.0f);
    sb->sendGainHF = clampFloat(sendGainHF, 0.0f, 1.0f);
    applyBufferRoutingAndSpatialState(sb);

    return true;
}

bool audioEngineSoundBufferSetEaxSend(int soundBufferIndex, bool enableReverbSend)
{
    if (!audioEngineIsInitialized() || !soundBufferIsValid(soundBufferIndex)) {
        return false;
    }

    AudioEngineSoundBuffer* sb = &(gAudioEngineSoundBuffers[soundBufferIndex]);
    std::lock_guard<std::recursive_mutex> lock(sb->mutex);

    if (!sb->active) {
        return false;
    }

    sb->eaxSendEnabled = enableReverbSend;
    applyBufferRoutingAndSpatialState(sb);

    return true;
}

void audioEngineSetEaxEnvironment(int environmentPresetId, float wetGainScale, float roomSizeScale)
{
    if (environmentPresetId < 0 || environmentPresetId >= AUDIO_EAX_ENVIRONMENT_COUNT) {
        environmentPresetId = AUDIO_EAX_ENVIRONMENT_GENERIC;
    }

    std::lock_guard<std::mutex> eaxLock(gEaxMutex);
    gCurrentEaxPresetId = environmentPresetId;

    EFXEAXREVERBPROPERTIES scaled = kEax2Presets[environmentPresetId];
    float sizeFactor = clampFloat(roomSizeScale, 0.45f, 1.85f);
    scaled.flDecayTime = clampFloat(scaled.flDecayTime * sizeFactor, AL_EAXREVERB_MIN_DECAY_TIME, AL_EAXREVERB_MAX_DECAY_TIME);
    scaled.flReflectionsDelay = clampFloat(scaled.flReflectionsDelay * sizeFactor, AL_EAXREVERB_MIN_REFLECTIONS_DELAY, AL_EAXREVERB_MAX_REFLECTIONS_DELAY);
    scaled.flLateReverbDelay = clampFloat(scaled.flLateReverbDelay * sizeFactor, AL_EAXREVERB_MIN_LATE_REVERB_DELAY, AL_EAXREVERB_MAX_LATE_REVERB_DELAY);

    gTargetEaxProps = scaled;
    gTargetWetGain = gEaxEnabled ? clampFloat(wetGainScale, 0.0f, 1.0f) : 0.0f;
    gEaxDirty = true;
}

int audioEngineGetEaxEnvironment()
{
    std::lock_guard<std::mutex> eaxLock(gEaxMutex);
    return gCurrentEaxPresetId;
}

const char* audioEngineGetEaxEnvironmentName(int environmentPresetId)
{
    if (environmentPresetId < 0 || environmentPresetId >= AUDIO_EAX_ENVIRONMENT_COUNT) {
        return "Unknown";
    }
    return kEax2PresetNames[environmentPresetId];
}

} // namespace fallout
