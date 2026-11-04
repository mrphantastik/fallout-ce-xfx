#include "game/weather_light.h"

#include <SDL.h>
#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "game/anim.h"
#include "game/art.h"
#include "game/gconfig.h"
#include "game/inventry.h"
#include "game/light.h"
#include "game/map.h"
#include "game/object.h"
#include "game/proto.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "plib/gnw/gnw.h"

namespace fallout {

namespace {

typedef enum ActiveWeatherMode {
    ACTIVE_WEATHER_NONE = 0,
    ACTIVE_WEATHER_RAIN_STORM = 1,
    ACTIVE_WEATHER_FALLOUT_ASH = 2,
    ACTIVE_WEATHER_SANDSTORM = 3,
    ACTIVE_WEATHER_UNDERGROUND_DUST = 4,
} ActiveWeatherMode;

typedef enum ParticleKind {
    PARTICLE_RAIN_STREAK = 0,
    PARTICLE_RAIN_SPLASH = 1,
    PARTICLE_ASH_FLAKE = 2,
    PARTICLE_RAD_EMBER = 3,
    PARTICLE_SAND_GRAIN = 4,
    PARTICLE_DUST_MOTE = 5,
    PARTICLE_FIRE_SPARK = 6,
    PARTICLE_RAD_MOTE = 7,
    PARTICLE_COMBAT_SPARK = 8,
} ParticleKind;

struct WeatherParticle {
    bool active;
    ParticleKind kind;
    float x;
    float y;
    float vx;
    float vy;
    float phase;
    float size;
    float age;
    float maxAge;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t alpha;
    uint8_t layer;
};

struct TransientDynamicLight {
    bool active;
    int tile;
    int elevation;
    float offsetX;
    float offsetY;
    float radiusPx;
    float intensity;
    float ageSec;
    float durationSec;
    float r;
    float g;
    float b;
    bool flicker;
};

struct EvaluatedLight {
    float screenX;
    float screenY;
    float radiusPx;
    float intensity;
    float r;
    float g;
    float b;
    bool castsWallShadows;
    bool indoorLight;
};

constexpr int kMaxWeatherParticles = 1400;
constexpr int kMaxEffectParticles = 512;
constexpr int kMaxTransientLights = 48;
constexpr int kMaxEvaluatedLights = 64;
constexpr int kGridCellSize = 8;
constexpr int kMaxGridWidth = 256;
constexpr int kMaxGridHeight = 160;

// Cell roof state for masking exterior weather from interior rooms
enum CellRoofState : uint8_t {
    CELL_EXTERIOR_OPEN = 0,
    CELL_ROOF_VISIBLE = 1,
    CELL_INDOOR_REVEALED = 2,
};

struct LightCellRGB {
    float r;
    float g;
    float b;
};

static bool gWeatherLightInitialized = false;
static int gWeatherConfigMode = WEATHER_PRESET_AUTO;
static int gDynamicLightingConfig = 1;

static int gLastMapIndex = -999;
static int gLastElevation = -999;
static int gLastCenterTile = -1;
static int gLastCenterScreenX = 0;
static int gLastCenterScreenY = 0;
static uint32_t gLastTickMs = 0;
static float gAccumTimeSec = 0.0f;

static ActiveWeatherMode gActiveWeather = ACTIVE_WEATHER_NONE;
static float gWindPhase = 0.0f;
static float gLightningFlashIntensity = 0.0f;
static float gNextLightningTimerSec = 11.0f;

static WeatherParticle gWeatherParticles[kMaxWeatherParticles];
static WeatherParticle gEffectParticles[kMaxEffectParticles];
static TransientDynamicLight gTransientLights[kMaxTransientLights];

static std::vector<uint8_t> gViewportMask;
static uint8_t gCellRoofGrid[kMaxGridHeight][kMaxGridWidth];
static uint8_t gWallBlockerGrid[kMaxGridHeight][kMaxGridWidth];
static LightCellRGB gLightmapGrid[kMaxGridHeight][kMaxGridWidth];

static uint32_t gRngState = 0x9E3779B9u;

static inline uint32_t fastRandU32()
{
    uint32_t x = gRngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gRngState = x;
    return x;
}

static inline float fastRandFloat(float minVal, float maxVal)
{
    float t = (float)(fastRandU32() & 0xFFFFFFu) / (float)0x1000000u;
    return minVal + t * (maxVal - minVal);
}

static inline bool containsIgnoreCase(const char* haystack, const char* needle)
{
    if (haystack == NULL || needle == NULL || *needle == '\0') {
        return false;
    }

    size_t needleLen = strlen(needle);
    for (const char* p = haystack; *p != '\0'; p++) {
        size_t i = 0;
        while (i < needleLen && p[i] != '\0' && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needleLen) {
            return true;
        }
    }
    return false;
}

static bool isUndergroundMapAndElevation(int mapIdx, int elevation)
{
    switch (mapIdx) {
    case MAP_VAULT13:
    case MAP_VAULTNEC:
    case MAP_CAVES:
    case MAP_BROHD12:
    case MAP_BROHD34:
    case MAP_MBSTRG12:
    case MAP_MBVATS12:
    case MAP_MSTRLR12:
    case MAP_MSTRLR34:
    case MAP_GLOW1:
    case MAP_GLOW2:
    case MAP_CHILDRN2:
        return true;
    case MAP_VAULTENT:
    case MAP_VAULTBUR:
    case MAP_V13ENT:
    case MAP_BROHDENT:
    case MAP_MBENT:
        return elevation > 0;
    default:
        break;
    }

    if (elevation > 0 && mapIdx != MAP_HUBHEIGT && mapIdx != MAP_JUNKENT && mapIdx != MAP_JUNKCSNO && mapIdx != MAP_LAADYTUM) {
        return true;
    }

    return false;
}

static ActiveWeatherMode determineWeatherForCurrentMap()
{
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_WEATHER_EFFECTS_KEY, &gWeatherConfigMode);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_DYNAMIC_LIGHTING_KEY, &gDynamicLightingConfig);

    if (gWeatherConfigMode <= WEATHER_PRESET_OFF) {
        return ACTIVE_WEATHER_NONE;
    }

    int mapIdx = map_data.field_34;
    int elev = map_elevation;

    if (isUndergroundMapAndElevation(mapIdx, elev)) {
        return ACTIVE_WEATHER_UNDERGROUND_DUST;
    }

    if (gWeatherConfigMode == WEATHER_PRESET_FORCE_RAIN) {
        return ACTIVE_WEATHER_RAIN_STORM;
    }
    if (gWeatherConfigMode == WEATHER_PRESET_FORCE_ASH) {
        return ACTIVE_WEATHER_FALLOUT_ASH;
    }
    if (gWeatherConfigMode == WEATHER_PRESET_FORCE_SANDSTORM) {
        return ACTIVE_WEATHER_SANDSTORM;
    }

    // WEATHER_PRESET_AUTO: select authentic regional post-apocalyptic weather
    switch (mapIdx) {
    // The Glow, Necropolis, Boneyard, Ruins, Destroyed Bases -> Radioactive Fallout Ash & Embers
    case MAP_GLOWENT:
    case MAP_HALLDED:
    case MAP_HOTEL:
    case MAP_WATRSHD:
    case MAP_CITY1:
    case MAP_CHILDEAD:
    case MAP_MBDEAD:
    case MAP_BRODEAD:
    case MAP_LAADYTUM:
    case MAP_LAFOLLWR:
    case MAP_LABLADES:
    case MAP_LARIPPER:
    case MAP_LAGUNRUN:
        return ACTIVE_WEATHER_FALLOUT_ASH;

    // Coast, Mountains, The Hub, Military Base Exterior, Cathedral Exterior, V13 Exterior -> Wasteland Acid Rain & Storm
    case MAP_COAST1:
    case MAP_COAST2:
    case MAP_MOUNTN1:
    case MAP_MOUNTN2:
    case MAP_MNTCRVN1:
    case MAP_MNTCRVN2:
    case MAP_MNTCRVN3:
    case MAP_MNTCRVN4:
    case MAP_HUBENT:
    case MAP_DETHCLAW:
    case MAP_HUBDWNTN:
    case MAP_HUBHEIGT:
    case MAP_HUBOLDTN:
    case MAP_HUBWATER:
    case MAP_HUBMIS1:
    case MAP_MBENT:
    case MAP_CHILDRN1:
    case MAP_V13ENT:
    case MAP_BROHDENT:
        return ACTIVE_WEATHER_RAIN_STORM;

    // Desert, Shady Sands, Junktown, Raiders, Caravans, Special Encounters -> Wasteland Sandstorm
    case MAP_DESERT1:
    case MAP_DESERT2:
    case MAP_DESERT3:
    case MAP_JUNKENT:
    case MAP_JUNKCSNO:
    case MAP_JUNKKILL:
    case MAP_SHADYE:
    case MAP_SHADYW:
    case MAP_RAIDERS:
    case MAP_VIPERS:
    case MAP_COLATRUK:
    case MAP_FSAUSER:
    case MAP_FOOT:
    case MAP_TARDIS:
    case MAP_TALKCOW:
    case MAP_USEDCAR:
    case MAP_DESCRVN1:
    case MAP_DESCRVN2:
    case MAP_DESCRVN3:
    case MAP_DESCRVN4:
    default:
        return ACTIVE_WEATHER_SANDSTORM;
    }
}

