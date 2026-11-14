/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// The vendored dr_libs decoders (third_party/dr_libs), declarations only;
// DrLibs.cpp compiles them. They read only through our SDL_IOStream
// callbacks, so their own file functions are left out.
#define DR_WAV_NO_STDIO
#define DR_MP3_NO_STDIO

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <dr_libs/dr_mp3.h>
#include <dr_libs/dr_wav.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
