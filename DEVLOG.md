# Fallout 1 Community Edition (Win64) — Development Log

## [2026-11-04] Screen-Space Weather & Particle Engine + Dynamic Colored Light System — Win64 Build

### Summary
Implemented a native **32-bit RGB Post-Viewport Compositor** (`src/game/weather_light.cc`, `src/plib/gnw/svga.cc`, `src/plib/gnw/gnw.cc`) running at 60 FPS on top of the unoccluded isometric game viewport (`display_win`) without smearing the 8-bit dirty-rectangle tile buffer or affecting the Pip-Boy HUD, dialog/inventory windows, or mouse cursor.

### Key Features Implemented
* **Screen-Space Weather & Particle Engine (`weather_effects` in `[preferences]`):**
  * **Regional Automatic Weather (`weather_effects=1`):** Inspects `map_data.field_34` (`Map` enum) and `map_elevation` to automatically activate lore-accurate regional weather:
    * **Acid / Wasteland Rain & Thunderstorm (`weather_effects=2` to force):** 3-layer parallax slanted rain streaks, animated water-ripple ground splashes on unroofed outdoor tiles, wind gusts, and periodic lightning flashes with thunderclaps.
    * **Radioactive Fallout Ash & Embers (`weather_effects=3` to force):** Sinusoidal fluttering ash flakes, glowing amber/green radioactive embers (`MAP_GLOWENT`, `MAP_NECRO`, `MAP_WATERSHD`, `MAP_CHILDRN1`), and fallout sky grading.
    * **Wasteland Sandstorm (`weather_effects=4` to force):** High-velocity wind-blown sand streaks, scrolling procedural volumetric dust haze bands, and desert ochre grading (`MAP_DESERT1..3`, `MAP_SHADYE/W`, `MAP_JUNK*`, `MAP_RAIDERS`, `MAP_TARDIS`).
    * **Subterranean Dust & Radiation Motes:** Gentle floating dust and glowing green radiation spores in Vaults, Caves, The Glow interior (`MAP_GLOW1..2`), and Mariposa Military Base (`MAP_MBVATS12`).
  * **World-Space Camera Scroll Parallax:** Tracks `tile_coord(tile_center_tile, ...)` deltas every frame so all weather and combat particles remain physically anchored in the world when scrolling the camera.
  * **Roof Cut-Away & Indoor Masking:** Inspects `square_xy_roof` and the `square[map_elevation]` roof flag bit (`0x01` cut-away flag + `obj_dude` roof state) to prevent exterior rain, ash, sandstorms, and lightning from rendering inside roofed rooms while keeping weather visible outdoors through windows and doorways.
* **Dynamic Colored Light System (`dynamic_lighting=1` in `[preferences]`):**
  * **True 32-bit RGB Colored Lighting & Bloom:** Bypasses the 256-color palette limitation by evaluating a smooth bilinearly-interpolated RGB lightmap and bloom overlay over the isometric viewport.
  * **Automatic Map Scenery & Critter Lights:** Scans visible objects in `display_win` and attaches colored dynamic lights and micro-particles to:
    * **Fires, Burn Barrels & Torches:** Dual-frequency Perlin-style flickering warm orange-amber light + rising glowing fire sparks.
    * **Radioactive Goo, Slime Pools & Glowing Ones (`MAP_GLOW*`, `MAP_MBVATS12`, `MAP_NECRO`):** Pulsing emerald-green illumination + rising radioactive spore motes.
    * **Vault / Brotherhood Terminals & Forcefields:** Subtle pulsing cyber-cyan/blue glow.
    * **Emergency Alarms:** Pulsing crimson red light.
    * **Flying Projectiles (`OBJ_TYPE_MISC`):** Real-time moving point lights tracking plasma bolts (emerald green), laser beams (ruby red), and rockets (exhaust orange).
  * **Transient Combat Muzzle Flashes, Explosions & Ballistic Sparks:** Hooks `anim_animate()`, `ANIM_KIND_SET_LIGHT_DISTANCE`, and `gsound_apply_spatial_to_sound()` to trigger instant colored light bursts and bouncing spark particles for gunfire, laser/plasma/pulse weapons, flamethrowers, and explosions.
  * **2D Isometric Wall Shadowcasting:** Builds a fast screen-space wall blocker grid from `OBJ_TYPE_WALL` and closed opaque doors (`!OBJECT_OPEN_DOOR`) and runs DDA raymarching from every light source so light is realistically blocked by walls and casts soft shadows across floors.

---

## [2026-11-04] OpenAL Soft 3D Surround Sound & ALC_EXT_EFX (EAX 2.0) Integration — Verified Win64 Build

### Summary
Replaced the flat 2-channel SDL2 callback mixer with a full 3D **OpenAL Soft** audio backend featuring **Surround Sound (5.1 / 7.1 / Binaural HRTF)** and **`ALC_EXT_EFX` (`AL_EFFECT_EAXREVERB` + `AL_FILTER_LOWPASS`)** environmental acoustics and dynamic wall/roof occlusion.