static void spawnWeatherParticle(WeatherParticle* p, ActiveWeatherMode mode, int vpWidth, int vpHeight, bool randomY)
{
    p->active = true;
    p->age = 0.0f;
    p->phase = fastRandFloat(0.0f, 6.2831853f);
    p->layer = (uint8_t)(fastRandU32() % 3);
    float depthScale = 0.65f + 0.22f * (float)p->layer;

    switch (mode) {
    case ACTIVE_WEATHER_RAIN_STORM:
        p->kind = PARTICLE_RAIN_STREAK;
        p->x = fastRandFloat(-80.0f, (float)vpWidth + 140.0f);
        p->y = randomY ? fastRandFloat(-40.0f, (float)vpHeight) : fastRandFloat(-60.0f, -4.0f);
        p->vx = fastRandFloat(-195.0f, -145.0f) * depthScale;
        p->vy = fastRandFloat(640.0f, 860.0f) * depthScale;
        p->size = fastRandFloat(11.0f, 20.0f) * depthScale;
        p->maxAge = 4.0f;
        p->r = (uint8_t)fastRandFloat(155.0f, 190.0f);
        p->g = (uint8_t)fastRandFloat(185.0f, 215.0f);
        p->b = (uint8_t)fastRandFloat(210.0f, 245.0f);
        p->alpha = (uint8_t)(fastRandFloat(85.0f, 155.0f) * depthScale);
        break;

    case ACTIVE_WEATHER_FALLOUT_ASH:
        if ((fastRandU32() % 9) == 0) {
            // Glowing radioactive or fire ember drifting in the ash
            p->kind = PARTICLE_RAD_EMBER;
            bool greenRad = (map_data.field_34 == MAP_GLOWENT || map_data.field_34 == MAP_HALLDED || map_data.field_34 == MAP_WATRSHD || (fastRandU32() % 3) == 0);
            p->x = fastRandFloat(-20.0f, (float)vpWidth + 20.0f);
            p->y = randomY ? fastRandFloat(0.0f, (float)vpHeight) : fastRandFloat(-20.0f, (float)vpHeight * 0.35f);
            p->vx = fastRandFloat(-28.0f, 38.0f);
            p->vy = fastRandFloat(18.0f, 48.0f);
            p->size = fastRandFloat(1.5f, 2.8f);
            p->maxAge = fastRandFloat(2.5f, 5.5f);
            if (greenRad) {
                p->r = 75;
                p->g = 255;
                p->b = 95;
            } else {
                p->r = 255;
                p->g = (uint8_t)fastRandFloat(120.0f, 185.0f);
                p->b = 45;
            }
            p->alpha = (uint8_t)fastRandFloat(145.0f, 225.0f);
        } else {
            p->kind = PARTICLE_ASH_FLAKE;
            p->x = fastRandFloat(-40.0f, (float)vpWidth + 40.0f);
            p->y = randomY ? fastRandFloat(-20.0f, (float)vpHeight) : fastRandFloat(-30.0f, -2.0f);
            p->vx = fastRandFloat(-22.0f, 32.0f) * depthScale;
            p->vy = fastRandFloat(36.0f, 82.0f) * depthScale;
            p->size = (p->layer == 2) ? 2.0f : 1.0f;
            p->maxAge = 12.0f;
            uint8_t shade = (uint8_t)fastRandFloat(135.0f, 205.0f);
            p->r = shade;
            p->g = (uint8_t)(shade > 8 ? shade - 6 : shade);
            p->b = (uint8_t)(shade > 16 ? shade - 14 : shade);
            p->alpha = (uint8_t)(fastRandFloat(95.0f, 175.0f) * depthScale);
        }
        break;

    case ACTIVE_WEATHER_SANDSTORM:
        p->kind = PARTICLE_SAND_GRAIN;
        p->x = randomY ? fastRandFloat(-60.0f, (float)vpWidth + 60.0f) : fastRandFloat(-90.0f, -4.0f);
        p->y = fastRandFloat(-30.0f, (float)vpHeight + 20.0f);
        p->vx = fastRandFloat(340.0f, 580.0f) * depthScale;
        p->vy = fastRandFloat(45.0f, 135.0f) * depthScale;
        p->size = fastRandFloat(3.0f, 8.0f) * depthScale;
        p->maxAge = 4.0f;
        p->r = (uint8_t)fastRandFloat(205.0f, 240.0f);
        p->g = (uint8_t)fastRandFloat(165.0f, 198.0f);
        p->b = (uint8_t)fastRandFloat(105.0f, 140.0f);
        p->alpha = (uint8_t)(fastRandFloat(80.0f, 155.0f) * depthScale);
        break;

    case ACTIVE_WEATHER_UNDERGROUND_DUST:
    default: {
        bool isGlowOrVats = (map_data.field_34 == MAP_GLOW1 || map_data.field_34 == MAP_GLOW2 || map_data.field_34 == MAP_MBVATS12);
        p->kind = (isGlowOrVats && (fastRandU32() % 3) == 0) ? PARTICLE_RAD_MOTE : PARTICLE_DUST_MOTE;
        p->x = fastRandFloat(0.0f, (float)vpWidth);
        p->y = fastRandFloat(0.0f, (float)vpHeight);
        p->vx = fastRandFloat(-12.0f, 12.0f);
        p->vy = fastRandFloat(-14.0f, 8.0f);
        p->size = (p->kind == PARTICLE_RAD_MOTE) ? 2.0f : 1.0f;
        p->maxAge = fastRandFloat(3.5f, 8.0f);
        if (p->kind == PARTICLE_RAD_MOTE) {
            p->r = 70;
            p->g = 250;
            p->b = 95;
            p->alpha = (uint8_t)fastRandFloat(110.0f, 185.0f);
        } else {
            p->r = 185;
            p->g = 180;
            p->b = 165;
            p->alpha = (uint8_t)fastRandFloat(55.0f, 105.0f);
        }
        break;
    }
    }
}

static void spawnEffectParticle(ParticleKind kind, float x, float y, float vx, float vy, float maxAge, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha, float size)
{
    for (int i = 0; i < kMaxEffectParticles; i++) {
        if (!gEffectParticles[i].active) {
            WeatherParticle* p = &gEffectParticles[i];
            p->active = true;
            p->kind = kind;
            p->x = x;
            p->y = y;
            p->vx = vx;
            p->vy = vy;
            p->phase = fastRandFloat(0.0f, 6.2831853f);
            p->size = size;
            p->age = 0.0f;
            p->maxAge = maxAge;
            p->r = r;
            p->g = g;
            p->b = b;
            p->alpha = alpha;
            p->layer = 2;
            return;
        }
    }
}

static int targetWeatherParticleCount(ActiveWeatherMode mode)
{
    switch (mode) {
    case ACTIVE_WEATHER_RAIN_STORM:
        return 520;
    case ACTIVE_WEATHER_FALLOUT_ASH:
        return 360;
    case ACTIVE_WEATHER_SANDSTORM:
        return 620;
    case ACTIVE_WEATHER_UNDERGROUND_DUST:
        return 140;
    case ACTIVE_WEATHER_NONE:
    default:
        return 0;
    }
}

