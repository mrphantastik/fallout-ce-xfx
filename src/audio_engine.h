#ifndef FALLOUT_AUDIO_ENGINE_H_
#define FALLOUT_AUDIO_ENGINE_H_

namespace fallout {

#define AUDIO_ENGINE_SOUND_BUFFER_LOCK_FROM_WRITE_POS 0x00000001
#define AUDIO_ENGINE_SOUND_BUFFER_LOCK_ENTIRE_BUFFER 0x00000002

#define AUDIO_ENGINE_SOUND_BUFFER_PLAY_LOOPING 0x00000001

#define AUDIO_ENGINE_SOUND_BUFFER_STATUS_PLAYING 0x00000001
#define AUDIO_ENGINE_SOUND_BUFFER_STATUS_LOOPING 0x00000004

typedef enum AudioEngineSurroundMode {
    AUDIO_SURROUND_MODE_STEREO = 0,
    AUDIO_SURROUND_MODE_AUTO = 1,
    AUDIO_SURROUND_MODE_HRTF = 2,
} AudioEngineSurroundMode;

typedef enum AudioEngineEaxEnvironment {
    AUDIO_EAX_ENVIRONMENT_GENERIC = 0,
    AUDIO_EAX_ENVIRONMENT_PADDEDCELL = 1,
    AUDIO_EAX_ENVIRONMENT_ROOM = 2,
    AUDIO_EAX_ENVIRONMENT_BATHROOM = 3,
    AUDIO_EAX_ENVIRONMENT_LIVINGROOM = 4,
    AUDIO_EAX_ENVIRONMENT_STONEROOM = 5,
    AUDIO_EAX_ENVIRONMENT_AUDITORIUM = 6,
    AUDIO_EAX_ENVIRONMENT_CONCERTHALL = 7,
    AUDIO_EAX_ENVIRONMENT_CAVE = 8,
    AUDIO_EAX_ENVIRONMENT_ARENA = 9,
    AUDIO_EAX_ENVIRONMENT_HANGAR = 10,
    AUDIO_EAX_ENVIRONMENT_CARPETEDHALLWAY = 11,
    AUDIO_EAX_ENVIRONMENT_HALLWAY = 12,
    AUDIO_EAX_ENVIRONMENT_STONECORRIDOR = 13,
    AUDIO_EAX_ENVIRONMENT_ALLEY = 14,
    AUDIO_EAX_ENVIRONMENT_FOREST = 15,
    AUDIO_EAX_ENVIRONMENT_CITY = 16,
    AUDIO_EAX_ENVIRONMENT_MOUNTAINS = 17,
    AUDIO_EAX_ENVIRONMENT_QUARRY = 18,
    AUDIO_EAX_ENVIRONMENT_PLAIN = 19,
    AUDIO_EAX_ENVIRONMENT_PARKINGLOT = 20,
    AUDIO_EAX_ENVIRONMENT_SEWERPIPE = 21,
    AUDIO_EAX_ENVIRONMENT_UNDERWATER = 22,
    AUDIO_EAX_ENVIRONMENT_DRUGGED = 23,
    AUDIO_EAX_ENVIRONMENT_DIZZY = 24,
    AUDIO_EAX_ENVIRONMENT_PSYCHOTIC = 25,
    AUDIO_EAX_ENVIRONMENT_COUNT = 26,
} AudioEngineEaxEnvironment;

void audioEngineSetConfig(int surroundMode, bool eaxEnabled);
int audioEngineGetSurroundMode();
bool audioEngineIsEaxEnabled();

bool audioEngineInit();
void audioEngineExit();
void audioEnginePause();
void audioEngineResume();
int audioEngineCreateSoundBuffer(unsigned int size, int bitsPerSample, int channels, int rate);
bool audioEngineSoundBufferRelease(int soundBufferIndex);
bool audioEngineSoundBufferSetVolume(int soundBufferIndex, int volume);
bool audioEngineSoundBufferGetVolume(int soundBufferIndex, int* volumePtr);
bool audioEngineSoundBufferSetPan(int soundBufferIndex, int pan);
bool audioEngineSoundBufferPlay(int soundBufferIndex, unsigned int flags);
bool audioEngineSoundBufferStop(int soundBufferIndex);
bool audioEngineSoundBufferGetCurrentPosition(int soundBufferIndex, unsigned int* readPosPtr, unsigned int* writePosPtr);
bool audioEngineSoundBufferSetCurrentPosition(int soundBufferIndex, unsigned int pos);
bool audioEngineSoundBufferLock(int soundBufferIndex, unsigned int writePos, unsigned int writeBytes, void** audioPtr1, unsigned int* audioBytes1, void** audioPtr2, unsigned int* audioBytes2, unsigned int flags);
bool audioEngineSoundBufferUnlock(int soundBufferIndex, void* audioPtr1, unsigned int audioBytes1, void* audioPtr2, unsigned int audioBytes2);
bool audioEngineSoundBufferGetStatus(int soundBufferIndex, unsigned int* status);

// Surround Sound (3D positional audio) and OpenAL Soft ALC_EXT_EFX (EAX 2.0) API
void audioEngineSetListenerPosition(float x, float y, float z);
bool audioEngineSoundBufferSet3DPosition(int soundBufferIndex, bool spatialized, float x, float y, float z);
bool audioEngineSoundBufferSetOcclusion(int soundBufferIndex, float directGain, float directGainHF, float sendGain, float sendGainHF);
bool audioEngineSoundBufferSetEaxSend(int soundBufferIndex, bool enableReverbSend);
void audioEngineSetEaxEnvironment(int environmentPresetId, float wetGainScale, float roomSizeScale);
int audioEngineGetEaxEnvironment();
const char* audioEngineGetEaxEnvironmentName(int environmentPresetId);

} // namespace fallout

#endif /* FALLOUT_AUDIO_ENGINE_H_ */
