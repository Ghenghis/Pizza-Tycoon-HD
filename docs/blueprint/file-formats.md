# File formats

Every file the engine reads, with byte layouts as the loaders decode them. Offsets are in
bytes from the start of the file (or record) unless stated otherwise. All multi-byte
integers are little-endian. "Not interpreted" means the engine skips the bytes; their
meaning is unknown.

Conventions used by all loaders:

* Paths are relative to the original game folder set by `fileutils::game_data_path()`
  (`src/resources/util/FileUtils.cpp`). `fileutils::find_file_path()` tries the path as
  given, then without its extension, then (when built with
  `PLEGACY_EMULATE_CASE_INSENSITIVE_FS`) a case-insensitive walk. Paths starting with
  `saves/` resolve to the per-user saves directory instead.
* The engine draws in a 640x400 indexed space. Original pictures are 320x200-based and are
  doubled when they are loaded (`gfx::asset_pixel_scale = 2`, `src/engine/gfx/Surface.h`).
  "Engine pixels" below means after doubling.
* Colour index 0 is transparent (`gfx::TRANSPARENT`, `src/engine/gfx/Surface.h`).
* Palettes are not stored in picture files. The palette of each VGA/BOB file comes from the
  table in `src/resources/AssetMetadata.cpp` (see [Palette table](#3-palette-assignment-table)).
* Text is not copied into this document. Only structure is described.

Contents:

1. [Original game files](#1-original-game-files)
   * [Directory map](#11-directory-map)
   * [Palette file `gfx/palette.e`](#12-palette-file-gfxpalettee)
   * [VGA pictures `*.vga`](#13-vga-pictures-vga)
   * [Blitmap offset tables `gfx/offsetsN`](#14-blitmap-offset-tables-gfxoffsetsn)
   * [BOB sprite sheets `*.bob`](#15-bob-sprite-sheets-bob)
   * [FNT bitmap fonts `fonts/*.fnt`](#16-fnt-bitmap-fonts-fontsfnt)
   * [M audio containers `sound/mN`](#17-m-audio-containers-soundmn)
   * [City data `dat/cityN.pc`](#18-city-data-datcitynpc)
   * [City map `dat/mapN.dat`](#19-city-map-datmapndat)
   * [District grid `dat/bezN.dat`](#110-district-grid-datbezndat)
   * [Character files `chr/cNe.pc` and `chr/amarkt`](#111-character-files-chrcnepc-and-chramarkt)
   * [Recipe files `r/rN.pc` and PREZ recipes](#112-recipe-files-rrnpc-and-prez-recipes)
   * [Restaurant layouts `f/fN.pc` and `q/fN.pc`](#113-restaurant-layouts-ffnpc-and-qfnpc)
   * [Floor pattern table `d/kachinfo.pc`](#114-floor-pattern-table-dkachinfopc)
   * [Mission files `m/N.mis`](#115-mission-files-mnmis)
   * [Text files `t/*.e`, `z/artikel*.e`](#116-text-files)
   * [Original save games (`.GAM`)](#117-original-save-games-gam)
2. [Engine formats](#2-engine-formats)
   * [`data/*.json` content tables](#21-datajson-content-tables)
   * [`data/fonts/hd_fonts.json`](#22-datafontshd_fontsjson)
   * [`data/game_location.vga`](#23-datagame_locationvga)
   * [Save games `.plsave`](#24-save-games-plsave)
   * [High scores `highscore.dat`](#25-high-scores-highscoredat)
   * [Mission rules trailer `PLRS`](#26-mission-rules-trailer-plrs)
   * [HD pack layout](#27-hd-pack-layout)
   * [Asset export layout and `manifest.json`](#28-asset-export-layout-and-manifestjson)
   * [HD warm list `hd_warm.txt`](#29-hd-warm-list-hd_warmtxt)
   * [`settings.conf` and `bindings.conf`](#210-settingsconf-and-bindingsconf)
   * [Writers](#211-writers)
3. [Palette assignment table](#3-palette-assignment-table)
4. [Unknowns](#4-unknowns)

---

## 1. Original game files

### 1.1 Directory map

The engine requires the English DOS release. `GameLocationPrompt` looks for `PT.EXE`
(`src/engine/GameLocationPrompt.cpp`). Folder names below are the lower-case paths the code
asks for.

| Path | Format | Read by |
|---|---|---|
| `gfx/palette.e` | 71 VGA palettes | `resources::load_palette_bank()` (`src/resources/PaletteLoader.cpp`) |
| `gfx/*.vga` | VGA RLE picture | `VgaLoader::load()` (`src/resources/VgaLoader.cpp`) |
| `gfx/menu.f`, `gfx/bo.f`, `euromap.f`, `gfx/permap.i`, `gfx/icons.96` | listed in the palette table only; no game code loads them and `--export-assets` skips them (it takes `.vga` and `.bob` only); format not verified | `AssetMetadata.cpp` |
| `z/zb1.vga`..`z/zb20.vga` | newspaper pictures (VGA); palette found through the base-name fallback `gfx/zbN` | `Newspaper` (`src/views/screens/Newspaper.cpp`) |
| `gfx/offsets1`..`gfx/offsets4` | icon rectangles for `gfx/blitmap1..4` | `Blitmap<>::Blitmap()` (`src/resources/Blitmap.inl`) |
| `bob/*.bob`, `gfx/menubobs.bob` | BOB sprite sheet | `BobLoader::load()` (`src/resources/BobLoader.cpp`) |
| `fonts/*.fnt` | bitmap font | `FontLoader::load()` (`src/resources/FontLoader.cpp`) |
| `sound/m0`..`sound/m11` | CMF/VOC container | `MFileLoader::load()` (`src/resources/MFileLoader.cpp`), `AudioManager::load_m_container()` (`src/engine/audio/AudioManager.cpp`) |
| `dat/city0.pc`..`dat/city9.pc` | city data | `CityFactory::load_city_data()` (`src/core/factories/CityFactory.cpp`) |
| `dat/map0.dat`..`dat/map9.dat` | 160x120 tile map | `CityFactory::load_map_data()` |
| `dat/bez0.dat`..`dat/bez9.dat` | 20x15 district grid | `CityFactory::load_district_bez()` |
| `chr/c0e.pc`..`chr/c99e.pc` | character portrait and stats | `CharacterFactory::create_original()` (`src/core/factories/CharacterFactory.cpp`) |
| `chr/amarkt` | job role per character | `JobMarket::init()` (`src/core/city/JobMarket.cpp`) |
| `r/r0.pc`..`r/r13.pc` | standard pizza references | `PizzaFactory::create_pizza(StandardPizza, ...)` (`src/core/factories/PizzaFactory.cpp`) |
| `r/r20.pc`..`r/r69.pc` | competition reference pizzas | `PizzaFactory::create_competition_pizza()`; index from `CompetitionCoordinator::reference_pizza_index()` |
| `f/f1.pc`..`f/f24.pc` | prebuilt AI restaurants | `RestaurantLayoutFactory::create_ai_layout()` |
| `q/f1.pc`..`q/f8.pc` | quick-start restaurants | `RealEstate::create_restaurant_with_layout()` (`src/core/state/RealEstate.cpp`) |
| `d/kachinfo.pc` | floor patterns | `FloorPatternFactory` (`src/core/factories/FloorPatternFactory.cpp`) |
| `m/*.mis` | built-in missions | `core::mission::read_mission_file()` (`src/core/mission/MissionFile.cpp`) |
| `t/*.e`, `Z/ARTIKEL0.E`, `Z/ARTIKEL1.E`, `T/ARTIKEL3.E` (upper case in the code) | text | see [1.16](#116-text-files) |

### 1.2 Palette file `gfx/palette.e`

| Offset | Size | Content |
|---|---|---|
| 0 | 71 x 768 = 54 528 | 71 palettes; each 256 colours of 3 bytes (R, G, B) |

* Each component is a 6-bit VGA DAC value (0-63). `PaletteBank::load_dos_palette()`
  (`src/engine/gfx/PaletteBank.h`) shifts each byte left by 2 to get 8-bit colour.
* The file must be exactly 54 528 bytes or loading fails (`fatal("palette.e has the wrong
  size")`).
* Palette N is selected by a `gfx::PaletteHandle{N}`; which palette a picture uses comes from
  `AssetMetadataRegistry` ([palette assignment table](#3-palette-assignment-table)).
* Before the file is loaded the engine uses a placeholder palette 0 with white at 255 and the
  cursor reds at 17-19 (`resources::load_fallback_palette()`).

### 1.3 VGA pictures `*.vga`

Loader: `VgaLoader::load(key)` appends `.vga` to the key (for example `gfx/back1` reads
`gfx/back1.vga`); if that file does not exist, `find_file_path` retries without the last
extension, so a key such as `gfx/menu.f` would read `gfx/menu.f`. Keys containing
`euromap` without a `gfx/` prefix get `gfx/` prepended.

Header (`VGA_HEADER_SIZE = 8`, `src/resources/Vga.h`):

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | width in original pixels (1..320) |
| 2 | u16 | height in original pixels (1..320) |
| 4 | u16 | data size: body length + 1 (the threshold byte). Written by `VgaWriter`; not read by the loader. |
| 6 | u16 | 0 (written by `VgaWriter`; not read) |
| 8 | ... | RLE body |
| file size - 1 | u8 | `rle_threshold + 1` |

RLE decoding (`VgaLoader::load`), with `T = last_byte - 1`:

```
pos = 8
while pos < file_size - 1:
    c = byte[pos++]
    if c > T:                       # run
        v = byte[pos++]
        emit v, (c - T) times
    else:                           # literal
        copy the next (c + 1) bytes
```

* The decoded pixel count must equal `width * height`, otherwise loading is fatal.
* The picture is one byte per pixel, row-major, palette indices.
* Width and height up to 320 are accepted because some files are atlases larger than the
  screen in one direction (comment in `VgaLoader::load`).
* After decoding, the surface is doubled to engine size with the palette from the metadata
  table and, if an HD pack is active, the matching PNG is attached (`HdPack::vga()`).

Atlases: many VGA files hold several pictures cut by rectangles defined in code. Known cutters:

| File | Cell layout | Cut by |
|---|---|---|
| `gfx/landsym` (256x256) | city tiles, 16 per row; tile id high nibble = column, low nibble = row; 16x16 original pixels per tile | `landsym_source_rect()` (`src/core/util/Constants.h`) with multiplier `BASE_MAP_ZOOM_MULTIPLIER = 4` |
| `gfx/med1` (64x64) | same tile ids at 4x4 original pixels | `landsym_source_rect(type)` in `CityView` (`src/views/screens/city_view/CityView.cpp`) |
| `gfx/icons` (256x78) | 16 x 6 icons of 32x26 engine pixels | `icon_rect()` and `enum class Icon` (`src/views/ui/Icon.h`) |
| `gfx/blitmap1..4` | 30 rectangles each | `gfx/offsets1..4` (1.4) and `Blitmap1Icon..Blitmap4Icon` (`src/resources/BlitmapIcons.h`) |
| `gfx/k1..k13` (96x22) | floor tile strip: tile `i` at x = i * 24 engine pixels; row 0 lit, row 1 (y = 22) "shadow" variant | `RestaurantView` floor drawing (`src/views/screens/restaurant/RestaurantView.cpp`) |
| `gfx/k0` (96x11) | kitchen floor; tile at `FLOOR_TILE_OFFSET = 2` | same |
| `gfx/spot0..8` | TV commercial frames 148x100 engine pixels, 4 per row | `TVPlaybackEngine::next_frame()` (`src/views/helpers/TVPlaybackEngine.cpp`) |
| `gfx/tvmisc` | static / intro frames, 3 per row | same |
| `gfx/g0..g23` (64x46) | guest faces | `GuestTalkPopup` (`src/views/ui/popups/GuestTalkPopup.cpp`) |

### 1.4 Blitmap offset tables `gfx/offsetsN`

Read by `Blitmap<IconEnum>::Blitmap()` (`src/resources/Blitmap.inl`) together with
`gfx/blitmapN.vga`. `BlitmapManager` loads N = 1..4 (`src/resources/BlitmapManager.cpp`).

| Offset | Type | Field |
|---|---|---|
| 4*i | u32 | rectangle `i` (i = 0..29) |

Each u32 packs four bytes, low byte first: `x1, y1, x2, y2` in original pixels, corners
inclusive. The engine converts to engine pixels as `x = min(x1,x2)*2`, `w = |x2*2+1 - x1*2| + 1`
(same for y). A short file keeps the rectangles read so far. Icon names per index are in
`src/resources/BlitmapIcons.h`.

### 1.5 BOB sprite sheets `*.bob`

Loader: `BobLoader::load()` (`src/resources/BobLoader.cpp`); frames are cut by `Bob::frame()`
(`src/resources/Bob.cpp`).

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | frame count `n` |
| 2 | 4 bytes | not interpreted |
| 6 + 7*i | u8 | frame `i` width (original pixels) |
| 7 + 7*i | u8 | frame `i` height |
| 8 + 7*i | 5 bytes | not interpreted (named `padding1, u2, u3, u4, padding2`) |
| 6 + 7*n | ... | pixel data |

* Pixel data is the frames back to back, frame 0 first, each `width * height` bytes,
  row-major, one palette index per byte, uncompressed. Frame `i` starts at
  `6 + 7*n + sum(w_j * h_j for j < i)`.
* Frames with width or height 0 exist; they are empty.
* `Bob::width()`/`height()` report engine pixels (twice the stored size).
* A palette can be forced per call (`Bob::frame(index, palette)`), otherwise it comes from the
  metadata table.
* `FrameInfo::num_bitplanes` exists for Amiga files; the DOS loader always uses 8-bit pixels.

Sheets used by the game (`src/resources/AssetMetadata.cpp`, view sources):

| Sheet | Content | Used by |
|---|---|---|
| `bob/moebel.bob` | dining-room furniture, doors, pizza on table (frame 0x154) | `RestaurantView`, `FurnitureListView`, `FloorListView` |
| `bob/kuebobs.bob` | kitchen equipment (category KITCHENWARE) | `RestaurantView::bob_renderer_for()` |
| `bob/zutaten.bob` | pizza ingredients | `PizzaDesign`, `CookBookPopup`, `RecipePreview`, `core::food::topping_size_fn()` |
| `bob/gaeste.bob` | seated guests and staff | `CharacterRenderer`, `SeatedCharacterRenderer` |
| `bob/walk{N}.bob` | walking guests by guest type | `GuestWalkScene` |
| `bob/typ{N}e.bob` | actor heads (talking, angry, happy frames) | `Actor::bob_filename()` (`src/core/character/Actor.cpp`) |
| `bob/nightp.bob`, `bob/nightw0..3.bob` | night attack scene | `AttackScene` |
| `bob/stanbobs.bob`, `bob/stabo.bob`, `bob/lebbo.bob` | UI widgets, chart bars, animated stats | stats screens, `NumberInputPopup`, `CityView` |
| `bob/menubobs.bob`, `gfx/menubobs.bob` | menu arrows | `Menu`, `ArrowMenuItem` |
| `bob/kus.bob`, `bob/mens.bob`, `bob/stas.bob` | listed in the metadata table only | no code reference found |

### 1.6 FNT bitmap fonts `fonts/*.fnt`

Loader: `FontLoader::load()` (`src/resources/FontLoader.cpp`). Header struct `FontHeader`
(`src/resources/FontHeader.h`, packed, 24 bytes):

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | `character_height` (rows) |
| 2 | u8 | padding |
| 3 | u8 | `fixed_width_font_flag`: `0x02` fixed width, any other value (seen: `0x62`) proportional |
| 4 | u16 | `character_width` (advance of a fixed-width font) |
| 6 | u16 | `baseline_height` (not used by the loader) |
| 8 | u8 | `first_character` |
| 9 | u8 | `last_character` |
| 10 | u8 | `start_at_bit` (not used) |
| 11 | 3 x u8 | not interpreted |
| 14 | u16 | `number_of_bytes_per_row` |
| 16 | u16 | constant, not interpreted |
| 18 | 4 x u8 | not interpreted |
| 22 | u16 | constant, not interpreted |

Body, starting at 24:

1. Bitmap: `number_of_bytes_per_row * character_height` bytes. One bit per pixel, most
   significant bit first; all glyphs side by side in one strip; row stride
   `number_of_bytes_per_row`.
2. Position table: `E = last_character - first_character + 2` records of
   `{u16 start_pos, u16 width}` (`CharacterPosition`), in pixels along the strip. The last
   record is for characters outside the range and is not used.
3. Proportional fonts only: `E` x u16 advance widths.

Glyph `c` (first..last) uses record `c - first_character`. Fonts
(`FontManager::font_map_`, `src/resources/FontManager.cpp`): `ASTANDAR`, `CSTANDAR`, `MENU`,
`PIZ-NUMB`, `RUBY`, `SUPERSMA`. `MINI` is `SUPERSMA.fnt` drawn at scale 1 instead of 2
(`FontLoader::load_file_bytes`, `render_scale`). Text strings are decoded as UTF-8 at render
time; `|` is a line break (`Font::render_text_onto`, `src/resources/Font.cpp`).

### 1.7 M audio containers `sound/mN`

Container (`MFileLoader::load`):

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | entry count `n` |
| 4 + 12*i | u32 | type: 1 = CMF music, 2 = VOC sample; other values are skipped with a warning |
| 8 + 12*i | u32 | absolute offset of the entry data |
| 12 + 12*i | u32 | length of the entry data |

CMF entries are passed whole to libADLMIDI (OPL3 emulation) when built with
`PLEGACY_ENABLE_MUSIC` (`AudioManager`). The engine does not parse CMF itself.

VOC entries (`VocDecoder::decode`, `src/engine/audio/VocDecoder.cpp`):

| Offset | Type | Field |
|---|---|---|
| 0 | 20 bytes | `"Creative Voice File"` followed by `0x1A` |
| 20 | u16 | offset of the first data block |
| block | u8 | block type: 0 = end, 1 = sound data; other types are skipped |
| block+1 | u24 | block length |
| (type 1) +4 | u8 | rate code; sample rate = 1 000 000 / (256 - code) |
| (type 1) +5 | u8 | compression; only 0 (8-bit unsigned PCM) is supported |
| (type 1) +6 | ... | mono unsigned 8-bit samples |

Samples are converted to signed 16-bit stereo and linearly resampled to 44 100 Hz.

Slot assignment is by load order, not by name. `AudioManager::load_default_assets()` loads
`sound/m0` .. `sound/m11` in order; every CMF fills the next `MusicId` slot and every VOC the
next `SoundId` slot (`src/engine/audio/AudioManager.h`):

| File | Expected content (from the enum comments) |
|---|---|
| m0 | VOC: restaurant noise x3 (`RESTAURANT_NOISE_1..3`) |
| m1..m6 | CMF: six songs (`MusicId` 0..5); also the jukebox stations |
| m7 | VOC: traffic x2 |
| m8 | VOC: dial tones 0-9, ringing, pick-up |
| m9 | VOC: helicopter (`CHOPPER`; playback starts at index 500 of the converted 16-bit stereo buffer to skip noise) |
| m10 | CMF x2 (break-in music 1, 2), then VOC x5 (weapons 1-4, siren) |
| m11 | CMF: break-in music 3 |

If a container holds a different number of entries, every later `SoundId` shifts.

### 1.8 City data `dat/cityN.pc`

N = city id 0..9. The engine's city names come from `core::city_names`
(`src/core/city/CityNames.h`), not from this file. Layout as read by
`CityFactory::load_city_data()` and helpers (`src/core/factories/CityFactory.cpp`):

| Offset | Size | Content | Reader |
|---|---|---|---|
| 0 | 8 | not interpreted | |
| 8 | u8 | district count `D` | `load_city_data` |
| 9 | u8 | food supplier count `S` | `load_city_data` |
| 10 | 30 | not interpreted | |
| 40 | u8 | realtor count | `load_city_data` |
| 41 | u8 | bank count | |
| 42 | u8 | insurance count | |
| 43 | u8 | authorities count (mayor + police) | |
| 44 | u8 | "others" count (mafia clans + arms dealers) | |
| 45 | u8 | not interpreted | |
| 46 | 50 | head ids, `[type*10 + j]`; head used = value + 1 (`bob/typ{head}e.bob`) | |
| 96 | 10 | skipped | |
| 106 | 200 | phone numbers: 5 types x 10 x u32, encoded (below); 0 = none | `parse_phone_numbers` |
| 306 | 40 | skipped | |
| 346 | 1200 | contact names: 5 types x 10 x 24 bytes ISO-8859-1, NUL-padded | `parse_contacts` |
| 1546 | 240 | skipped (comment: zero bytes) | |
| 1786 | 4 x realtors | realtor settings `{u8, u8, rip_off_chance, pad}` | `read_realtor_data` (absolute seek) |
| 1826 | 4 x banks | bank settings `{interest_rate_modifier, loan_amount_multiplier, unused, pad}` | `read_bank_data` |
| 1866 | 4 x insurers | insurance settings `{premium_factor, willingness_threshold, rejection_percentage, pad}` | `read_insurance_data` |
| 1946 | 4 x others | mafia settings `{crime_danger_level, u8, u8, u8}` | `read_mafia_data` |
| 2026 | 96 x u16 | ingredient base prices in cents, indexed by ingredient id (0..95) | `load_ingredient_prices` (absolute seek) |
| 2218 | 330 | not interpreted (the sequential reader skips 2026..2547) | |
| 2548 | 104 x D | district records (below) | `load_districts` |
| ... | 319 x S - 10 | food supplier records (below), then 2 bytes skipped | `load_suppliers` |
| ... | 3072 | building names: 8 building types x 24 names x 16 bytes ISO-8859-1 | `load_building_names` |

Actor types per index (`build_city_object`): 0 realtors, 1 banks, 2 insurance, 3 authorities
(entry 0 = mayor, later entries = police), 4 others (entries 0-1 = mafia clans, later = arms
dealers). Actors get consecutive `ActorId`s in this order.

Phone number encoding: the loader decodes each non-zero u32 `T` with
`invert_transform_phone_number`: `s = ~(T - 1) >> 2`, then `N = (s / 1 000 001) * 1 000 000 +
s % 1 000 001`, aborting if the remainder is 1 000 000 or more (32-bit arithmetic). In other
words the file stores `T = ~((N + N / 1 000 000) << 2) + 1`. The forward function
`transform_phone_number` (commented "Reproduces the original's phone-number transform")
additionally subtracts 15 000 000; it is not called anywhere, so the role of that constant is
not known.

District record (`DistrictDataRaw`, 104 bytes, static_assert in `CityFactory.cpp`):

| Offset | Type | Field | Use (`src/core/city/District.h`) |
|---|---|---|---|
| 0 | u8 | traffic_intensity | `traffic_density()` |
| 1 | u8 | id (usually index + 1) | `District::id()`; police investigation uses it |
| 2 | char[24] | name, ISO-8859-1 | `name()` |
| 26 | 4 | not interpreted | |
| 30 | u8 | control screen alternate colour (1 = yes) | `control_screen_alternate_color()` |
| 31 | 9 | not interpreted | |
| 40 | u8 | `demographic_something`, not used | |
| 41 | 7 | not interpreted | |
| 48 | u8 | demographic_factor | `PizzaDemand::calculate` |
| 49 | u8 | crime_stats | `crime_statistics()` |
| 50 | u8 | demographic_base | `PizzaDemand::calculate` |
| 51 | u8 | average_income_factor; income = 9000 - factor * 80 | `PizzaDemand::calculate_price_income_modifier`, `average_income()` |
| 52 | u8 | service_speed_sensitivity | `Restaurant::calculate_popularity` (waiting time) |
| 53 | u8 | not used | |
| 54 | u8 | minimum desired standard pizzas x 10 (divided by 10 on load) | popularity |
| 55 | u8 | minimum desired user pizzas << 4 (shifted right 4 on load) | popularity |
| 56 | u8 | not interpreted | |
| 57 | u8 | district_type (`DistrictType`: 1 residential, 2 industrial, 3 business, 4 leisure, 5 shopping, 6 traffic) | display |
| 58 | u16 | average premise size | `CityFactory::populate_properties` |
| 60 | u16 | average size modifier | same |
| 62 | u16 | price level (dollars per tile) | purchase price |
| 64 | u16 | rent level (dollars per tile per month) | `Property::get_monthly_rent` |
| 66 | 4 | not interpreted | |
| 70 | u8 | premises_availability (percent) | premise status rolls |
| 71 | u8 | not interpreted | |
| 72 | 6 x f32 | population weights for the 6 consumer types | `Property::calculate_demographics` |
| 96 | 8 | not interpreted | |

Food supplier record (309 bytes read; records after the first are preceded by 10 skipped
bytes, so the stride is 319):

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | category: 0 vegetables, 1 meat, 2 fruit, 3 cheese; 4-7 exist in the data and are skipped (comment: cut content) |
| 1 | u8 | quality |
| 2 | u8 | supplier id |
| 3 | u8 | price_factor (used in `FoodSupplier::daily_update`) |
| 4 | char[26] | name, ISO-8859-1 |
| 30 | u16 | minimum contract amount, dollars |
| 32 | u32 | loyalty level amount, dollars |
| 36 | 197 | not interpreted |
| 233 | u8 | head id; head used = value + 1 |
| 234 | 5 | not interpreted |
| 239 | 22 | price level per product within the category; 0 = not sold. The original reserves 24; the engine reads 22 |
| 261 | 48 | per-product stock, zero in all files (the engine randomises stock) |

Building names: the 8 building types are, in order, church, bank, cinema, school, disco, bar,
department store, hotel (`City::building_type_name`). Each named-building tile on the map is
given the next name of its type in map order, wrapping at the count of non-empty names
(`load_map_data`).

### 1.9 City map `dat/mapN.dat`

One byte per tile, row-major, `MAP_TILES_PER_ROW = 160` by `MAP_TILES_PER_COLUMN = 120`
(`src/core/util/Constants.h`), 19 200 bytes. The loader reads the whole file and does not
check its size. Tile index = `y * 160 + x`.

The byte is a cell in `gfx/landsym`: high nibble = atlas column, low nibble = atlas row.
Meaning (`CityFactory::populate_types`, `City::*` in `src/core/city/City.cpp`):

| Value | Meaning |
|---|---|
| low nibble `0xA` (any high nibble) | business premise (a `Property` is created). Owned premises are drawn as `((owner & 0xF) << 4) \| 0xA` (`City::type`) |
| `0x04` | depot premise (`PropertyType::DEPOT`) |
| `0x05` | depot building (`City::building_type`), no property |
| low nibble `0x8`, high nibble `0x8..0xF` | named building; type = high nibble - 8 (church .. hotel) |
| low nibble `0x6` or `0x7`, except `0xE6`, `0xF6` | road (`City::is_road_tile`); variants in `enum class RoadType` (`src/core/city/CityTypes.h`) |
| low nibble `0x0` | nature |
| low nibble `0x3` | water |
| low nibble `0x4` | building |
| low nibble `0x5` | path |
| low nibble `0x9` | villa |
| low nibble `0xB` | residential building |

Population weights per tile for the catchment area are in `Property::calculate_tile_population`
(`src/core/city/Property.cpp`). With `fix_legacy_bugs`, tile 11589 of `map1.dat` is replaced
by its right neighbour (a stray shop front).

### 1.10 District grid `dat/bezN.dat`

300 bytes, `NUMBER_OF_DISTRICT_CELL_COLUMNS = 20` by `NUMBER_OF_DISTRICT_CELL_ROWS = 15`
cells, row-major; each cell covers 8x8 map tiles. The low nibble is the index into the
district list of `cityN.pc` (`City::district_by_district_index`). The high nibble is not read;
`District::bez_id()` computes `0x80 + index (+ 0x10 for the alternate colour)`, which suggests
the file stores those values, but no code reads it.

### 1.11 Character files `chr/cNe.pc` and `chr/amarkt`

`chr/cNe.pc`, N = 0..99 (`JobMarket::ORIGINAL_SLOTS = 100`). Packed struct
`CharacterFileDOS` (`src/core/factories/CharacterFactory.cpp`), 3177 bytes read:

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | file index (warned if it differs from N) |
| 1 | u8 | not interpreted |
| 2 | 3 | padding |
| 5 | 3068 | portrait, 59x52 indexed pixels |
| 3073 | char[16] | first name |
| 3089 | char[16] | last name |
| 3105 | 9 | not interpreted |
| 3114 | u8 | not interpreted (`field_28`) |
| 3115 | u8 | not interpreted |
| 3116 | u8 | `pregnancy_possible` (read, not used by `populate_character_data`) |
| 3117 | 22 | not interpreted |
| 3139 | 10 | starting money / salary: first 8 bytes are an IEEE double (dollars) |
| 3149 | u8 | pizzas per day |
| 3150 | 5 | not interpreted |
| 3155 | 15 x u8 | age, popularity, health, energy, intelligence, charisma, dexterity, personality, courage, experience, trustworthiness, honesty, cooking, serving, management |
| 3170 | 2 | not interpreted |
| 3172 | u8 | motivation |
| 3173 | 4 | not interpreted |

Characters 100 and up are "doubles": portrait and stats of character `(id - 100) % 100`, names
from line pairs of `t/double.e`, with stats perturbed from the name letters
(`CharacterFactory::create_double`).

`chr/amarkt`: u8 count, then `count` bytes of job role (0 chef, 1 waiter, 2 manager; larger
values are clamped to 2) for characters 0..count-1.

### 1.12 Recipe files `r/rN.pc` and PREZ recipes

Both kinds wrap the original's in-memory recipe record. Record offsets
(`core::food::recipe_node`, `src/core/food/RecipeNode.h`):

| Record offset | Type | Field |
|---|---|---|
| 8 | 7 x u16 | daily sales history, oldest first |
| 23 | u8 | recipe uid (< 14 = standard; user recipes count from 50) |
| 24 | char[30] | name, NUL-terminated |
| 54 | u8 | judged flag |
| 65 | u8 | imitation (0..100) |
| 68 | 4 x u8 | primary ingredient ids, strongest first, 255 = unused |
| 72 | 4 x u16 | fat, sweet, salty, hot (0..10) |
| 80 | u8 | appearance |
| 81 | u8 | refinement |
| 82 | u8 | normality |
| 83 | u8 | herb score |
| 84 | 6 x u8 | popularity per consumer type |
| 90 | u16 | calories |
| 98 | 96 x u16 | grams per ingredient id |
| 290 | u8 | distinct ingredient count |
| 291 | u8 | placement count `P` |
| 292 | 4 x P | placements `{ingredient_id, state, x, y}` |
| 292 + 4P | 4 | zero terminator |

Placement fields: `state` = `rotation (0..3) | chop << 2` with chop 0 whole, 1 chopped,
2 minced (`Topping`, `src/core/food/Topping.cpp`); weight per chop level comes from
`ingredients.json`. `x, y` are the top-left corner of the ingredient sprite in original
(320x200) screen pixels. The engine converts them to engine pixels relative to the wooden
border, `top_left = (x*2 - 24, y*2 - 28)`, and stores the sprite centre
(`make_topping` in `PizzaFactory.cpp`; `WOOD_BORDER_WIDTH_LEFT = 24`,
`WOOD_BORDER_HEIGHT_TOP = 28` in `src/core/util/LegacyConstants.h`). `save_prez` reverses this
with rounding to the nearest grid step.

File wrappers:

| Kind | Header | Record starts at | Notes |
|---|---|---|---|
| Reference file (`r/rN.pc`) | 4 bytes, not interpreted | 4 | For N 0..13 the name in the file is ignored; the engine uses `standard_pizza_names` (`src/core/food/StandardPizza.h`). Stats are read from the file (`set_disk_stats`, imitation forced to 100). |
| PREZ file (user recipe, `<user dir>/recipes/*.pc`) | `"PREZ"`, u32 checksum, u32 record length | 12 | Checksum = byte sum of the record modulo 2^32 (`math::sum_bytes`). The reader does not verify checksum or length. |

The reader (`read_pc_file`) takes the name at 28 (+8 for PREZ), stats at 76 (+8), counts at
294 (+8), and warns if the 4 bytes after the placements are not zero. `read_summary` is used
for the import preview.

### 1.13 Restaurant layouts `f/fN.pc` and `q/fN.pc`

`f/f1.pc`..`f/f24.pc` are the AI restaurants (`ai_layout_count = 24`), `q/f1.pc`..`q/f8.pc`
the quick-start restaurants (odd = big, even = small; `GameState::finalize_initialization`).
Parser: `RestaurantLayoutFactory::create_layout(span)` (`src/core/factories/RestaurantLayoutFactory.cpp`).

Header (41 bytes):

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | dining room width (tiles) |
| 1 | u8 | dining room height |
| 2 | u8 | kitchen width |
| 3 | u8 | kitchen height |
| 4 | u8 | entrance door offset |
| 5 | u8 | kitchen entrance offset |
| 6 | 3 | not interpreted |
| 9 | u8 | floor vendor = supplier id + 1 (`SupplierID`) |
| 10 | 4 x u8 | floor tile pattern (top-left, top-right, bottom-left, bottom-right) |
| 14 | 6 x i8 | floor popularity per consumer type (read, then replaced by the `kachinfo` entry when one matches) |
| 20 | 19 | not interpreted |
| 39 | u8 | furniture record count `F` |
| 40 | u8 | oven record count `K` |

Then `F` dining-room records and `K` kitchen records, 9 bytes each:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | furniture type id (`FurnitureType`, index into `furniture.json`) |
| 1 | u8 | rotation: 240 + r (r = 0..3); other values mean unrotated |
| 2 | 3 | not interpreted |
| 5 | u16 | x / 2 |
| 7 | u8 | y / 2 (the original biases y) |
| 8 | u8 | linked furniture index (chair to table); 0xFF = none |

Coordinates are converted with `restaurant_point_from_legacy()`
(`src/core/restaurant/RestaurantLayout.cpp`) for the dining room and an inline
right/bottom-aligned transform for the kitchen. `create_ai_layout(index, count)` keeps only
the first `count` furniture records and all oven records; AI restaurants grow by adding more
records later (`Restaurant::ai_layout`).

### 1.14 Floor pattern table `d/kachinfo.pc`

`FloorPatternFactory::parse_and_index()`:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | record count `n` |
| 1 + 6*i | u8 | vendor: `SupplierID` + 1 |
| 2 + 6*i | 4 x u8 | tile indices + 1 (each must decode to 0..15) |
| 6 + 6*i | u8 | popularity factor |

The file size must be exactly `1 + 6n`. Each record becomes a `FloorPattern` with the
vendor's quality, price per tile and customer ratings from `suppliers.json`;
`popularity_for(type) = rating[type] * factor / 2` (`src/core/restaurant/FloorPattern.h`).

### 1.15 Mission files `m/N.mis`

`core::mission::read_mission_file()` (`src/core/mission/MissionFile.cpp`). 400 bytes, four
column blocks of 100 slots:

| Offset | Size | Field |
|---|---|---|
| 0 | 100 | milestone (event) id per slot (`core::events::MilestoneId`) |
| 100 | 100 | day |
| 200 | 100 | month |
| 300 | 100 | year - 1900; 0 = empty slot |

Slots with invalid ids (0, >140, 28, 29, 93, 98, 99) or dates are skipped; a file with no
valid milestone fails to load. With `fix_legacy_bugs`, missions dated before 1995 are shifted
to start in the game start year (`shift_mission_to_game_start`). Bytes past 400 may hold the
engine's rules trailer ([2.6](#26-mission-rules-trailer-plrs)).

### 1.16 Text files

Plain text, one entry per line, CR stripped (`fileutils::load_text_lines`). No transcoding is
applied; the renderer decodes UTF-8 and shows malformed bytes as U+FFFD. Line numbers are
the keys: conversation code indexes lines by its `*Line` enum value
(`BaseConversation::make_line`, `src/core/conversations/BaseConversation.h`).

| File | Structure | Reader |
|---|---|---|
| `t/bank.e` | conversation lines, index = `BankLine` | `BankConversation` |
| `t/versich.e` | `InsuranceLine` | `InsuranceConversation` |
| `t/makler.e` | `RealtorLine` | `RealtorConversation` |
| `t/mafia.e` | `MafiaLine`; also mission descriptions | `MafiaConversation`, `MafiaMissionView` |
| `t/polizei.e` | `PoliceLine` | `PoliceConversation` |
| `t/waffen.e` | `ArmsDealerLine` | `ArmsDealerConversation` |
| `t/lief.e` | `SupplierLine` | `SupplierConversation` |
| `t/bg.e` | `MayorLine` | `MayorConversation` |
| `t/bewerb{bucket}.e` | job application texts, two lines per applicant (1-based) | `StaffView` |
| `t/pers.e`, `t/persf.e` | staff event dialogs | `StaffEventDialogs` (`src/core/restaurant/StaffEventDialogs.cpp`) |
| `t/gtalk0.e`, `t/gtalk1.e` | guest remarks (informal / formal) | `GuestTalkPopup` |
| `t/leben.e` | CV label per milestone id (lines 1..139) | `CareerLog`, `PlayerProfile` |
| `t/levent.e` | full-screen milestone text (up to 97) | `CareerLog` |
| `t/markt.e` | pizza chart texts; lines 51-149 are the AI pizza names | `PizzaCharts` (`ai_pizza_names`, `src/core/city/PizzaCharts.h`) |
| `t/double.e` | first/last name pairs for characters 100+ | `CharacterFactory`, `JobMarket` |
| `t/werbung.e` | newspaper advert mini-game: 5 blocks of 28 fragment lines, each block followed by 2 blank lines (`BLOCK_LINES`, `BLOCK_GAP`); 4 ad-size labels from line 150 (`SIZE_LABEL_BASE_LINE`); `#` is replaced by the company name | `NewspaperAdGame` (`src/core/marketing/NewspaperAdGame.h`) |
| `t/action.e` | action texts | `ActionText` (`src/core/item/ActionText.cpp`) |
| `t/credits.e` | intro credits | `Intro` |
| `t/ende.e` | win sequence text (empty lines skipped) | `WinSequenceView` |
| `Z/ARTIKEL0.E`, `Z/ARTIKEL1.E` | up to 50 articles each, read into a 15 000-byte buffer: `0xA7` title `!` then `$` body, body until the next `0xA7`; line breaks in the body become spaces | `ArticleDatabase::parse_article_file` |
| `T/ARTIKEL3.E` | alternating title / body template lines; `#` is replaced by an ingredient name; 8 templates = 4 categories x (positive, negative) | `ArticleDatabase::parse_ingredient_templates` |

Article picture numbers (0 = none, otherwise `z/zbN` / `gfx/zbN`) are a table in code
(`flavor_image_map_`, `event_image_map_` in `src/core/newspaper/ArticleDatabase.h`), not in the
text files.

### 1.17 Original save games (`.GAM`)

Not supported. `PizzaFactory::create_pizza_from_gam_node()`, `gam_node_recipe_uid()` and
`gam_node_daily_sales()` can parse a recipe node of an original save, but nothing in `src/`
calls them, and there is no `.GAM` loader.

---

## 2. Engine formats

### 2.1 `data/*.json` content tables

Location: `<program dir>/data/` (`platform_paths::data_dir()`, `src/engine/PlatformPaths.cpp`).
Parsed by `data_file::parse_file()` / `data_file::load<T, E>()`
(`src/core/factories/DataFile.h`, `.cpp`). Rules enforced by `load`:

* the root is a JSON object;
* every key must be the exact name of a value of the C++ enum `E` (`magic_enum::enum_cast`);
  any other key, including a comment key, is fatal;
* every enum value must be present (`expected N entries`);
* a field error names the file and entry.

Field lists are in [content-data.md](content-data.md).

| File | Enum (keys) | Reader |
|---|---|---|
| `furniture.json` | `FurnitureType` (163 values, `src/core/restaurant/RestaurantTypes.h`) | `FurnitureFactory` |
| `ingredients.json` | `core::food::IngredientType` (65 values, `src/core/food/IngredientType.h`) | `IngredientFactory` |
| `suppliers.json` | `SupplierID` (36 values, `src/core/supply/Supplier.h`) | `SupplierFactory` |
| `trainings.json` | `TrainingType` (18 values, `src/core/character/Training.h`) | `TrainingFactory` |

### 2.2 `data/fonts/hd_fonts.json`

Read by `HdTypefaces::configure(<data dir>/fonts)` when `hd_scale > 1`
(`src/resources/HdTypeface.cpp`; called from `Game::configure_hd`, `src/engine/Game.cpp`).
Object keyed by font name (`ASTANDAR`, ...). Entries that are not objects or have no `file`
are ignored (so `_comment` is allowed here).

| Field | Type | Default | Meaning |
|---|---|---|---|
| `file` | string | required | `.ttf` file name in the same folder; a path with a directory is rejected |
| `size` | number | 1.0 | cap height as a multiple of the original font's cap height |
| `tracking` | number | 0.0 | extra letter spacing in original font pixels |
| `uppercase` | bool | false | the original font has capitals only |

The original font's cap height and baseline are measured from its capital `H`
(`FontLoader::hd_metrics`).

### 2.3 `data/game_location.vga`

A VGA picture in the format of 1.3, written by the engine's `VgaWriter`
(`src/resources/VgaWriter.cpp`) and generated by `tools/gen_game_location_screen`. Shown by
`GameLocationScreen` before the original game is found (`src/views/screens/GameLocationScreen.cpp`).

### 2.4 Save games `.plsave`

Directory: `<user dir>/saves/` (`platform_paths::saves_dir()`). Autosaves are
`AUTOSAV<n>.plsave`, rotating over `autosave_count` slots (`next_autosave_filename`). Writer
and reader: `SaveGameManager` (`src/core/save/SaveGameManager.cpp`).

| Offset | Type | Field |
|---|---|---|
| 0 | char[6] | `"PLSAVE"` |
| 6 | u16 | header version (2) |
| 8 | u64 | uncompressed JSON size (max 256 MiB) |
| 16 | u32 | (version >= 2) metadata length `L` (max 1 MiB) |
| 20 | L bytes | uncompressed metadata JSON: `saved_at`, `game_date`, `player_names`, `active_player`, `active_city` |
| 20 + L | ... | zlib (`compress2`) stream of the full JSON document |

Writes go to `<file>.tmp` and are renamed. Document version: `SAVE_FORMAT_VERSION = 3`;
versions 1..3 load; version 1 is migrated (premise status 1 and 2 swapped,
`migrate_premise_status`).

Top-level keys (`SaveGameManager::state_to_json`): `version`, `rng_state`, `saved_at`, `time`
(`blocks_since_start`, `display`), `active_player`, `active_city`, `city_initialised`,
`city_start_of_day_ran`, `restaurant_day_ran`, `city_ai_action_taken`, `active_restaurant`,
`difficulty_level`, `rules`, `starting_rules`, `rules_changed`,
`district_under_investigation`, `game_mode`, `mission_target`, `mission_rules`,
`mission_rules_changed`, `customer_taste_profiles`, `weapon_market`, `players`, `cities`,
`restaurants`, `deferred_staff_events`, `deferred_calls`, `deferred_racket_offers`,
`deferred_manager_alerts`, `floor_editing_bonus_shown`, `competition_count`,
`price_update_category`, `interruption_phase`, `second_in_block`, `pit_tick_in_second`,
`pending_screen_charge_blocks`, `player_cities`, `shown_messages`.

Sub-objects are written by `src/core/save/SaveGameCity.cpp` (properties by tile `index`,
suppliers' stock, price levels and relationships, trends, charts, control map, mayor office,
depots, newspaper), `SaveGameRestaurant.cpp` (layout, menu, staff, stats, inventory, economy),
`SaveGameCharacter.cpp` (players, characters, finances, loans, mafia and insurance state,
contracts, campaigns) and `SaveGameFood.cpp` (pizzas and recipes).

Important for modding: a save stores only the city id (`cities[].id`). On load,
`GameState::ensure_city_loaded()` rebuilds each city from the original `dat/` files and then
overlays the saved dynamic state (`json_to_city`). Static city data (map, districts, actors,
suppliers' identities) is not in the save.

`SaveGameManager::fingerprint()` hashes the document without `saved_at` (`debug dump
fingerprint`); the HD gates use it to prove that rendering options do not change the game.

### 2.5 High scores `highscore.dat`

In the saves directory. Same 16-byte header with magic `"PLHIGH"` and version 1, no metadata
block, then a zlib JSON `{"version": 1, "entries": [{"name", "score"}]}`; at most
`HIGH_SCORE_MAX_ENTRIES = 8` (`load_high_scores`, `add_high_score`).

### 2.6 Mission rules trailer `PLRS`

Written after the 400 original bytes by `write_mission_file()`; the original game ignores it.

| Offset | Type | Field |
|---|---|---|
| 400 | char[4] | `"PLRS"` |
| 404 | u8 | trailer version (1) |
| 405 | u8 | payload length |
| 406 | bytes | `rules_changed`, `fix_legacy_bugs`, `mafia_shakedowns_target_all_restaurants`, `demand_fulfillment_popularity`, `win_condition`, `pizza_judging`, `police`, `suppliers_remember_missed_payments`, `cook_book` (one byte each) |

Unknown trailing payload bytes are ignored, so newer files load in older builds. Player
missions go to `<user dir>/missions/<name>.mis`.

### 2.7 HD pack layout

`resources::HdPack` (`src/resources/HdPack.h`, `.cpp`). Root folder from `--hd-pack`, the
`hd_pack` option, or an `hd` folder next to the program (`Game::configure_hd`). Active only
when `hd_scale` is 2..8 (`gfx::max_hd_scale = 8`).

| Original | Pack file |
|---|---|
| VGA key `gfx/back1` (`GFX/BACK1.VGA`) | `<pack>/gfx/back1.png` |
| BOB key `bob/moebel.bob`, frame 12 | `<pack>/bob/moebel/0012.png` |

Key normalisation (`HdPack::normalize_key`): backslashes to `/`, lower case, `.vga`/`.bob`
removed, leading `./` removed; absolute paths become `plegacy/<file name>` (engine-owned files
such as `data/game_location.vga`).

Rules (`HdPack::decode`):

* PNG of any size whose aspect ratio is within 2 % of the original's
  (`aspect_tolerance = 0.02`); otherwise it is rejected with a warning.
* It is resampled once to `engine_size * hd_scale` (engine size = original doubled) and cached.
* Missing files keep the original art. Alpha is respected when drawing.
* Only existing pictures and existing frames can be replaced. The pack cannot add frames or
  new pictures; frame counts and sizes still come from the original files.

The HD image travels with the `Surface` (`Surface::attach_hd`) and is drawn by the GPU
compositor (`src/engine/HdCompositor.*`, `src/engine/gfx/HdList.*`). Game logic is unchanged.

### 2.8 Asset export layout and `manifest.json`

`resources::export_assets(game_root, out_dir)` (`src/resources/AssetExporter.cpp`), run with
`--export-assets <dir>`. It walks the game folder for `.vga` and `.bob` files, decodes them
with the engine loaders (plain doubling, no pack), takes every second pixel back to original
size, and writes PNGs in exactly the pack layout of 2.7, with index 0 as alpha 0. Damaged
files are skipped (`plausible_vga`, `plausible_bob`); empty BOB frames are skipped.

`manifest.json`:

```json
{
  "format": 1,
  "generator": "Pizza Legacy HD asset export",
  "engine_pixel_scale": 2,
  "note": "...",
  "images": [
    {"kind": "vga", "key": "gfx/back1", "file": "gfx/back1.png", "width": 192,
     "height": 176, "palette": 6, "palette_known": true,
     "opaque_pixels": 0, "transparent_pixels": 0},
    {"kind": "bob", "key": "bob/moebel", "file": "bob/moebel/0012.png", "frame": 12, "...": "..."}
  ]
}
```

(Values in the example are illustrative.) `tools/hdpack/hdpack.py validate` checks a pack
against this manifest the way the engine will.

### 2.9 HD warm list `hd_warm.txt`

Next to the settings file. Written at exit (`HdPack::save_warm_list`): a `#` comment line,
then `width height relative_path` per pack file used, in first-use order. Read at start-up
and decoded on a background thread up to 1.5 GiB (`HdPack::prefetch`, `Game::run`). Lines
with `..` or absolute paths are ignored.

### 2.10 `settings.conf` and `bindings.conf`

In the per-user directory (`platform_paths::config_file_path()`,
`platform_paths::bindings_file_path()`). `settings.conf` is `key=value` lines; blank lines and
lines starting with `#` are ignored (`ConfigManager`, `src/core/util/ConfigManager.cpp`).
Keys include the rules (`fix_legacy_bugs`, `win_condition`, `pizza_judging`, ...), audio,
display (`hd_scale`, `hd_pack`, `hd_text`, `smooth_motion`, `lighting`, `vsync`, ...) and
`game_path`. `bindings.conf` documents itself (`src/engine/input/BindingsFile.cpp`).

### 2.11 Writers

| Writer | Output | Source |
|---|---|---|
| `VgaWriter::to_bytes(surface, scale)` | VGA with threshold 128 (last byte 129), runs of up to 127, literals of up to 129 | `src/resources/VgaWriter.cpp`; console `savevga` |
| `io::recipe::save_prez()` | PREZ recipe; uid fixed at `0x20`, judged = 1 | `src/core/food/RecipeCodec.cpp` |
| `write_mission_file()` | `.mis` + PLRS trailer | `src/core/mission/MissionFile.cpp` |
| `SaveGameManager::save()` | `.plsave` | `src/core/save/SaveGameManager.cpp` |
| `export_assets()` | PNG + manifest | `src/resources/AssetExporter.cpp` |

---

## 3. Palette assignment table

`AssetMetadataRegistry::metadata_` (`src/resources/AssetMetadata.cpp`) maps a key to a palette
index (0..70) and a nominal size. Lookup falls back to the base name with `gfx/` or `bob/`
prefixed. Files not in the table use palette 0. Summary:

| Keys | Palette |
|---|---|
| `gfx/k0..k13` (floors), `gfx/back9`, `bob/moebel`, `bob/kuebobs`, `bob/walk*`, `bob/night*`, `bob/lebbo`, `bob/stas`, `bob/kus`, `bob/mens` | 4 |
| `gfx/back1..back40` | individual (4..61) per file |
| `gfx/blitmap1` 5, `blitmap2` 7, `blitmap3` 13, `blitmap4` 7, `blitmap5` 9 | as listed |
| `gfx/med1`, `gfx/permap`, `gfx/permap.i` | 0 |
| `gfx/euromap` | 2 |
| `gfx/landsym` | 1 |
| `gfx/menu`, `gfx/menu.f`, `bob/menubobs`, `gfx/menubobs` | 38 |
| `gfx/titel1` 64, `gfx/titel2` 65, `gfx/cyber` 66, `gfx/ende` 63, `gfx/mplogolo` 70, `gfx/maestro` 41, `gfx/jury` 12, `gfx/orden` 36, `gfx/bo` 62, `gfx/icons` 62, `gfx/tvmisc` 47 | as listed |
| `gfx/zw1..zw11` | 48..58 |
| `gfx/zb1..zb20` | 35 |
| `gfx/spot0..8` 47, `gfx/plakat0..8` 46 | as listed |
| `gfx/g0..g23` | none in the table (handle 0). The pixels are indices; `GuestTalkPopup` blits them onto its own surface, so they show in that surface's palette |
| `gfx/back41`, `gfx/back99`, `gfx/blitmap6` | none; listed with a size only, no code reference found |
| `bob/gaeste` 16, `bob/stanbobs` 8, `bob/stabo` 42, `bob/zutaten` 6, `bob/typNe` 22 (typ3e 5, typ4e 14, typ22e 36) | as listed |

A new picture for a mod needs an entry here (or an explicit palette at the call site), since
the file format has no palette field.

## 4. Unknowns

* VGA header bytes 4-7 in original files are not read; the writer's meaning (data size and 0)
  is the engine's own convention and is assumed, not verified, for originals.
* BOB header bytes 2-5 and frame header bytes 2-6.
* FNT header bytes 11-13, 16-23 and `start_at_bit`, `baseline_height`.
* `cityN.pc`: bytes 0-7, 10-39, 45, 96-105, 306-345, 1546-1785, 1986-2025 (after up to 10
  mafia records), 2218-2547; the purpose of the two unknown realtor bytes and three unknown
  mafia bytes; supplier record bytes 36-232 and 234-238; district record unknown bytes; food
  supplier categories 4-7.
* `chr/cNe.pc`: the unknown fields listed above; the Amiga sources reportedly had more skills.
* `f/fN.pc`: header bytes 6-8 and 20-38; record bytes 2-4.
* `.e` text files: the code assumes ASCII-compatible text; any non-UTF-8 byte is drawn as
  U+FFFD. The original encoding of non-ASCII characters in these files is not handled.
* `.GAM` original save files: not decoded beyond the recipe node.
* CMF is decoded by libADLMIDI, not by engine code.