static void rebuildWeatherParticles(int vpWidth, int vpHeight)
{
    memset(gWeatherParticles, 0, sizeof(gWeatherParticles));
    int count = targetWeatherParticleCount(gActiveWeather);
    if (count > kMaxWeatherParticles) {
        count = kMaxWeatherParticles;
    }

    for (int i = 0; i < count; i++) {
        spawnWeatherParticle(&gWeatherParticles[i], gActiveWeather, vpWidth, vpHeight, true);
    }
}

// Builds the 8x8 roof state grid and wall blocker grid for the current viewport.
static void buildRoofAndWallGrids(int gridW, int gridH)
{
    bool roofVisible = (tile_roof_visible() != 0);
    TileData* elevSquare = (elevationIsValid(map_elevation)) ? square[map_elevation] : NULL;

    for (int gy = 0; gy < gridH; gy++) {
        int py = gy * kGridCellSize + (kGridCellSize / 2);
        for (int gx = 0; gx < gridW; gx++) {
            int px = gx * kGridCellSize + (kGridCellSize / 2);
            uint8_t state = CELL_EXTERIOR_OPEN;

            if (elevSquare != NULL) {
                int rx = 0;
                int ry = 0;
                square_xy_roof(px, py, map_elevation, &rx, &ry);
                if (rx >= 0 && rx < SQUARE_GRID_WIDTH && ry >= 0 && ry < SQUARE_GRID_HEIGHT) {
                    int val = elevSquare->field_0[SQUARE_GRID_WIDTH * ry + rx];
                    int upper = (val >> 16) & 0xFFFF;
                    int roofId = upper & 0xFFF;
                    int roofFlag = (upper >> 12) & 0xF;
                    if (roofId > 1) {
                        if (!roofVisible || (roofFlag & 0x01) != 0) {
                            state = CELL_INDOOR_REVEALED;
                        } else {
                            state = CELL_ROOF_VISIBLE;
                        }
                    }
                }
            }

            gCellRoofGrid[gy][gx] = state;
            gWallBlockerGrid[gy][gx] = 0;
        }
    }

    // Also mark around obj_dude if obj_dude is under a roof (in case of egg cut-away)
    if (obj_dude != NULL && elevSquare != NULL && obj_dude->elevation == map_elevation) {
        int dudeScreenX = 0;
        int dudeScreenY = 0;
        if (tile_coord(obj_dude->tile, &dudeScreenX, &dudeScreenY, map_elevation) == 0) {
            int rx = 0;
            int ry = 0;
            square_xy_roof(dudeScreenX + 16, dudeScreenY + 8, map_elevation, &rx, &ry);
            if (rx >= 0 && rx < SQUARE_GRID_WIDTH && ry >= 0 && ry < SQUARE_GRID_HEIGHT) {
                int upper = (elevSquare->field_0[SQUARE_GRID_WIDTH * ry + rx] >> 16) & 0xFFFF;
                if ((upper & 0xFFF) > 1) {
                    // Player is standing under a roof: mark the egg cut-away oval as CELL_INDOOR_REVEALED
                    int cx = (dudeScreenX + 16) / kGridCellSize;
                    int cy = (dudeScreenY - 24) / kGridCellSize;
                    for (int dy = -8; dy <= 8; dy++) {
                        int yy = cy + dy;
                        if (yy < 0 || yy >= gridH) continue;
                        for (int dx = -10; dx <= 10; dx++) {
                            int xx = cx + dx;
                            if (xx < 0 || xx >= gridW) continue;
                            if (gCellRoofGrid[yy][xx] == CELL_ROOF_VISIBLE) {
                                gCellRoofGrid[yy][xx] = CELL_INDOOR_REVEALED;
                            }
                        }
                    }
                }
            }
        }
    }
}

static inline bool isScreenPointIndoorRevealed(int x, int y, int gridW, int gridH)
{
    int gx = x / kGridCellSize;
    int gy = y / kGridCellSize;
    if (gx < 0 || gx >= gridW || gy < 0 || gy >= gridH) {
        return false;
    }
    return gCellRoofGrid[gy][gx] == CELL_INDOOR_REVEALED;
}

