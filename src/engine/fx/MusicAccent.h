/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// The music accent's input, for the audio lane: the only way the picture
// learns how loud the music is. The picture never reads the audio itself
// (no MixTap, no music ring), so it can never disturb it.
//
// Contract for the caller (the audio lane, from its own meter):
//   * level is the music's RMS, normalised to 0..1 (1 = full scale). Values
//     outside are clamped; NaN counts as 0.
//   * Call it from any thread, as often as the meter updates (every audio
//     block is fine). It is one relaxed atomic store: no lock, no
//     allocation, never blocks, safe on the audio callback thread.
//   * Smoothing is the caller's: the picture uses the latest value as is,
//     so a meter with ~100 ms attack and release reads best.
//   * Stop calling, or call set_music_level(0), when the music stops; the
//     last value otherwise stays.
//
// Fed today by the game loop (Game.cpp), once a frame, from
// AudioManager::music_level() (the mixer's own RMS meter). A capture or
// golden that must not depend on the music keeps "Music accent" off (the
// default) or sets the level to 0 first.
//
// What it does (fx::resolve_frame): with the "Music accent" picture option
// on and Ambient motion at Full, at the North Pole, the emissive glow is
// multiplied by 1 + 0.25 * music_accent(level) (at most 1.25x, and never
// past full glow). Nothing else changes, and it never touches game state
// or RNG. Nobody calling it leaves the level 0, and the accent then does
// nothing.
namespace fx {

// The meter's plain RMS (of full scale) on a perceptual scale: -40 dBFS
// and below give 0, -10 dBFS and above give 1, linear in dB between, with
// a soft knee at both ends (smoothstep). Typical music near -20 dBFS (an
// RMS of 0.1) gives about 0.74, a pulse of about 18%; -25 dBFS gives 0.5.
// A pure function of the level: the same level, the same accent.
inline constexpr float music_floor_db = -40.0f;
inline constexpr float music_ceiling_db = -10.0f;
[[nodiscard]] float music_accent(float rms) noexcept;

void set_music_level(float level) noexcept;
// The level last set (0..1), as the picture uses it.
[[nodiscard]] float music_level() noexcept;

} // namespace fx