### Win64 Build Verification
* **Binary Outputs:**
  * `build/win64/fallout-ce.exe` (7.6 MB, `PE32+ executable (GUI) x86-64, for MS Windows`)
  * `fallout-ce.exe` (copied to repository root for convenience)
* **Toolchain & Static Linking:**
  * Built with `x86_64-w64-mingw32-g++-posix` via `cmake/toolchain-mingw64.cmake`.
  * Statically links `libOpenAL32.a` (OpenAL Soft `1.21.1` with WASAPI, DirectSound, WinMM backends, SSE/SSE2/SSE3/SSE4.1 mixers, and embedded `Default HRTF.mhr`), `libSDL2.a`, `libadecode-static.a`, `libfpattern-static.a`, `libstdc++`, `libgcc`, and `libwinpthread`.
  * **Zero non-OS DLL dependencies:** Depends exclusively on standard Windows system DLLs (`KERNEL32.dll`, `USER32.dll`, `GDI32.dll`, `WINMM.dll`, `ole32.dll`, `OLEAUT32.dll`, `ADVAPI32.dll`, `SHELL32.dll`, `SETUPAPI.dll`, `IMM32.dll`, `VERSION.dll`, `msvcrt.dll`).

### Modified & Added Files
* `CMakeLists.txt` — Integrated `third_party/openal-soft` (`AL_LIBTYPE_STATIC`, `AL_ALEXT_PROTOTYPES`) into the Win64 build pipeline.
* `third_party/openal-soft/CMakeLists.txt` — Added `FetchContent` static build configuration for OpenAL Soft `1.21.1` with embedded HRTF datasets (`ALSOFT_EMBED_HRTF_DATA`).
* `cmake/toolchain-mingw64.cmake` — Added Win64 (`x86_64-w64-mingw32-g++-posix`) cross-compilation toolchain with full static runtime linking (`-static -static-libgcc -static-libstdc++`).
* `src/audio_engine.h` & `src/audio_engine.cc` —
  * Replaced 8-buffer flat SDL2 mixer with a 32-channel OpenAL Soft streaming/positional engine.
  * Added `AudioEngineSurroundMode` (`AUDIO_SURROUND_MODE_STEREO`, `AUDIO_SURROUND_MODE_AUTO`, `AUDIO_SURROUND_MODE_HRTF`) using `ALC_SOFT_HRTF` and `AL_SOFT_source_spatialize`.
  * Implemented all 26 classic EAX 2.0 reverb presets (`EFXEAXREVERBPROPERTIES` via `AL_EFFECT_EAXREVERB`) with real-time continuous parameter interpolation (`interpolateEaxProps`) in the audio worker thread to eliminate clicks during indoor/outdoor transitions.
  * Added per-source `AL_FILTER_LOWPASS` filters on both the direct path (`AL_DIRECT_FILTER`) and EAX reverb send (`AL_AUXILIARY_SEND_FILTER`) for wall, door, and roof occlusion.
* `src/int/sound.h` & `src/int/sound.cc` —
  * Extended `Sound` descriptor with `spatialObject`, `isSpatial`, `eaxSendEnabled`, 3D coordinates `(posX, posY, posZ)`, and EFX low-pass occlusion gains.
  * Added `soundSet3DPosition`, `soundSetOcclusion`, `soundSetEaxSend`, `soundSetSpatialObject`, `soundGetSpatialObject`, and `soundGetFirstActive`.
* `src/game/gconfig.h` & `src/game/gconfig.cc` —
  * Added `[sound]` configuration keys `surround_mode=1` (`0` = Stereo, `1` = Auto Surround/Multichannel, `2` = Force HRTF) and `eax_environment=1` (`0` = Disabled, `1` = Enabled).
* `src/game/gsound.h` & `src/game/gsound.cc` —
  * Implemented isometric hex-to-3D meter coordinate conversion (`gsound_compute_object_3d_and_occlusion`) anchored to `obj_dude` and blended with `tile_center_tile`.
  * Implemented hex-grid line-of-sight wall/door/roof occlusion raycasting (`obj_sight_blocking_at`, `obj_blocking_at`, `gsound_tile_has_roof`).
  * Implemented `gsound_update_listener_and_environment()` which inspects current `Map` ID (`MAP_DESERT1` .. `MAP_HUBMIS1`), elevation, roof coverage above `obj_dude->tile`, and 6-directional hex wall raycasts to dynamically select and scale EAX 2.0 presets (`CAVE`, `STONEROOM`, `HALLWAY`, `HANGAR`, `SEWERPIPE`, `AUDITORIUM`, `STONECORRIDOR`, `CITY`, `ALLEY`, `MOUNTAINS`, `PLAIN`, etc.).
  * Added real-time positional tracking for active spatial sounds in `gsound_bkg_proc()`.
* `src/game/anim.cc`, `src/game/combatai.cc`, `src/game/protinst.cc`, `src/int/support/intextra.cc`, `src/game/map.cc` —
  * Wired object emitters into animation sound callbacks (`register_object_play_sfx` -> `gsnd_anim_sound`), combat AI weapon reload/out-of-ammo SFX, locked doors/containers, script `op_play_sfx` calls, and map/elevation transitions.