static void collectActiveLightsAndBlockers(EvaluatedLight* outLights, int* outLightCount, int vpWidth, int vpHeight, int gridW, int gridH, float dtSec)
{
    int count = 0;

    // 1. Scan active map objects on current elevation for wall blockers & light emitters
    for (Object* obj = obj_find_first_at(map_elevation); obj != NULL; obj = obj_find_next_at()) {
        if ((obj->flags & OBJECT_HIDDEN) != 0) {
            continue;
        }

        int sx = 0;
        int sy = 0;
        if (tile_coord(obj->tile, &sx, &sy, map_elevation) != 0) {
            continue;
        }
        sx += 16 + obj->x;
        sy += 8 + obj->y;

        if (sx < -240 || sx > vpWidth + 240 || sy < -240 || sy > vpHeight + 240) {
            continue;
        }

        int objType = FID_TYPE(obj->fid);

        // Mark sight-blocking walls and closed doors in the shadow blocker grid
        if ((objType == OBJ_TYPE_WALL || objType == OBJ_TYPE_SCENERY) && (obj->flags & OBJECT_LIGHT_THRU) == 0 && (obj->flags & OBJECT_FLAT) == 0) {
            int gx = sx / kGridCellSize;
            int gy = (sy - 12) / kGridCellSize;
            for (int dy = -1; dy <= 1; dy++) {
                int cy = gy + dy;
                if (cy < 0 || cy >= gridH) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int cx = gx + dx;
                    if (cx >= 0 && cx < gridW) {
                        gWallBlockerGrid[cy][cx] = 1;
                    }
                }
            }
        }

        if (count >= kMaxEvaluatedLights) {
            continue;
        }

        bool isEmitter = false;
        float r = 1.0f;
        float g = 0.86f;
        float b = 0.64f;
        float radiusPx = 0.0f;
        float intensity = 0.0f;
        bool isFireEmitter = false;
        bool isRadEmitter = false;

        if (obj == obj_dude) {
            // Player flashlight / ambient lantern aura
            isEmitter = true;
            int lightDist = std::max(obj->lightDistance, 4);
            radiusPx = (float)lightDist * 30.0f;
            intensity = (obj->lightIntensity > 20000) ? 0.65f : 0.35f;
            r = 1.0f;
            g = 0.88f;
            b = 0.66f;
        } else if (objType == OBJ_TYPE_MISC && (obj->flags & OBJECT_NO_SAVE) != 0) {
            // Active flying projectile or temporary effect object (plasma bolt, laser, rocket, explosion, flare)
            isEmitter = true;
            int fidIdx = obj->fid & 0xFFF;
            if ((obj->flags & OBJECT_TRANS_ENERGY) != 0 || fidIdx == 13 || fidIdx == 14) {
                // Plasma projectile
                r = 0.25f;
                g = 1.0f;
                b = 0.42f;
                radiusPx = 155.0f;
                intensity = 1.25f;
                isRadEmitter = true;
            } else if ((obj->flags & OBJECT_TRANS_RED) != 0 || fidIdx == 9 || fidIdx == 10) {
                // Laser bolt
                r = 1.0f;
                g = 0.22f;
                b = 0.20f;
                radiusPx = 145.0f;
                intensity = 1.15f;
            } else {
                // Rocket / explosive / flame projectile
                r = 1.0f;
                g = 0.65f;
                b = 0.24f;
                radiusPx = 160.0f;
                intensity = 1.20f;
                isFireEmitter = true;
            }
        } else if (obj->lightIntensity > 1024 && obj->lightDistance > 0) {
            // Native light-emitting object on the map (burn barrel, flare, computer, glowing slime, lamp)
            isEmitter = true;
            radiusPx = std::max(72.0f, (float)obj->lightDistance * 32.0f);
            intensity = std::min(1.15f, ((float)obj->lightIntensity / 65536.0f) * 0.92f);

            const char* name = object_name(obj);
            if ((obj->flags & OBJECT_TRANS_ENERGY) != 0 || containsIgnoreCase(name, "slime") || containsIgnoreCase(name, "goo") || containsIgnoreCase(name, "waste") || containsIgnoreCase(name, "radioactive") || containsIgnoreCase(name, "glowing") || containsIgnoreCase(name, "vat")) {
                r = 0.24f;
                g = 1.0f;
                b = 0.38f;
                isRadEmitter = true;
            } else if (containsIgnoreCase(name, "fire") || containsIgnoreCase(name, "barrel") || containsIgnoreCase(name, "torch") || containsIgnoreCase(name, "flare") || containsIgnoreCase(name, "brazier") || containsIgnoreCase(name, "candle")) {
                r = 1.0f;
                g = 0.56f;
                b = 0.18f;
                isFireEmitter = true;
            } else if (containsIgnoreCase(name, "computer") || containsIgnoreCase(name, "monitor") || containsIgnoreCase(name, "terminal") || containsIgnoreCase(name, "screen") || containsIgnoreCase(name, "force")) {
                r = 0.32f;
                g = 0.74f;
                b = 1.0f;
            } else if ((obj->flags & OBJECT_TRANS_RED) != 0 || containsIgnoreCase(name, "alarm") || containsIgnoreCase(name, "emergency")) {
                r = 1.0f;
                g = 0.22f;
                b = 0.20f;
            } else {
                // Default warm wasteland lamp / firelight
                r = 1.0f;
                g = 0.78f;
                b = 0.50f;
                if (objType == OBJ_TYPE_SCENERY && obj->lightDistance >= 4) {
                    isFireEmitter = true;
                }
            }
        } else if (objType == OBJ_TYPE_SCENERY || objType == OBJ_TYPE_ITEM) {
            // Check scenery/items without explicit lightIntensity that are fire barrels, slime pools, or monitors
            const char* name = object_name(obj);
            if (containsIgnoreCase(name, "burning") || containsIgnoreCase(name, "campfire") || containsIgnoreCase(name, "fire")) {
                isEmitter = true;
                r = 1.0f;
                g = 0.54f;
                b = 0.16f;
                radiusPx = 155.0f;
                intensity = 0.90f;
                isFireEmitter = true;
            } else if (containsIgnoreCase(name, "slime") || containsIgnoreCase(name, "goo") || containsIgnoreCase(name, "toxic")) {
                isEmitter = true;
                r = 0.22f;
                g = 0.98f;
                b = 0.35f;
                radiusPx = 115.0f;
                intensity = 0.72f;
                isRadEmitter = true;
            }
        } else if (objType == OBJ_TYPE_CRITTER) {
            if ((obj->data.critter.combat.results & DAM_ON_FIRE) != 0) {
                isEmitter = true;
                r = 1.0f;
                g = 0.52f;
                b = 0.15f;
                radiusPx = 150.0f;
                intensity = 1.05f;
                isFireEmitter = true;
            } else {
                const char* name = object_name(obj);
                if (containsIgnoreCase(name, "glowing")) {
                    isEmitter = true;
                    r = 0.28f;
                    g = 1.0f;
                    b = 0.42f;
                    radiusPx = 135.0f;
                    intensity = 0.85f;
                    isRadEmitter = true;
                }
            }
        }

        if (isEmitter) {
            float phase = (float)(obj->tile * 17 + (int)(uintptr_t)obj) * 0.13f;
            if (isFireEmitter) {
                float flicker = 1.0f + 0.14f * sinf(gAccumTimeSec * 8.5f + phase) + 0.09f * sinf(gAccumTimeSec * 19.3f + phase * 1.7f);
                intensity *= flicker;
                radiusPx *= (0.95f + 0.05f * flicker);

                // Emit rising fire sparks from burning objects
                if (sx >= 0 && sx < vpWidth && sy >= 0 && sy < vpHeight && fastRandFloat(0.0f, 1.0f) < dtSec * 9.5f) {
                    spawnEffectParticle(
                        PARTICLE_FIRE_SPARK,
                        (float)sx + fastRandFloat(-8.0f, 8.0f),
                        (float)sy - fastRandFloat(14.0f, 28.0f),
                        fastRandFloat(-18.0f, 18.0f),
                        fastRandFloat(-52.0f, -24.0f),
                        fastRandFloat(0.65f, 1.45f),
                        255,
                        (uint8_t)fastRandFloat(130.0f, 210.0f),
                        45,
                        230,
                        2.0f);
                }
            } else if (isRadEmitter) {
                float pulse = 1.0f + 0.16f * sinf(gAccumTimeSec * 2.8f + phase);
                intensity *= pulse;

                if (sx >= 0 && sx < vpWidth && sy >= 0 && sy < vpHeight && fastRandFloat(0.0f, 1.0f) < dtSec * 4.5f) {
                    spawnEffectParticle(
                        PARTICLE_RAD_MOTE,
                        (float)sx + fastRandFloat(-12.0f, 12.0f),
                        (float)sy - fastRandFloat(6.0f, 20.0f),
                        fastRandFloat(-10.0f, 10.0f),
                        fastRandFloat(-26.0f, -10.0f),
                        fastRandFloat(1.1f, 2.4f),
                        65,
                        255,
                        95,
                        195,
                        2.0f);
                }
            }

            EvaluatedLight* el = &outLights[count++];
            el->screenX = (float)sx;
            el->screenY = (float)(sy - 16);
            el->radiusPx = radiusPx;
            el->intensity = intensity;
            el->r = r;
            el->g = g;
            el->b = b;
            el->castsWallShadows = (obj != obj_dude);
            el->indoorLight = isScreenPointIndoorRevealed(sx, sy - 16, gridW, gridH);
        }
    }

    // 2. Update and collect transient combat/weapon/explosion dynamic lights
    for (int i = 0; i < kMaxTransientLights; i++) {
        TransientDynamicLight* tl = &gTransientLights[i];
        if (!tl->active) {
            continue;
        }

        tl->ageSec += dtSec;
        if (tl->ageSec >= tl->durationSec || tl->elevation != map_elevation) {
            tl->active = false;
            continue;
        }

        int sx = 0;
        int sy = 0;
        if (tile_coord(tl->tile, &sx, &sy, tl->elevation) != 0) {
            continue;
        }
        sx += 16 + (int)tl->offsetX;
        sy += 8 + (int)tl->offsetY;

        float normAge = tl->ageSec / tl->durationSec;
        float envelope = (1.0f - normAge) * (1.0f - normAge);
        if (tl->flicker) {
            envelope *= (0.85f + 0.15f * sinf(gAccumTimeSec * 45.0f + (float)i));
        }

        if (count < kMaxEvaluatedLights && envelope > 0.01f) {
            EvaluatedLight* el = &outLights[count++];
            el->screenX = (float)sx;
            el->screenY = (float)(sy - 20);
            el->radiusPx = tl->radiusPx * (0.75f + 0.35f * (1.0f - normAge * 0.5f));
            el->intensity = tl->intensity * envelope;
            el->r = tl->r;
            el->g = tl->g;
            el->b = tl->b;
            el->castsWallShadows = true;
            el->indoorLight = isScreenPointIndoorRevealed(sx, sy - 20, gridW, gridH);
        }
    }

    *outLightCount = count;
}

// Computes the wall shadow factor between light cell (lx, ly) and target cell (gx, gy).
static float computeGridShadowFactor(int lx, int ly, int gx, int gy, int gridW, int gridH)
{
    int dx = abs(gx - lx);
    int dy = abs(gy - ly);
    int steps = std::max(dx, dy);
    if (steps <= 2) {
        return 1.0f;
    }

    float xInc = (float)(gx - lx) / (float)steps;
    float yInc = (float)(gy - ly) / (float)steps;
    float cx = (float)lx + xInc * 1.5f;
    float cy = (float)ly + yInc * 1.5f;

    int blockersHit = 0;
    // Stop 1 step before the target cell so the front face of a wall is still illuminated
    for (int s = 2; s < steps - 1; s++) {
        int ix = (int)cx;
        int iy = (int)cy;
        if (ix >= 0 && ix < gridW && iy >= 0 && iy < gridH) {
            if (gWallBlockerGrid[iy][ix] != 0) {
                blockersHit++;
                if (blockersHit >= 2) {
                    return 0.10f;
                }
            }
        }
        cx += xInc;
        cy += yInc;
    }

    return (blockersHit == 1) ? 0.38f : 1.0f;
}

