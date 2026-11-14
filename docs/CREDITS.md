# Credits

Pizza Legacy is a reimplementation of the 1994 game Pizza Tycoon.

Copyright (C) 2010-2026 Cowomaly <https://pizzalegacy.nl/> (the years in the
source files' SPDX headers)

Pizza Tycoon HD additions (HD rendering, packs, export, pack tools):
Copyright (C) 2026 ShadowByte, under the same GPL-3.0-or-later licence.

## Original game

Pizza Tycoon (released as Pizza Connection in Germany) was created by
Cybernetic Corporation and published by Software 2000 (German) and MicroProse
(English) in 1994. Thanks to the original developers for a game worth
reimplementing three decades later.

Thanks also to [pizzatycoon.org](https://www.pizzatycoon.org/) for keeping the
game's documentation available online.

## Open-source libraries

This project is made possible by the following open-source libraries.
Full license texts are available in the `LICENSES/` directory.

### System dependencies (linked libraries)

#### SDL3

Usage: Platform abstraction (Graphics, Input, Audio, Window management,
native dialogs)

    Copyright: Copyright (c) 1997-2026 Sam Lantinga
    License:   zlib License (see LICENSES/Zlib.txt)

#### libADLMIDI

Usage: Software OPL3 FM synthesis for CMF music

    Copyright: Copyright (c) 2015-2024 Vitaly Novichkov
    Licenses:  LGPL-3.0-or-later (see LICENSES/LGPL-3.0-or-later.txt)
               GPL-3.0-or-later (see LICENSES/GPL-3.0-or-later.txt)

#### zlib

Usage:     Compression for JSON save files

    Copyright: Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler
    License:   zlib License (see LICENSES/zlib-zlib.txt)

### Bundled libraries

Source code for these libraries is included in the `third_party/` directory of
the [source tree](https://codeberg.org/cowomaly/pizzalegacy/).

#### nlohmann/json

Usage: JSON production/consumption for save games

    Copyright: Copyright (c) 2013-2024 Niels Lohmann
    License:   MIT (see LICENSES/MIT-nlohmann.txt)

#### UTF8-CPP

Usage: Handling of UTF-8 encoded strings and conversions

    Copyright: Copyright 2006 Nemanja Trifunovic
    License:   Boost Software License 1.0 (see LICENSES/BSL-1.0.txt)

#### magic\_enum

Usage: String conversion of enums for debugging

    Copyright: Copyright (c) 2019-2024 Daniil Goncharov
    License:   MIT (see LICENSES/MIT-magic-enum.txt)

#### stb\_truetype

Usage: Drawing the HD typefaces (text at HD resolution)

    Copyright: Copyright (c) 2017 Sean Barrett
    License:   MIT or public domain (see LICENSES/MIT-stb.txt)

#### dr\_wav and dr\_mp3

Usage: Decoding the player's own WAV and MP3 files for their music

    Versions:  dr_wav 0.14.6 (tag wav-0.14.6, commit 35b5c392cc15456736c440a68dbe073178bbffe0)
               dr_mp3 0.7.4 (tag mp3-0.7.4, commit aec2dd20667f54acfdb66031217df6d240f56c52)
               of github.com/mackron/dr_libs; hashes in third_party/dr_libs/SHA256SUMS
    Copyright: Copyright 2020 David Reid
    License:   Public domain (Unlicense) or MIT No Attribution
               (see LICENSES/Unlicense-dr-libs.txt)

#### libVLC headers

Usage: Declarations for playing the player's music through an installed
libVLC, which the game loads at runtime when it is present. No libVLC code
is bundled.

    Version:   VLC 3.0.24 (tag 3.0.24, github.com/videolan/vlc), include/vlc:
               libvlc.h, libvlc_media.h, libvlc_media_player.h and
               libvlc_renderer_discoverer.h; hashes in third_party/vlc/SHA256SUMS
    Copyright: Copyright (C) 1998-2015 VLC authors and VideoLAN
    License:   LGPL-2.1-or-later (see LICENSES/LGPL-2.1-or-later.txt)

### Bundled fonts

The HD text uses these typefaces from `data/fonts/`, each under the SIL Open
Font License 1.1 (the license text is next to the fonts and in `LICENSES/`).
Archivo and Crimson Pro are fixed instances of the variable fonts (Archivo at
weight 800 and width 112, Crimson Pro at weight 500).

| Typeface | Stands in for | Copyright | License |
|---|---|---|---|
| Archivo | ASTANDAR | 2020 The Archivo Project Authors | OFL-1.1 (LICENSES/OFL-1.1-archivo.txt) |
| Chakra Petch | SUPERSMA, CSTANDAR, MINI | 2018 The Chakra Petch Project Authors | OFL-1.1 (LICENSES/OFL-1.1-chakrapetch.txt) |
| Crimson Pro | MENU | 2018 The Crimson Pro Project Authors | OFL-1.1 (LICENSES/OFL-1.1-crimsonpro.txt) |
| Fira Sans | PIZ-NUMB | 2012-2015 The Mozilla Foundation and Telefonica S.A. | OFL-1.1 (LICENSES/OFL-1.1-firasans.txt) |
| Zilla Slab | RUBY | 2017 The Mozilla Foundation | OFL-1.1 (LICENSES/OFL-1.1-zillaslab.txt) |

### Adapted code

#### MMPX

Usage: Smoothing of the original art and fonts, ported to work on palette
colors (`src/engine/gfx/Mmpx.cpp`), and its numpy port for RGBA pictures in
the pack tool's `--method mmpx` (`tools/hdpack/pixelart.py`)

    Copyright: Copyright 2020 Morgan McGuire & Mara Gagiu
    License:   MIT (see LICENSES/MIT-mmpx.txt)

## Pizza Tycoon HD art

New art made for Pizza Tycoon HD (commit 3b7f0b6, 2026-11-13): the title
screen `assets/hd/gfx/titel1.png` (3840x2400, replaces GFX/TITEL1.VGA when
HD rendering is on), the box cover `assets/art/PizzaTycoonHD_BoxCover_2880x3840.png`
and their web-size copies `screenshots/cover_*.jpg`. Made on a local GPU
with ComfyUI:

| Model | Used for | Licence |
|---|---|---|
| FLUX.1 [dev] (Black Forest Labs) | generating the pictures | FLUX.1 [dev] Non-Commercial License |
| 4x-UltraSharp | upscaling | CC BY-NC-SA 4.0 |

Both models are under non-commercial licences. Read what each one says
about generated output before any commercial use of this art. The prompts,
seeds and whether the original game's art was an input were not recorded.

The HD art packs (`hdpack.py`, `redraw.py`) are made from the original
game's art and are not part of this repository or its releases.

## Screenshots

`screenshots/citymap.png`, `screenshots/pizzadesign.png` and
`screenshots/hd_compare.png` show the original game's art (c) 1994
Cybernetic Corporation / Software 2000 / MicroProse, for documentation only.
They are not shipped in releases.
