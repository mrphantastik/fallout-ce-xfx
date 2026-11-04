#ifndef FALLOUT_GAME_WEATHER_LIGHT_H_
#define FALLOUT_GAME_WEATHER_LIGHT_H_

#include <stdint.h>

#include "game/object_types.h"
#include "plib/gnw/rect.h"

namespace fallout {

typedef enum WeatherPreset {
    WEATHER_PRESET_OFF = 0,
    WEATHER_PRESET_AUTO = 1,
    WEATHER_PRESET_FORCE_RAIN = 2,
    WEATHER_PRESET_FORCE_ASH = 3,
    WEATHER_PRESET_FORCE_SANDSTORM = 4,
} WeatherPreset;

typedef enum DynamicLightEffectType {
    DYN_LIGHT_MUZZLE_GUN = 0,
    DYN_LIGHT_MUZZLE_LASER = 1,
    DYN_LIGHT_MUZZLE_PLASMA = 2,
    DYN_LIGHT_MUZZLE_PULSE = 3,
    DYN_LIGHT_MUZZLE_FLAME = 4,
    DYN_LIGHT_EXPLOSION = 5,
    DYN_LIGHT_ELECTRICAL_ARC = 6,
    DYN_LIGHT_PLASMA_BLAST = 7,
} DynamicLightEffectType;

void weather_light_init();
void weather_light_reset();
void weather_light_exit();

void weather_light_on_map_changed();

void weather_light_trigger_effect(Object* sourceObj, int tile, int elevation, DynamicLightEffectType effectType);
void weather_light_trigger_from_anim(Object* owner, int anim);
void weather_light_trigger_from_sfx(const char* sfxName, Object* emitter);

// Composites the 32-bit Dynamic Colored Lightmap and Screen-Space Weather & Particle Engine
// onto a 32-bit RGB888 frame buffer strictly over unoccluded pixels of display_win.
void weather_light_composite_frame(const uint32_t* srcPixels, uint32_t* dstPixels, int width, int height, int pitchBytes);

} // namespace fallout

#endif /* FALLOUT_GAME_WEATHER_LIGHT_H_ */