static void evaluateDynamicLightmapGrid(const EvaluatedLight* lights, int lightCount, int gridW, int gridH)
{
    // Base atmospheric tint & lightning flash on the 8x8 grid
    for (int gy = 0; gy < gridH; gy++) {
        for (int gx = 0; gx < gridW; gx++) {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;

            uint8_t roofState = gCellRoofGrid[gy][gx];
            if (roofState != CELL_INDOOR_REVEALED) {
                if (gActiveWeather == ACTIVE_WEATHER_RAIN_STORM && gLightningFlashIntensity > 0.01f) {
                    r += gLightningFlashIntensity * 0.55f;
                    g += gLightningFlashIntensity * 0.68f;
                    b += gLightningFlashIntensity * 0.95f;
                } else if (gActiveWeather == ACTIVE_WEATHER_SANDSTORM) {
                    // Procedural scrolling volumetric desert dust bands
                    float wx = (float)(gx * kGridCellSize) * 0.012f - gAccumTimeSec * 2.2f;
                    float wy = (float)(gy * kGridCellSize) * 0.018f - gAccumTimeSec * 0.5f;
                    float wave = 0.5f + 0.5f * sinf(wx + sinf(wy * 1.7f));
                    float dust = 0.04f + 0.07f * wave;
                    r += dust * 1.15f;
                    g += dust * 0.82f;
                    b += dust * 0.42f;
                } else if (gActiveWeather == ACTIVE_WEATHER_FALLOUT_ASH) {
                    r += 0.025f;
                    g += 0.032f;
                    b += 0.018f;
                }
            }

            gLightmapGrid[gy][gx].r = r;
            gLightmapGrid[gy][gx].g = g;
            gLightmapGrid[gy][gx].b = b;
        }
    }

    if (!gDynamicLightingConfig || lightCount <= 0) {
        return;
    }

    for (int i = 0; i < lightCount; i++) {
        const EvaluatedLight& L = lights[i];
        if (L.radiusPx <= 4.0f || L.intensity <= 0.005f) {
            continue;
        }

        int minGx = std::max(0, (int)((L.screenX - L.radiusPx) / (float)kGridCellSize));
        int maxGx = std::min(gridW - 1, (int)((L.screenX + L.radiusPx) / (float)kGridCellSize) + 1);
        int minGy = std::max(0, (int)((L.screenY - L.radiusPx) / (float)kGridCellSize));
        int maxGy = std::min(gridH - 1, (int)((L.screenY + L.radiusPx) / (float)kGridCellSize) + 1);

        int lx = (int)(L.screenX / (float)kGridCellSize);
        int ly = (int)(L.screenY / (float)kGridCellSize);
        float invRadiusSq = 1.0f / (L.radiusPx * L.radiusPx);

        for (int gy = minGy; gy <= maxGy; gy++) {
            float py = (float)(gy * kGridCellSize + (kGridCellSize / 2));
            float dy = (py - L.screenY) * 1.25f; // Isometric aspect compensation
            float dySq = dy * dy;

            for (int gx = minGx; gx <= maxGx; gx++) {
                float px = (float)(gx * kGridCellSize + (kGridCellSize / 2));
                float dx = px - L.screenX;
                float distSq = dx * dx + dySq;
                float normDistSq = distSq * invRadiusSq;
                if (normDistSq >= 1.0f) {
                    continue;
                }

                float falloff = (1.0f - normDistSq);
                falloff = falloff * falloff * L.intensity;

                // Attenuate indoor lights across solid exterior roofs and vice-versa
                bool cellIndoor = (gCellRoofGrid[gy][gx] == CELL_INDOOR_REVEALED);
                if (L.indoorLight && gCellRoofGrid[gy][gx] == CELL_ROOF_VISIBLE) {
                    falloff *= 0.12f;
                } else if (!L.indoorLight && cellIndoor) {
                    falloff *= 0.35f;
                }

                if (L.castsWallShadows) {
                    falloff *= computeGridShadowFactor(lx, ly, gx, gy, gridW, gridH);
                }

                gLightmapGrid[gy][gx].r += L.r * falloff;
                gLightmapGrid[gy][gx].g += L.g * falloff;
                gLightmapGrid[gy][gx].b += L.b * falloff;
            }
        }
    }
}

static inline void blendPixelAlpha(uint32_t* dstRow, int x, int width, const uint8_t* maskRow, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    if (x < 0 || x >= width || maskRow[x] == 0 || alpha == 0) {
        return;
    }

    uint32_t bg = dstRow[x];
    uint32_t bgR = (bg >> 16) & 0xFFu;
    uint32_t bgG = (bg >> 8) & 0xFFu;
    uint32_t bgB = bg & 0xFFu;

    uint32_t a = alpha;
    uint32_t invA = 255u - a;
    uint32_t outR = (bgR * invA + (uint32_t)r * a) >> 8;
    uint32_t outG = (bgG * invA + (uint32_t)g * a) >> 8;
    uint32_t outB = (bgB * invA + (uint32_t)b * a) >> 8;

    dstRow[x] = (outR << 16) | (outG << 8) | outB;
}

static inline void blendPixelAdditive(uint32_t* dstRow, int x, int width, const uint8_t* maskRow, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    if (x < 0 || x >= width || maskRow[x] == 0 || alpha == 0) {
        return;
    }

    uint32_t bg = dstRow[x];
    uint32_t bgR = (bg >> 16) & 0xFFu;
    uint32_t bgG = (bg >> 8) & 0xFFu;
    uint32_t bgB = bg & 0xFFu;

    uint32_t outR = std::min(255u, bgR + (((uint32_t)r * alpha) >> 8));
    uint32_t outG = std::min(255u, bgG + (((uint32_t)g * alpha) >> 8));
    uint32_t outB = std::min(255u, bgB + (((uint32_t)b * alpha) >> 8));

    dstRow[x] = (outR << 16) | (outG << 8) | outB;
}

