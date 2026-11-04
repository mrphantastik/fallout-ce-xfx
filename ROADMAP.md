# Fallout 1 Community Edition (Win64) — Engineering Roadmap

## Completed Milestones
- [x] **OpenAL Soft Audio Backend (`src/audio_engine.cc`)**
  - Replaced legacy 8-voice flat SDL2 audio mixer with 32-voice OpenAL Soft streaming & positional architecture.
  - Preserved full circular ring-buffer compatibility (`Lock`/`Unlock`/`GetCurrentPosition`) for ACM streaming music, speech, and Interplay MVE movies.
- [x] **3D Surround Sound (5.1 / 7.1 / Binaural HRTF)**
  - Isometric hex-to-3D world coordinate conversion for critters, weapons, projectiles, doors, containers, and scripted map objects.
  - Configurable `surround_mode` in `fallout.cfg` (`0` = Stereo, `1` = Auto Multichannel/Surround, `2` = Binaural HRTF).
  - Real-time position updates for moving critters, projectiles, and camera scrolling.
- [x] **EAX 2.0 Environmental Audio via `ALC_EXT_EFX` (`AL_EFFECT_EAXREVERB`)**
  - Integrated all 26 classic EAX 2.0 acoustic presets (`<AL/efx-presets.h>`).
  - Dynamic indoor/outdoor detection via Fallout 1 roof tile layer (`square[elevation]`) and 6-axis hex wall raycasting for room size scaling.
  - Smooth parameter interpolation across presets to prevent reverb tail clicks when crossing doorways.
  - Hex raycast wall/door/roof occlusion using per-source `AL_FILTER_LOWPASS` on direct and auxiliary reverb paths.
- [x] **Standalone Win64 Build (`build/win64/fallout-ce.exe` & `/fallout-ce.exe`)**
  - Cross-compiled `PE32+ executable (GUI) x86-64` with OpenAL Soft `1.21.1` (embedded HRTF datasets, WASAPI/DirectSound/WinMM backends) and SDL2 `2.26.1` statically linked.
- [x] **Screen-Space Weather & Particle Engine (`src/game/weather_light.cc`)**
  - 60 FPS post-viewport 32-bit RGB particle & atmosphere compositor with automatic window/HUD/cursor occlusion masking (`win_build_display_win_mask`).
  - Regional weather presets (Acid/Wasteland Rain & Thunderstorm, Radioactive Fallout Ash & Embers, Desert Sandstorm with scrolling volumetric dust haze, and Subterranean Dust/Radiation Motes).
  - Camera-scroll world-space parallax and automatic roof cut-away masking (`square_xy_roof` / indoor reveal detection).
- [x] **Dynamic Colored Light System & Isometric Wall Shadowcasting (`src/game/weather_light.cc`)**
  - True 32-bit RGB colored lightmap with bilinear interpolation, additive bloom, and DDA wall shadowcasting (`OBJ_TYPE_WALL` and closed doors).
  - Flickering fire/burn-barrel lights with rising sparks, pulsing radioactive goo/Glowing One emerald lights with spore motes, terminal/forcefield cyan glow, real-time flying projectile lights, and weapon muzzle flash / explosion light bursts with ballistic sparks.

## Upcoming / Planned Enhancements
- [ ] **In-Game Sound Options UI Extension (`src/game/options.cc`)**
  - Add toggles/sliders in the Fallout Preferences screen for Surround Mode (`Stereo / Surround / HRTF`) and EAX 2.0 Environmental Reverb intensity.
- [ ] **Dual Auxiliary Effect Slot Crossfading (Indoor/Outdoor Portal Blending)**
  - Run simultaneous Indoor and Outdoor EFX Auxiliary Effect Slots so exterior gunfire heard from inside a building carries outdoor reflections before entering the indoor room filter.
- [ ] **Material-Specific Early Reflection Tuning**
  - Inspect nearby wall prototype materials (`MATERIAL_TYPE_METAL`, `MATERIAL_TYPE_STONE`, `MATERIAL_TYPE_WOOD`, `MATERIAL_TYPE_DIRT`) during the 6-ray enclosure scan to fine-tune `AL_EAXREVERB_DECAY_HFRATIO` and `AL_EAXREVERB_REFLECTIONS_GAIN`.
- [ ] **Doppler Velocity Vectors for Flying Projectiles**
  - Calculate hex velocity vectors for rockets, plasma bolts, and thrown grenades to drive `AL_VELOCITY` on projectile sound sources.
