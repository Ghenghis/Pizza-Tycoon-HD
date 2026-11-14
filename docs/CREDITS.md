# Credits

Pizza Legacy is a reimplementation of the 1994 game Pizza Tycoon.

Copyright (C) 2026 Cowomaly <https://pizzalegacy.nl/>

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
    License:   zlib License (see LICENSES/zlib.txt)

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
colors (`src/engine/gfx/Mmpx.cpp`)

    Copyright: Copyright 2020 Morgan McGuire & Mara Gagiu
    License:   MIT (see LICENSES/MIT-mmpx.txt)