static void updateAndRenderParticles(uint32_t* dstPixels, int width, int height, int pitchPixels, const Rect& vpRect, int gridW, int gridH, float dtSec, float camDeltaX, float camDeltaY)
{
    int vpWidth = vpRect.lrx - vpRect.ulx + 1;
    int vpHeight = vpRect.lry - vpRect.uly + 1;

    gWindPhase += dtSec * 1.35f;
    float windGust = sinf(gWindPhase) * 0.25f + sinf(gWindPhase * 2.7f) * 0.12f;

    // Update lightning flashes during rainstorms
    if (gActiveWeather == ACTIVE_WEATHER_RAIN_STORM) {
        if (gLightningFlashIntensity > 0.0f) {
            gLightningFlashIntensity = std::max(0.0f, gLightningFlashIntensity - dtSec * 3.8f);
        }
        gNextLightningTimerSec -= dtSec;
        if (gNextLightningTimerSec <= 0.0f) {
            gLightningFlashIntensity = fastRandFloat(0.65f, 1.15f);
            gNextLightningTimerSec = fastRandFloat(9.0f, 22.0f);
        }
    } else {
        gLightningFlashIntensity = 0.0f;
    }

    // 1. Update & draw ambient weather particles
    int activeWeatherCount = targetWeatherParticleCount(gActiveWeather);
    for (int i = 0; i < activeWeatherCount; i++) {
        WeatherParticle* p = &gWeatherParticles[i];
        if (!p->active) {
            spawnWeatherParticle(p, gActiveWeather, vpWidth, vpHeight, false);
        }

        // Apply world-space camera scroll parallax compensation
        float parallax = 0.70f + 0.15f * (float)p->layer;
        p->x += camDeltaX * parallax;
        p->y += camDeltaY * parallax;

        p->age += dtSec;
        if (p->kind == PARTICLE_ASH_FLAKE || p->kind == PARTICLE_RAD_EMBER || p->kind == PARTICLE_DUST_MOTE || p->kind == PARTICLE_RAD_MOTE) {
            float flutter = sinf(gAccumTimeSec * 2.6f + p->phase) * 24.0f;
            p->x += (p->vx + flutter) * dtSec;
            p->y += p->vy * dtSec;
        } else {
            p->x += p->vx * (1.0f + windGust) * dtSec;
            p->y += p->vy * dtSec;
        }

        // Respawn particles that leave the viewport bounds
        if (p->age >= p->maxAge || p->y > (float)vpHeight + 30.0f || p->x < -110.0f || p->x > (float)vpWidth + 160.0f) {
            // If a raindrop hits an unroofed floor tile inside the viewport, spawn a water ripple splash
            if (p->kind == PARTICLE_RAIN_STREAK && (fastRandU32() & 3u) == 0) {
                int splashX = (int)p->x;
                int splashY = (int)fastRandFloat(20.0f, (float)vpHeight - 8.0f);
                if (splashX >= vpRect.ulx && splashX <= vpRect.lrx && splashY >= vpRect.uly && splashY <= vpRect.lry) {
                    int gx = splashX / kGridCellSize;
                    int gy = splashY / kGridCellSize;
                    if (gx >= 0 && gx < gridW && gy >= 0 && gy < gridH && gCellRoofGrid[gy][gx] == CELL_EXTERIOR_OPEN) {
                        spawnEffectParticle(PARTICLE_RAIN_SPLASH, (float)splashX, (float)splashY, 0.0f, 0.0f, 0.28f, 185, 215, 245, 135, 3.0f);
                    }
                }
            }
            spawnWeatherParticle(p, gActiveWeather, vpWidth, vpHeight, false);
            continue;
        }

        int px = (int)p->x;
        int py = (int)p->y;
        if (px < vpRect.ulx || px > vpRect.lrx || py < vpRect.uly || py > vpRect.lry) {
            continue;
        }

        // Suppress exterior weather inside roof-revealed indoor rooms
        if (gActiveWeather != ACTIVE_WEATHER_UNDERGROUND_DUST && isScreenPointIndoorRevealed(px, py, gridW, gridH)) {
            continue;
        }

        if (p->kind == PARTICLE_RAIN_STREAK) {
            int steps = (int)p->size;
            float invSteps = 1.0f / (float)std::max(1, steps);
            float dxStep = (p->vx * 0.018f) * invSteps;
            float dyStep = (p->vy * 0.018f) * invSteps;
            for (int s = 0; s < steps; s++) {
                int sx = (int)(p->x - dxStep * (float)s);
                int sy = (int)(p->y - dyStep * (float)s);
                if (sy >= vpRect.uly && sy <= vpRect.lry && sx >= vpRect.ulx && sx <= vpRect.lrx) {
                    uint8_t segAlpha = (uint8_t)(((int)p->alpha * (steps - s)) / steps);
                    blendPixelAlpha(
                        dstPixels + sy * pitchPixels,
                        sx,
                        width,
                        gViewportMask.data() + sy * width,
                        p->r,
                        p->g,
                        p->b,
                        segAlpha);
                }
            }
        } else if (p->kind == PARTICLE_SAND_GRAIN) {
            int steps = std::max(2, (int)p->size);
            for (int s = 0; s < steps; s++) {
                int sx = px - s * 2;
                int sy = py - (s / 2);
                if (sy >= vpRect.uly && sy <= vpRect.lry && sx >= vpRect.ulx && sx <= vpRect.lrx) {
                    uint8_t segAlpha = (uint8_t)(((int)p->alpha * (steps - s)) / steps);
                    blendPixelAlpha(
                        dstPixels + sy * pitchPixels,
                        sx,
                        width,
                        gViewportMask.data() + sy * width,
                        p->r,
                        p->g,
                        p->b,
                        segAlpha);
                }
            }
        } else if (p->kind == PARTICLE_RAD_EMBER || p->kind == PARTICLE_RAD_MOTE) {
            uint8_t pulseAlpha = (uint8_t)((float)p->alpha * (0.65f + 0.35f * sinf(gAccumTimeSec * 6.0f + p->phase)));
            for (int dy = -1; dy <= 1; dy++) {
                int sy = py + dy;
                if (sy < vpRect.uly || sy > vpRect.lry) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int sx = px + dx;
                    uint8_t a = (dx == 0 && dy == 0) ? pulseAlpha : (uint8_t)(pulseAlpha / 3);
                    blendPixelAdditive(
                        dstPixels + sy * pitchPixels,
                        sx,
                        width,
                        gViewportMask.data() + sy * width,
                        p->r,
                        p->g,
                        p->b,
                        a);
                }
            }
        } else {
            // Ash flake or dust mote
            blendPixelAlpha(
                dstPixels + py * pitchPixels,
                px,
                width,
                gViewportMask.data() + py * width,
                p->r,
                p->g,
                p->b,
                p->alpha);
            if (p->size >= 2.0f && px + 1 <= vpRect.lrx) {
                blendPixelAlpha(
                    dstPixels + py * pitchPixels,
                    px + 1,
                    width,
                    gViewportMask.data() + py * width,
                    p->r,
                    p->g,
                    p->b,
                    (uint8_t)(p->alpha * 3 / 4));
            }
        }
    }

    // 2. Update & draw transient effect particles (rain splashes, fire sparks, combat sparks, radioactive motes)
    for (int i = 0; i < kMaxEffectParticles; i++) {
        WeatherParticle* p = &gEffectParticles[i];
        if (!p->active) {
            continue;
        }

        p->x += camDeltaX;
        p->y += camDeltaY;
        p->age += dtSec;
        if (p->age >= p->maxAge) {
            p->active = false;
            continue;
        }

        float normAge = p->age / p->maxAge;
        p->x += p->vx * dtSec;
        p->y += p->vy * dtSec;
        if (p->kind == PARTICLE_COMBAT_SPARK) {
            p->vy += 120.0f * dtSec; // gravity on ballistic sparks
        }

        int px = (int)p->x;
        int py = (int)p->y;
        if (px < vpRect.ulx || px > vpRect.lrx || py < vpRect.uly || py > vpRect.lry) {
            continue;
        }

        uint8_t curAlpha = (uint8_t)((float)p->alpha * (1.0f - normAge));
        if (p->kind == PARTICLE_RAIN_SPLASH) {
            // Expanding isometric oval ripple on wet ground
            int rx = 1 + (int)(normAge * 4.0f);
            int ry = 1 + (int)(normAge * 2.0f);
            int pts[8][2] = {
                { px - rx, py },
                { px + rx, py },
                { px, py - ry },
                { px, py + ry },
                { px - rx + 1, py - ry },
                { px + rx - 1, py - ry },
                { px - rx + 1, py + ry },
                { px + rx - 1, py + ry },
            };
            for (int k = 0; k < 8; k++) {
                int sx = pts[k][0];
                int sy = pts[k][1];
                if (sy >= vpRect.uly && sy <= vpRect.lry && sx >= vpRect.ulx && sx <= vpRect.lrx) {
                    blendPixelAlpha(
                        dstPixels + sy * pitchPixels,
                        sx,
                        width,
                        gViewportMask.data() + sy * width,
                        p->r,
                        p->g,
                        p->b,
                        curAlpha);
                }
            }
        } else {
            // Additive glowing spark / ember / radiation mote
            for (int dy = -1; dy <= 1; dy++) {
                int sy = py + dy;
                if (sy < vpRect.uly || sy > vpRect.lry) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int sx = px + dx;
                    uint8_t a = (dx == 0 && dy == 0) ? curAlpha : (uint8_t)(curAlpha / 2);
                    blendPixelAdditive(
                        dstPixels + sy * pitchPixels,
                        sx,
                        width,
                        gViewportMask.data() + sy * width,
                        p->r,
                        p->g,
                        p->b,
                        a);
                }
            }
        }
    }
}

} // namespace

void weather_light_init()
{
    memset(gWeatherParticles, 0, sizeof(gWeatherParticles));
    memset(gEffectParticles, 0, sizeof(gEffectParticles));
    memset(gTransientLights, 0, sizeof(gTransientLights));
    gLastMapIndex = -999;
    gLastElevation = -999;
    gLastCenterTile = -1;
    gLastTickMs = SDL_GetTicks();
    gAccumTimeSec = 0.0f;
    gWeatherLightInitialized = true;
}

void weather_light_reset()
{
    memset(gEffectParticles, 0, sizeof(gEffectParticles));
    memset(gTransientLights, 0, sizeof(gTransientLights));
    gLastMapIndex = -999;
    gLastElevation = -999;
    gLastCenterTile = -1;
    gLightningFlashIntensity = 0.0f;
}

void weather_light_exit()
{
    gViewportMask.clear();
    gWeatherLightInitialized = false;
}

void weather_light_on_map_changed()
{
    gLastMapIndex = -999;
    gLastElevation = -999;
    gLastCenterTile = -1;
    memset(gEffectParticles, 0, sizeof(gEffectParticles));
    memset(gTransientLights, 0, sizeof(gTransientLights));
}

void weather_light_trigger_effect(Object* sourceObj, int tile, int elevation, DynamicLightEffectType effectType)
{
    if (!gWeatherLightInitialized) {
        weather_light_init();
    }

    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_DYNAMIC_LIGHTING_KEY, &gDynamicLightingConfig);
    if (!gDynamicLightingConfig) {
        return;
    }

    if (sourceObj != NULL) {
        tile = sourceObj->tile;
        elevation = sourceObj->elevation;
    }
    if (!hexGridTileIsValid(tile) || !elevationIsValid(elevation)) {
        return;
    }

    int slot = -1;
    for (int i = 0; i < kMaxTransientLights; i++) {
        if (!gTransientLights[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        slot = (int)(fastRandU32() % kMaxTransientLights);
    }

    TransientDynamicLight* tl = &gTransientLights[slot];
    tl->active = true;
    tl->tile = tile;
    tl->elevation = elevation;
    tl->offsetX = (sourceObj != NULL) ? (float)sourceObj->x : 0.0f;
    tl->offsetY = (sourceObj != NULL) ? (float)sourceObj->y : 0.0f;
    tl->ageSec = 0.0f;
    tl->flicker = true;

    uint8_t sparkR = 255;
    uint8_t sparkG = 195;
    uint8_t sparkB = 85;
    int sparkCount = 14;

    switch (effectType) {
    case DYN_LIGHT_MUZZLE_LASER:
        tl->r = 1.0f;
        tl->g = 0.20f;
        tl->b = 0.18f;
        tl->radiusPx = 190.0f;
        tl->intensity = 1.45f;
        tl->durationSec = 0.18f;
        sparkR = 255;
        sparkG = 65;
        sparkB = 65;
        sparkCount = 16;
        break;

    case DYN_LIGHT_MUZZLE_PLASMA:
    case DYN_LIGHT_PLASMA_BLAST:
        tl->r = 0.25f;
        tl->g = 1.0f;
        tl->b = 0.42f;
        tl->radiusPx = (effectType == DYN_LIGHT_PLASMA_BLAST) ? 240.0f : 195.0f;
        tl->intensity = (effectType == DYN_LIGHT_PLASMA_BLAST) ? 1.75f : 1.45f;
        tl->durationSec = (effectType == DYN_LIGHT_PLASMA_BLAST) ? 0.42f : 0.24f;
        sparkR = 75;
        sparkG = 255;
        sparkB = 105;
        sparkCount = 24;
        break;

    case DYN_LIGHT_MUZZLE_PULSE:
    case DYN_LIGHT_ELECTRICAL_ARC:
        tl->r = 0.42f;
        tl->g = 0.80f;
        tl->b = 1.0f;
        tl->radiusPx = 225.0f;
        tl->intensity = 1.70f;
        tl->durationSec = 0.32f;
        sparkR = 115;
        sparkG = 210;
        sparkB = 255;
        sparkCount = 28;
        break;

    case DYN_LIGHT_MUZZLE_FLAME:
        tl->r = 1.0f;
        tl->g = 0.52f;
        tl->b = 0.15f;
        tl->radiusPx = 215.0f;
        tl->intensity = 1.55f;
        tl->durationSec = 0.38f;
        sparkR = 255;
        sparkG = 145;
        sparkB = 40;
        sparkCount = 26;
        break;

    case DYN_LIGHT_EXPLOSION:
        tl->r = 1.0f;
        tl->g = 0.64f;
        tl->b = 0.22f;
        tl->radiusPx = 285.0f;
        tl->intensity = 2.10f;
        tl->durationSec = 0.52f;
        sparkR = 255;
        sparkG = 175;
        sparkB = 60;
        sparkCount = 42;
        break;

    case DYN_LIGHT_MUZZLE_GUN:
    default:
        tl->r = 1.0f;
        tl->g = 0.76f;
        tl->b = 0.36f;
        tl->radiusPx = 175.0f;
        tl->intensity = 1.35f;
        tl->durationSec = 0.15f;
        sparkR = 255;
        sparkG = 205;
        sparkB = 95;
        sparkCount = 12;
        break;
    }

    if (elevation == map_elevation) {
        int sx = 0;
        int sy = 0;
        if (tile_coord(tile, &sx, &sy, elevation) == 0) {
            sx += 16 + (int)tl->offsetX;
            sy += 8 + (int)tl->offsetY - 22;
            for (int i = 0; i < sparkCount; i++) {
                float angle = fastRandFloat(0.0f, 6.2831853f);
                float speed = fastRandFloat(35.0f, 165.0f);
                spawnEffectParticle(
                    PARTICLE_COMBAT_SPARK,
                    (float)sx + fastRandFloat(-6.0f, 6.0f),
                    (float)sy + fastRandFloat(-6.0f, 6.0f),
                    cosf(angle) * speed,
                    sinf(angle) * speed - 35.0f,
                    fastRandFloat(0.22f, 0.58f),
                    sparkR,
                    sparkG,
                    sparkB,
                    245,
                    2.0f);
            }
        }
    }
}

void weather_light_trigger_from_anim(Object* owner, int anim)
{
    if (owner == NULL) {
        return;
    }

    switch (anim) {
    case ANIM_FIRE_SINGLE:
    case ANIM_FIRE_BURST:
    case ANIM_FIRE_CONTINUOUS: {
        DynamicLightEffectType kind = DYN_LIGHT_MUZZLE_GUN;
        Object* weapon = inven_right_hand(owner);
        if (weapon == NULL) {
            weapon = inven_left_hand(owner);
        }
        if (weapon != NULL) {
            const char* wname = object_name(weapon);
            if (containsIgnoreCase(wname, "plasma") || containsIgnoreCase(wname, "alien")) {
                kind = DYN_LIGHT_MUZZLE_PLASMA;
            } else if (containsIgnoreCase(wname, "laser") || containsIgnoreCase(wname, "gatling")) {
                kind = DYN_LIGHT_MUZZLE_LASER;
            } else if (containsIgnoreCase(wname, "flamer") || containsIgnoreCase(wname, "molotov")) {
                kind = DYN_LIGHT_MUZZLE_FLAME;
            } else if (containsIgnoreCase(wname, "pulse")) {
                kind = DYN_LIGHT_MUZZLE_PULSE;
            }
        }
        weather_light_trigger_effect(owner, owner->tile, owner->elevation, kind);
        break;
    }
    case ANIM_EXPLODED_TO_NOTHING:
    case ANIM_CHUNKS_OF_FLESH:
        weather_light_trigger_effect(owner, owner->tile, owner->elevation, DYN_LIGHT_EXPLOSION);
        break;
    case ANIM_FIRE_DANCE:
    case ANIM_BURNED_TO_NOTHING:
    case ANIM_CHARRED_BODY:
        weather_light_trigger_effect(owner, owner->tile, owner->elevation, DYN_LIGHT_MUZZLE_FLAME);
        break;
    case ANIM_ELECTRIFY:
    case ANIM_ELECTRIFIED_TO_NOTHING:
        weather_light_trigger_effect(owner, owner->tile, owner->elevation, DYN_LIGHT_ELECTRICAL_ARC);
        break;
    case ANIM_SLICED_IN_HALF:
        weather_light_trigger_effect(owner, owner->tile, owner->elevation, DYN_LIGHT_MUZZLE_LASER);
        break;
    case ANIM_MELTED_TO_NOTHING:
        weather_light_trigger_effect(owner, owner->tile, owner->elevation, DYN_LIGHT_PLASMA_BLAST);
        break;
    default:
        if (FID_TYPE(owner->fid) == OBJ_TYPE_MISC) {
            weather_light_trigger_effect(owner, owner->tile, owner->elevation, DYN_LIGHT_EXPLOSION);
        }
        break;
    }
}

void weather_light_trigger_from_sfx(const char* sfxName, Object* emitter)
{
    if (sfxName == NULL || emitter == NULL) {
        return;
    }

    // Fallout weapon sound naming convention: "wa..." or explosion SFX
    char c0 = (char)tolower((unsigned char)sfxName[0]);
    char c1 = (char)tolower((unsigned char)sfxName[1]);
    if (c0 == 'w') {
        if (c1 == 'h' || c1 == 'i') {
            return; // melee swing/impact
        }
        weather_light_trigger_from_anim(emitter, ANIM_FIRE_SINGLE);
    } else if (containsIgnoreCase(sfxName, "explode") || containsIgnoreCase(sfxName, "whn1") || containsIgnoreCase(sfxName, "whp1")) {
        weather_light_trigger_effect(emitter, emitter->tile, emitter->elevation, DYN_LIGHT_EXPLOSION);
    }
}

void weather_light_composite_frame(const uint32_t* srcPixels, uint32_t* dstPixels, int width, int height, int pitchBytes)
{
    if (srcPixels == NULL || dstPixels == NULL || width <= 0 || height <= 0) {
        return;
    }

    int pitchPixels = pitchBytes / 4;
    size_t totalBytes = (size_t)pitchBytes * (size_t)height;
    memcpy(dstPixels, srcPixels, totalBytes);

    if (!gWeatherLightInitialized) {
        weather_light_init();
    }

    size_t pixelCount = (size_t)width * (size_t)height;
    if (gViewportMask.size() != pixelCount) {
        gViewportMask.resize(pixelCount, 0);
    }

    Rect vpRect;
    if (!win_build_display_win_mask(display_win, gViewportMask.data(), width, height, &vpRect)) {
        return;
    }

    uint32_t nowMs = SDL_GetTicks();
    uint32_t deltaMs = nowMs - gLastTickMs;
    gLastTickMs = nowMs;
    if (deltaMs > 100) {
        deltaMs = 16;
    }
    float dtSec = (float)deltaMs * 0.001f;
    gAccumTimeSec += dtSec;

    int vpWidth = vpRect.lrx - vpRect.ulx + 1;
    int vpHeight = vpRect.lry - vpRect.uly + 1;
    int gridW = std::min(kMaxGridWidth - 1, (width + kGridCellSize - 1) / kGridCellSize + 1);
    int gridH = std::min(kMaxGridHeight - 1, (height + kGridCellSize - 1) / kGridCellSize + 1);

    // Detect map or elevation changes and initialize weather particles
    ActiveWeatherMode desiredWeather = determineWeatherForCurrentMap();
    if (map_data.field_34 != gLastMapIndex || map_elevation != gLastElevation || desiredWeather != gActiveWeather) {
        gLastMapIndex = map_data.field_34;
        gLastElevation = map_elevation;
        gActiveWeather = desiredWeather;
        gLastCenterTile = tile_center_tile;
        tile_coord(tile_center_tile, &gLastCenterScreenX, &gLastCenterScreenY, map_elevation);
        rebuildWeatherParticles(vpWidth, vpHeight);
    }

    // Compute camera scroll delta in screen space so particles stay anchored in world space
    float camDeltaX = 0.0f;
    float camDeltaY = 0.0f;
    if (gLastCenterTile != -1 && tile_center_tile != gLastCenterTile) {
        int prevCenterNowX = 0;
        int prevCenterNowY = 0;
        if (tile_coord(gLastCenterTile, &prevCenterNowX, &prevCenterNowY, map_elevation) == 0) {
            camDeltaX = (float)(prevCenterNowX - gLastCenterScreenX);
            camDeltaY = (float)(prevCenterNowY - gLastCenterScreenY);
        }
        gLastCenterTile = tile_center_tile;
        tile_coord(tile_center_tile, &gLastCenterScreenX, &gLastCenterScreenY, map_elevation);
    } else if (gLastCenterTile == -1 && hexGridTileIsValid(tile_center_tile)) {
        gLastCenterTile = tile_center_tile;
        tile_coord(tile_center_tile, &gLastCenterScreenX, &gLastCenterScreenY, map_elevation);
    }

    // 1. Build 8x8 roof state and wall blocker grids
    buildRoofAndWallGrids(gridW, gridH);

    // 2. Collect dynamic light sources & update effect emitters
    EvaluatedLight lights[kMaxEvaluatedLights];
    int lightCount = 0;
    collectActiveLightsAndBlockers(lights, &lightCount, vpWidth, vpHeight, gridW, gridH, dtSec);

    // 3. Evaluate 8x8 RGB lightmap with wall shadowcasting & weather sky/haze modulation
    evaluateDynamicLightmapGrid(lights, lightCount, gridW, gridH);

    // 4. Apply bilinearly interpolated RGB lightmap to unoccluded viewport pixels
    bool hasAnyGridEffect = (gDynamicLightingConfig && lightCount > 0) || (gActiveWeather != ACTIVE_WEATHER_NONE);
    if (hasAnyGridEffect) {
        for (int y = vpRect.uly; y <= vpRect.lry; y++) {
            int gy0 = y / kGridCellSize;
            int gy1 = std::min(gridH - 1, gy0 + 1);
            int wy = y & (kGridCellSize - 1);
            int invWy = kGridCellSize - wy;

            uint32_t* row = dstPixels + y * pitchPixels;
            const uint8_t* maskRow = gViewportMask.data() + y * width;

            for (int x = vpRect.ulx; x <= vpRect.lrx; x++) {
                if (maskRow[x] == 0) {
                    continue;
                }

                int gx0 = x / kGridCellSize;
                int gx1 = std::min(gridW - 1, gx0 + 1);
                int wx = x & (kGridCellSize - 1);
                int invWx = kGridCellSize - wx;

                int w00 = invWx * invWy;
                int w10 = wx * invWy;
                int w01 = invWx * wy;
                int w11 = wx * wy;

                const LightCellRGB& c00 = gLightmapGrid[gy0][gx0];
                const LightCellRGB& c10 = gLightmapGrid[gy0][gx1];
                const LightCellRGB& c01 = gLightmapGrid[gy1][gx0];
                const LightCellRGB& c11 = gLightmapGrid[gy1][gx1];

                float lr = (c00.r * (float)w00 + c10.r * (float)w10 + c01.r * (float)w01 + c11.r * (float)w11) * (1.0f / 64.0f);
                float lg = (c00.g * (float)w00 + c10.g * (float)w10 + c01.g * (float)w01 + c11.g * (float)w11) * (1.0f / 64.0f);
                float lb = (c00.b * (float)w00 + c10.b * (float)w10 + c01.b * (float)w01 + c11.b * (float)w11) * (1.0f / 64.0f);

                if (lr <= 0.004f && lg <= 0.004f && lb <= 0.004f) {
                    continue;
                }

                uint32_t px = row[x];
                float baseR = (float)((px >> 16) & 0xFFu);
                float baseG = (float)((px >> 8) & 0xFFu);
                float baseB = (float)(px & 0xFFu);

                // Colored surface illumination + soft atmospheric bloom
                uint32_t outR = (uint32_t)std::min(255.0f, baseR * (1.0f + lr * 0.85f) + lr * 72.0f);
                uint32_t outG = (uint32_t)std::min(255.0f, baseG * (1.0f + lg * 0.85f) + lg * 72.0f);
                uint32_t outB = (uint32_t)std::min(255.0f, baseB * (1.0f + lb * 0.85f) + lb * 72.0f);

                row[x] = (outR << 16) | (outG << 8) | outB;
            }
        }
    }

    // 5. Update and render 60 FPS screen-space weather & combat/environment particles
    updateAndRenderParticles(dstPixels, width, height, pitchPixels, vpRect, gridW, gridH, dtSec, camDeltaX, camDeltaY);
}

} // namespace fallout
