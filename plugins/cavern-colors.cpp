// Restores per-mineral floor/ramp/stair colors in Steam/premium graphics mode.
//
// In ASCII DF, mined stone surfaces inherit the mineral's color. In the Steam
// renderer all cavern stone uses uncolored grey sprites. This plugin patches
// screentexpos_background (the floor layer) after DF renders each frame: for
// each mineral-bearing tile, it tints DF's own sprite with the mineral's color
// and substitutes the tinted variant. Variants are cached per (source-sprite,
// material) pair so the carved shape (ramp slope, stair, flat floor) is
// preserved while the color is applied.

#include "Debug.h"
#include "LuaTools.h"
#include "PluginLua.h"
#include "PluginManager.h"
#include "TileTypes.h"
#include "VTableInterpose.h"

#include "modules/Gui.h"
#include "modules/Maps.h"
#include "modules/Screen.h"
#include "modules/Filesystem.h"
#include "modules/Textures.h"

#include "modules/DFSDL.h"

#include "df/block_square_event.h"
#include "df/block_square_event_mineralst.h"
#include "df/enabler.h"
#include "df/feature_init.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/inorganic_raw.h"
#include "df/map_block.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/world.h"

#include <SDL_pixels.h>
#include <SDL_surface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace DFHack;
using namespace df::enums;
using namespace DFHack::DFSDL;
using df::global::enabler;
using df::global::gps;
using df::global::window_x;
using df::global::window_y;
using df::global::window_z;
using df::global::world;

DFHACK_PLUGIN("cavern-colors");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(enabler);
REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);
REQUIRE_GLOBAL(world);

namespace DFHack {
    DBG_DECLARE(cavern_colors, log, DebugCategory::LINFO);
}

// ---------------------------------------------------------------------------
// Color mode

enum class ColorMode { hybrid, mat_rgb, basic_color };
static ColorMode color_mode = ColorMode::hybrid;

static const char *mode_name(ColorMode m) {
    switch (m) {
    case ColorMode::hybrid:      return "hybrid";
    case ColorMode::mat_rgb:     return "mat_rgb";
    case ColorMode::basic_color: return "basic_color";
    }
    return "?";
}

// DF's standard 16-color palette (CGA-like), indexed by basic_color[0] + (bright ? 8 : 0).
static const uint8_t DF_PALETTE[16][3] = {
    {  0,   0,   0}, // 0  black
    {  0,   0, 128}, // 1  dark blue
    {  0, 128,   0}, // 2  dark green
    {  0, 128, 128}, // 3  dark cyan
    {128,   0,   0}, // 4  dark red
    {128,   0, 128}, // 5  dark magenta
    {128, 128,   0}, // 6  dark yellow (brown)
    {192, 192, 192}, // 7  grey
    {128, 128, 128}, // 8  dark grey
    {  0,   0, 255}, // 9  blue
    {  0, 255,   0}, // 10 green
    {  0, 255, 255}, // 11 cyan
    {255,   0,   0}, // 12 red
    {255,   0, 255}, // 13 magenta
    {255, 255,   0}, // 14 yellow
    {255, 255, 255}, // 15 white
};

// ---------------------------------------------------------------------------
// Per-material tint factors + dynamic cache of tinted sprite variants

static uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static void palette_tint(int fg, int bright,
                         uint8_t &tr, uint8_t &tg, uint8_t &tb) {
    int idx = std::clamp(fg, 0, 7) + (bright ? 8 : 0);
    tr = DF_PALETTE[idx][0];
    tg = DF_PALETTE[idx][1];
    tb = DF_PALETTE[idx][2];
}

static bool get_tint(const df::material &mat, uint8_t &tr, uint8_t &tg, uint8_t &tb) {
    if (color_mode == ColorMode::mat_rgb ||
        (color_mode == ColorMode::hybrid &&
         (mat.mat_rgb[0] > 0.0f || mat.mat_rgb[1] > 0.0f || mat.mat_rgb[2] > 0.0f)))
    {
        tr = (uint8_t)(std::clamp(mat.mat_rgb[0], 0.0f, 1.0f) * 255.0f);
        tg = (uint8_t)(std::clamp(mat.mat_rgb[1], 0.0f, 1.0f) * 255.0f);
        tb = (uint8_t)(std::clamp(mat.mat_rgb[2], 0.0f, 1.0f) * 255.0f);
        if (tr || tg || tb) return true;
    }
    // hybrid fallback and basic_color mode both use basic_color
    palette_tint(mat.basic_color[0], mat.basic_color[1], tr, tg, tb);
    return true;
}

// Tint math (clamp-source-first multiply with saturation control):
//   eff_mat_channel = mat_channel * tint_strength + 255 * (1 - tint_strength)
//   boosted_channel = min(src_channel * brightness_boost, 255)
//   result_channel  = boosted_channel * eff_mat_channel / 255
//
// brightness_boost compensates for DF's carved-stone sprites being darker than
// the surface they're meant to represent (multiplicative tint alone would push
// them too dim). tint_strength lerps the per-material color toward white:
// 1.0 = full saturation, 0.0 = no color shift. Defaults tuned for stock DF
// content; both are user-tunable at runtime via the command.
//
// Clamping each boosted source channel *before* the material multiply has two
// useful properties vs. a direct `src * mat * boost / 255` with after-clamp:
//
//   1. Mid-tones don't clamp and behave like a straight multiply — channel-
//      level detail from the source sprite (cracks, shading variation in
//      the brick pattern) survives.
//   2. Very bright source pixels (lit ramp cream faces) clamp uniformly to
//      255 on every channel that exceeded the limit, then collapse cleanly
//      to peak material color when multiplied. A direct multiply with
//      after-clamp saturates the strongest material channel first and
//      shifts hue — pale materials blow out to pure white, claystone
//      goes vibrant orange.
static float brightness_boost = 2.0f;
static float tint_strength = 0.8f;

// Per-material RGB tint, indexed by inorganic_mat. Stored 0-255 (no boost
// applied here; the boost is applied per-pixel so we can clamp).
static std::vector<std::array<uint8_t, 3>> material_tints;

// Cache of tinted sprite variants. Key: (source_texpos << 32) | inorganic_mat.
// Value: TexposHandle of the tinted variant (0 if the source couldn't be read).
static std::unordered_map<uint64_t, TexposHandle> tinted_cache;

static uint64_t cache_key(int32_t src_texpos, int mat) {
    return ((uint64_t)(uint32_t)src_texpos << 32) | (uint32_t)mat;
}

static void build_material_tints() {
    material_tints.clear();
    if (!world || world->raws.inorganics.all.empty()) return;

    auto &inorganics = world->raws.inorganics.all;
    material_tints.resize(inorganics.size(), {255, 255, 255});
    for (size_t i = 0; i < inorganics.size(); i++) {
        if (!inorganics[i]) continue;
        auto &m = inorganics[i]->material;
        get_tint(m, material_tints[i][0], material_tints[i][1], material_tints[i][2]);
        int bc_idx = std::clamp((int)m.basic_color[0], 0, 7) + (m.basic_color[1] ? 8 : 0);
        DEBUG(log).print(
            "material {}: tint={}/{}/{} mat_rgb={:.3f}/{:.3f}/{:.3f} "
            "basic_color=[{},{}] (palette#{} -> {}/{}/{})\n",
            inorganics[i]->id,
            material_tints[i][0], material_tints[i][1], material_tints[i][2],
            m.mat_rgb[0], m.mat_rgb[1], m.mat_rgb[2],
            m.basic_color[0], m.basic_color[1],
            bc_idx, DF_PALETTE[bc_idx][0], DF_PALETTE[bc_idx][1], DF_PALETTE[bc_idx][2]);
    }
    DEBUG(log).print("computed tints for {} inorganic materials\n",
                     material_tints.size());
}

// Read the pixels of the sprite at src_texpos from DF's texture atlas, apply
// the per-material multiplicative tint, and register the result as a new
// texture. Preserves the source sprite's dimensions so carved ramp/stair shapes
// render correctly.
static TexposHandle make_tinted_from(int32_t src_texpos, int mat) {
    if (!enabler) return 0;
    if (src_texpos <= 0) return 0;
    if ((size_t)src_texpos >= enabler->textures.raws.size()) return 0;
    if (mat < 0 || (size_t)mat >= material_tints.size()) return 0;

    SDL_Surface *src = (SDL_Surface *)enabler->textures.raws[src_texpos];
    if (!src) return 0;

    SDL_PixelFormat *fmt = DFSDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!fmt) return 0;
    SDL_Surface *conv = DFSDL_ConvertSurface(src, fmt, 0);
    if (!conv) return 0;

    int w = conv->w, h = conv->h;

    // Lerp the per-material color toward white by (1 - tint_strength) once,
    // so the per-pixel inner loop stays a plain multiply.
    float s = std::clamp(tint_strength, 0.0f, 1.0f);
    float boost = std::max(brightness_boost, 0.0f);
    auto lerp_white = [s](uint8_t c) -> uint8_t {
        return (uint8_t)std::clamp(c * s + 255.0f * (1.0f - s), 0.0f, 255.0f);
    };
    uint8_t mr = lerp_white(material_tints[mat][0]);
    uint8_t mg = lerp_white(material_tints[mat][1]);
    uint8_t mb = lerp_white(material_tints[mat][2]);

    std::vector<uint32_t> pixels((size_t)w * h);
    for (int y = 0; y < h; y++) {
        uint8_t *row = (uint8_t *)conv->pixels + y * conv->pitch;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + x * 4;
            int br = (int)(p[0] * boost); if (br > 255) br = 255;
            int bg = (int)(p[1] * boost); if (bg > 255) bg = 255;
            int bb = (int)(p[2] * boost); if (bb > 255) bb = 255;
            int r = br * mr / 255;
            int g = bg * mg / 255;
            int b = bb * mb / 255;
            pixels[(size_t)y * w + x] = rgba((uint8_t)r, (uint8_t)g, (uint8_t)b, p[3]);
        }
    }

    DFSDL_FreeSurface(conv);
    return Textures::createTile(pixels, w, h, true);
}

static TexposHandle get_tinted(int32_t src_texpos, int mat) {
    uint64_t k = cache_key(src_texpos, mat);
    auto it = tinted_cache.find(k);
    if (it != tinted_cache.end()) return it->second;
    TexposHandle h = make_tinted_from(src_texpos, mat);
    tinted_cache[k] = h; // cache even 0 so we don't retry failing lookups
    return h;
}

// ---------------------------------------------------------------------------
// Rough-edge bleed
//
// DF composites a per-side overlay onto cells adjacent to rough cavern
// tiles. The fringe sprites come from vanilla floors.png, which packs a
// 9-slice for rough cavern floors at (1-based) rows 4-6 × cols 1-3:
//
//   (c1,r4) NW | (c2,r4) N  | (c3,r4) NE
//   (c1,r5) W  | (c2,r5) C  | (c3,r5) E
//   (c1,r6) SW | (c2,r6) S  | (c3,r6) SE
//
// Centre (c2,r5) is the rough tile itself — DF draws it on the rough
// cell and our base-sprite tint path already colors it. The 8 surrounding
// cells are the fringes that bleed onto adjacent floors.
//
// Spatial inversion: the sprite at compass direction X in the source
// arrangement depicts how the rough extends INTO its neighbour at
// direction X. From the receiver's frame the rough is on the OPPOSITE
// side. So receiver "rough-to-S" → draw the PNG-N sprite (c2,r4): its
// content sits along its own bottom edge, which lands at receiver's
// south edge once composited.
//
// floor_flag[idx] on the receiver tells us which sides have rough. Byte
// layout (confirmed by sample-cell):
//   byte 0 (bits  0-7):  S
//   byte 1 (bits  8-15): W
//   byte 2 (bits 16-23): E
//   byte 3 (bits 24-31): N
// Within each byte: bits 0-2 = texture_index variant (unused for our
// purposes — we use only the single per-direction sprite from the PNG),
// bit 3 = enable. Bytes 4-7 hold diagonals; not yet decoded.
//
// DF's renderer uploads each fringe sprite to GPU at world-load and
// ignores subsequent SDL_Surface edits (verified with paint-overlay).
// So we can't tint in place. Fix: bake a composite per cell — base
// sprite tinted by base mat plus alpha-blended (PNG fringe re-tinted by
// rough neighbour mat), register via Textures::createTile, replace
// screentexpos_background[idx], zero floor_flag[idx] to suppress DF's
// own untinted overlay redraw.

static bool rough_edges_enabled = true;

// When the cell at the current z is open/empty, DF renders a fogged-down
// sprite from the deepest visible floor below. By default we mirror DF and
// walk down z-levels for those cells so the tint follows the sprite that's
// actually being shown. Disable to restrict tinting to the current z only.
static bool z_fog_enabled = true;

// One fringe sprite per direction (4 cardinals + 4 diagonals), loaded
// once from floors.png at world-load. Indexed by ROUGH_SIDES position:
// 0=S 1=W 2=E 3=N 4=NE 5=SE 6=SW 7=NW.
struct directional_overlay {
    bool valid = false;
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;
    int32_t source_texpos = 0; // synthetic id: col*100+row (1-based)
};
static std::array<directional_overlay, 8> overlay_by_dir;
struct rough_side_info {
    int dx, dy;
    int byte_offset; // floor_flag byte index
};
static const std::array<rough_side_info, 4> ROUGH_SIDES = {{
    { 0, +1, 0}, // S (byte 0)
    {-1,  0, 1}, // W (byte 1)
    {+1,  0, 2}, // E (byte 2)
    { 0, -1, 3}, // N (byte 3)
}};

// Corners aren't encoded in floor_flag at all — DF draws a corner fringe
// whenever both of its constituent cardinals have rough neighbours. The
// corner sprite is rendered UNDER the two cardinal fringes (corners
// fill, cardinals overlay). The corner takes its tint from the first of
// its two cardinals that has a resolved material; if neither resolves we
// skip it.
struct corner_info {
    int dx, dy;        // neighbour offset (informational)
    int cardinal_a;    // ROUGH_SIDES index of first constituent
    int cardinal_b;    // ROUGH_SIDES index of second constituent
    int overlay_index; // overlay_by_dir slot (4..7)
};
static const std::array<corner_info, 4> ROUGH_CORNERS = {{
    { +1, -1, 3, 2, 4 }, // NE: N + E → overlay slot 4
    { +1, +1, 0, 2, 5 }, // SE: S + E → slot 5
    { -1, +1, 0, 1, 6 }, // SW: S + W → slot 6
    { -1, -1, 3, 1, 7 }, // NW: N + W → slot 7
}};

// Composite cache key. (base_texpos, floor_flag, per-cardinal rough
// neighbour mats, base mat) uniquely determines the composite output.
// Corners are derived deterministically from the cardinal mats so they
// don't need their own cache fields.
struct composite_key {
    int32_t base_texpos;
    uint64_t floor_flag;
    int16_t base_mat;
    int16_t mat[4]; // indexed by ROUGH_SIDES position (cardinals only)
    bool operator==(const composite_key &o) const {
        if (base_texpos != o.base_texpos || floor_flag != o.floor_flag ||
            base_mat != o.base_mat) return false;
        for (int i = 0; i < 4; i++)
            if (mat[i] != o.mat[i]) return false;
        return true;
    }
};
struct composite_key_hash {
    size_t operator()(const composite_key &k) const {
        size_t h = std::hash<uint64_t>()(k.floor_flag);
        auto mix = [&h](uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        };
        mix((uint32_t)k.base_texpos);
        mix((uint16_t)k.base_mat);
        for (int i = 0; i < 4; i++) mix((uint16_t)k.mat[i]);
        return h;
    }
};
static std::unordered_map<composite_key, TexposHandle, composite_key_hash>
    composite_cache;

static void clear_composite_cache();

// Load the rough-floor fringe sprites from vanilla floors.png.
//
// floors.png ships a 9-slice for cavern floors at (1-based) rows 4..6
// × cols 1..3. The centre (row 5, col 2) is the rough stone tile itself
// — DF draws that directly on rough cavern cells, our plugin already
// tints it via the base-sprite path. The 8 surrounding cells are the
// fringe sprites that bleed into adjacent floors.
//
// Spatial inversion: the sprite at compass-direction X in the source
// arrangement depicts how the rough tile extends INTO its neighbour at
// direction X. From the receiver's frame the rough is at the OPPOSITE
// side. So if our floor_flag says "rough is to the south" we draw the
// PNG-N sprite (row 4 col 2) — its fringe sits along its own bottom
// edge, which is exactly the south edge of the receiver where it ends
// up rendered.
//
// Mapping (ROUGH_SIDES index → floors.png cell, 1-based):
//   0 S  → (col 2, row 4)  PNG-N
//   1 W  → (col 3, row 5)  PNG-E
//   2 E  → (col 1, row 5)  PNG-W
//   3 N  → (col 2, row 6)  PNG-S
//   4 NE → (col 1, row 6)  PNG-SW
//   5 SE → (col 1, row 4)  PNG-NW
//   6 SW → (col 3, row 4)  PNG-NE
//   7 NW → (col 3, row 6)  PNG-SE
static void snapshot_overlays() {
    for (auto &o : overlay_by_dir) o = {};
    auto path = Filesystem::getcwd() /
        "data" / "vanilla" / "vanilla_environment" /
        "graphics" / "images" / "floors.png";
    SDL_Surface *src = DFIMG_Load(path.string().c_str());
    if (!src) {
        color_ostream_proxy c(Core::getInstance().getConsole());
        c.printerr("[cavern-colors] could not load floors.png at '{}'\n",
                   path.string());
        return;
    }
    SDL_PixelFormat *fmt = DFSDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!fmt) { DFSDL_FreeSurface(src); return; }
    SDL_Surface *conv = DFSDL_ConvertSurface(src, fmt, 0);
    DFSDL_FreeSurface(src);
    if (!conv) return;

    auto extract = [&](int dir, int col_1based, int row_1based) {
        int sx = (col_1based - 1) * 32;
        int sy = (row_1based - 1) * 32;
        if (sx + 32 > conv->w || sy + 32 > conv->h) return;
        auto &snap = overlay_by_dir[dir];
        snap.valid = true;
        snap.w = 32;
        snap.h = 32;
        snap.pixels.assign(32 * 32, 0);
        // Track the source coords so the status block can show what
        // got loaded (use a synthetic identifier in source_texpos:
        // col*100+row, easy to read).
        snap.source_texpos = col_1based * 100 + row_1based;
        for (int dy = 0; dy < 32; dy++) {
            uint8_t *row = (uint8_t *)conv->pixels +
                           (sy + dy) * conv->pitch + sx * 4;
            memcpy(&snap.pixels[(size_t)dy * 32], row, 32 * 4);
        }
    };
    extract(0, 2, 4); // S  receiver → PNG N  (above centre)
    extract(1, 3, 5); // W  receiver → PNG E  (right of centre)
    extract(2, 1, 5); // E  receiver → PNG W  (left of centre)
    extract(3, 2, 6); // N  receiver → PNG S  (below centre)
    extract(4, 1, 6); // NE receiver → PNG SW (bottom-left of cluster)
    extract(5, 1, 4); // SE receiver → PNG NW (top-left of cluster)
    extract(6, 3, 4); // SW receiver → PNG NE (top-right of cluster)
    extract(7, 3, 6); // NW receiver → PNG SE (bottom-right of cluster)

    DFSDL_FreeSurface(conv);

    color_ostream_proxy c(Core::getInstance().getConsole());
    c.print("[cavern-colors] loaded rough-edge fringes from floors.png "
            "(S:{} W:{} E:{} N:{} NE:{} SE:{} SW:{} NW:{})\n",
            overlay_by_dir[0].valid ? "ok" : "-",
            overlay_by_dir[1].valid ? "ok" : "-",
            overlay_by_dir[2].valid ? "ok" : "-",
            overlay_by_dir[3].valid ? "ok" : "-",
            overlay_by_dir[4].valid ? "ok" : "-",
            overlay_by_dir[5].valid ? "ok" : "-",
            overlay_by_dir[6].valid ? "ok" : "-",
            overlay_by_dir[7].valid ? "ok" : "-");
}

static void clear_composite_cache() {
    composite_cache.clear();
}

static void clear_tinted_cache() {
    // Workaround: not calling Textures::deleteHandle.
    //
    // deleteHandle ends with `while (surface->refcount) DFSDL_FreeSurface(surface);`
    // (library/modules/Textures.cpp). SDL_FreeSurface frees the SDL_Surface struct
    // when refcount reaches 0, so the loop's next refcount read is on freed memory.
    // Observed to crash inside SDL2 intermittently on mode switches — intermittency
    // consistent with a use-after-free whose outcome depends on allocator state.
    // Skipping the call leaves the handles registered in dfhack's texture maps;
    // they're released by Textures::cleanup() on DFHack shutdown.
    //
    // Upstream fix candidates for deleteHandle:
    //   int n = surface->refcount;
    //   for (int i = 0; i < n; i++) DFSDL_FreeSurface(surface);
    // or:
    //   surface->refcount = 1;
    //   DFSDL_FreeSurface(surface);
    tinted_cache.clear();
}

// ---------------------------------------------------------------------------
// Per-tile material lookup
//
// MINERAL tiles (veins): walk block_events and take the last mineral event
//   whose bitmask covers (tx, ty).
// STONE tiles (layer stone): use the tile's biome + geolayer_index designation
//   to look up the layer's inorganic material via the geology cache.
// LAVA_STONE: typically obsidian and not represented in the geology layers;
//   fall back to the vein lookup (block events sometimes mark these tiles).
//
// TODO: consider switching to MapExtras::Block (modules/MapCache.h):
//   - baseMaterialAt(p) covers vein + layer + feature stone in one call
//   - lavaStoneAt(p) is a first-class lookup (replaces our LAVA_STONE
//     vein-fallback heuristic)
//   - layerMaterialAt(p) / biomeInfoAt(p).layer_stone[] replaces our
//     build_geology cache below
// This is on a per-frame hot path (~80x80 cells x up to 9 z-levels), and
// MapCache is built for editing rather than read-only sampling, so the
// trade-off needs measuring before switching.

// layer_mats[biome_idx][geolayer_idx] -> inorganic_mat. Built once per world.
// TODO: see MapExtras note above — biomeInfoAt(p).layer_stone[] gives the
// same data per-block without us caching it ourselves.
static std::vector<std::vector<int16_t>> layer_mats;

static void build_geology() {
    layer_mats.clear();
    std::vector<df::coord2d> geoidx;
    Maps::ReadGeology(&layer_mats, &geoidx);
    DEBUG(log).print("loaded geology for {} biomes\n", layer_mats.size());
}

// TODO: MapExtras::Block::veinMaterialAt / baseMaterialAt would replace this.
static int get_vein_mat(df::map_block *block, int tx, int ty) {
    int last = -1;
    for (auto *ev : block->block_events) {
        if (ev->getType() != df::block_square_event_type::mineral) continue;
        auto *m = (df::block_square_event_mineralst *)ev;
        if (m->getassignment(tx, ty)) last = m->inorganic_mat;
    }
    return last;
}

// TODO: MapExtras::Block::layerMaterialAt(p) returns this directly per-tile.
static int get_layer_mat(df::map_block *block, int tx, int ty) {
    auto &des = block->designation[tx][ty];
    // des.bits.biome is a per-tile slot index (0..8); block->region_offset[]
    // translates it to the cardinal-direction slot that layer_mats /
    // ReadGeology is indexed by. Hardcoding "Here" (=4) here is wrong:
    // region_offset[] varies per block, and ReadGeology's 3x3 is centred on
    // the embark's NW-corner world region — so the literal "4" only happens
    // to land on the right geology in some saves.
    uint8_t bslot = des.bits.biome;
    if (bslot >= 9) return -1;
    int biome = block->region_offset[bslot];
    int geolayer = des.bits.geolayer_index;
    if (biome < 0 || (size_t)biome >= layer_mats.size()) return -1;
    auto &row = layer_mats[biome];
    if (geolayer < 0 || (size_t)geolayer >= row.size()) return -1;
    return row[geolayer];
}

// TODO: MapExtras::Block::baseMaterialAt(p) collapses these three cases plus
// feature stone into a single lookup, and lavaStoneAt(p) replaces the
// LAVA_STONE → vein heuristic with the proper biome lava-stone material.
static int get_tile_mat(df::map_block *block, int tx, int ty, df::tiletype tt) {
    switch (tileMaterial(tt)) {
    case df::tiletype_material::MINERAL:    return get_vein_mat(block, tx, ty);
    case df::tiletype_material::STONE:      return get_layer_mat(block, tx, ty);
    case df::tiletype_material::LAVA_STONE: return get_vein_mat(block, tx, ty);
    default:                                return -1;
    }
}


// ---------------------------------------------------------------------------
// Composite sprite generation for rough-edge bleed. See the "Rough-edge
// bleed" section header above for the model and motivation.

// Re-tint a single overlay pixel for a target material. The cached overlay
// pixels carry whatever palette DF baked at world-load (in practice always
// one specific material, regardless of who the rough source actually is on
// the map). We treat each pixel as a luminance value, then apply the same
// brightness-boost-then-multiply-by-material-tint math that the base sprite
// uses. That preserves the overlay's shading texture while swapping its
// hue/saturation to the target material.
static uint32_t retint_overlay_pixel(uint32_t src, int mat) {
    uint8_t r = src & 0xff;
    uint8_t g = (src >> 8) & 0xff;
    uint8_t b = (src >> 16) & 0xff;
    uint8_t a = (src >> 24) & 0xff;
    if (a == 0) return 0;
    if (mat < 0 || (size_t)mat >= material_tints.size()) return src;
    float lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
    float s = std::clamp(tint_strength, 0.0f, 1.0f);
    float boost = std::max(brightness_boost, 0.0f);
    auto lerp_white = [s](uint8_t c) -> uint8_t {
        return (uint8_t)std::clamp(c * s + 255.0f * (1.0f - s), 0.0f, 255.0f);
    };
    uint8_t mr = lerp_white(material_tints[mat][0]);
    uint8_t mg = lerp_white(material_tints[mat][1]);
    uint8_t mb = lerp_white(material_tints[mat][2]);
    float lb = std::min(lum * boost, 1.0f);
    int new_r = (int)std::clamp(lb * mr, 0.0f, 255.0f);
    int new_g = (int)std::clamp(lb * mg, 0.0f, 255.0f);
    int new_b = (int)std::clamp(lb * mb, 0.0f, 255.0f);
    return rgba((uint8_t)new_r, (uint8_t)new_g, (uint8_t)new_b, a);
}

// Bake a composite: tinted base sprite + per-side tinted overlays.
static TexposHandle make_composite(const composite_key &key) {
    if (!enabler) return 0;
    if (key.base_texpos <= 0 ||
        (size_t)key.base_texpos >= enabler->textures.raws.size()) return 0;
    SDL_Surface *base_src =
        (SDL_Surface *)enabler->textures.raws[key.base_texpos];
    if (!base_src) return 0;
    SDL_PixelFormat *fmt = DFSDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!fmt) return 0;
    SDL_Surface *conv = DFSDL_ConvertSurface(base_src, fmt, 0);
    if (!conv) return 0;
    int w = conv->w, h = conv->h;
    std::vector<uint32_t> pixels((size_t)w * h);

    // 1. Tint base by base material (if known and in our tint table). This
    // mirrors make_tinted_from's per-channel boost-then-multiply pipeline.
    float s = std::clamp(tint_strength, 0.0f, 1.0f);
    float boost = std::max(brightness_boost, 0.0f);
    auto lerp_white = [s](uint8_t c) -> uint8_t {
        return (uint8_t)std::clamp(c * s + 255.0f * (1.0f - s), 0.0f, 255.0f);
    };
    bool tint_base = key.base_mat >= 0 &&
                     (size_t)key.base_mat < material_tints.size();
    uint8_t mr = 255, mg = 255, mb = 255;
    if (tint_base) {
        mr = lerp_white(material_tints[key.base_mat][0]);
        mg = lerp_white(material_tints[key.base_mat][1]);
        mb = lerp_white(material_tints[key.base_mat][2]);
    }
    for (int y = 0; y < h; y++) {
        uint8_t *row = (uint8_t *)conv->pixels + y * conv->pitch;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + x * 4;
            uint8_t pr = p[0], pg = p[1], pb = p[2], pa = p[3];
            if (tint_base) {
                int br = (int)(pr * boost); if (br > 255) br = 255;
                int bg = (int)(pg * boost); if (bg > 255) bg = 255;
                int bb = (int)(pb * boost); if (bb > 255) bb = 255;
                pr = (uint8_t)(br * mr / 255);
                pg = (uint8_t)(bg * mg / 255);
                pb = (uint8_t)(bb * mb / 255);
            }
            pixels[(size_t)y * w + x] = rgba(pr, pg, pb, pa);
        }
    }
    DFSDL_FreeSurface(conv);

    // 2. Alpha-blend overlays on top of the tinted base, in layer order:
    //    corners (lower) → cardinals (upper). DF renders corner fringes
    //    only when both their constituent cardinals are present, and
    //    cardinals draw over corners so the corner shape fills in gaps
    //    between the two cardinal strips.
    auto blend_overlay = [&](const directional_overlay &overlay, int16_t mat) {
        if (!overlay.valid) return;
        if (overlay.w != w || overlay.h != h) return;
        if (mat < 0) return;
        for (size_t p = 0; p < overlay.pixels.size(); p++) {
            uint32_t ov = retint_overlay_pixel(overlay.pixels[p], mat);
            uint8_t oa = (ov >> 24) & 0xff;
            if (oa == 0) continue;
            uint32_t base = pixels[p];
            uint8_t br = base & 0xff, bg = (base >> 8) & 0xff;
            uint8_t bb = (base >> 16) & 0xff, ba = (base >> 24) & 0xff;
            uint8_t or_ = ov & 0xff, og = (ov >> 8) & 0xff;
            uint8_t ob = (ov >> 16) & 0xff;
            int inv = 255 - oa;
            uint8_t nr = (uint8_t)((or_ * oa + br * inv) / 255);
            uint8_t ng = (uint8_t)((og * oa + bg * inv) / 255);
            uint8_t nb = (uint8_t)((ob * oa + bb * inv) / 255);
            uint8_t na = std::max(ba, oa);
            pixels[p] = rgba(nr, ng, nb, na);
        }
    };

    auto enabled = [&](int cardinal_idx) {
        int byte_value = (int)((key.floor_flag >>
                                (ROUGH_SIDES[cardinal_idx].byte_offset * 8))
                               & 0xff);
        return (byte_value & 0x08) != 0;
    };

    // Corners first (lower layer). Only apply a corner if both of its
    // cardinals resolved to a rough-stone material — a corner spanning
    // stone + grass shouldn't get our stone-tinted corner sprite, since
    // DF will draw its own mixed/grass corner naturally on the bytes we
    // leave intact.
    for (const auto &cn : ROUGH_CORNERS) {
        if (!enabled(cn.cardinal_a) || !enabled(cn.cardinal_b)) continue;
        if (key.mat[cn.cardinal_a] < 0 || key.mat[cn.cardinal_b] < 0) continue;
        int16_t mat = key.mat[cn.cardinal_a];
        blend_overlay(overlay_by_dir[cn.overlay_index], mat);
    }

    // Cardinals second (upper layer). Only sides that resolved to a
    // rough-stone material; sides whose neighbour is grass/etc. stay
    // in floor_flag for DF to render naturally.
    for (size_t i = 0; i < ROUGH_SIDES.size(); i++) {
        if (!enabled((int)i)) continue;
        if (key.mat[i] < 0) continue;
        blend_overlay(overlay_by_dir[i], key.mat[i]);
    }

    return Textures::createTile(pixels, w, h, true);
}

// Resolve a composite for the cell. `out_consumed_mask` returns the
// floor_flag byte mask of sides we actually composited (each handled
// side contributes 0xff at its byte offset). Sides whose neighbour
// isn't a rough-stone tile we recognise stay 0 in the mask — the
// caller should leave those bytes in floor_flag intact so DF can
// render its natural overlay (grass bleed, etc.) for them.
// Returns 0 if no sides resolved — in that case the caller should
// fall back to plain base-sprite tinting.
static TexposHandle get_composite(int32_t base_texpos, uint64_t floor_flag,
                                  int wx, int wy, int wz, int base_mat,
                                  uint64_t &out_consumed_mask) {
    out_consumed_mask = 0;
    composite_key key{};
    key.base_texpos = base_texpos;
    key.floor_flag = floor_flag;
    key.base_mat = (int16_t)base_mat;
    for (int i = 0; i < 4; i++) key.mat[i] = -1;
    for (size_t i = 0; i < ROUGH_SIDES.size(); i++) {
        int byte = (int)((floor_flag >>
                          (ROUGH_SIDES[i].byte_offset * 8)) & 0xff);
        if (!(byte & 0x08)) continue;
        int nx = wx + ROUGH_SIDES[i].dx;
        int ny = wy + ROUGH_SIDES[i].dy;
        df::map_block *nb = Maps::getTileBlock(nx, ny, wz);
        if (!nb) continue;
        df::tiletype *nt = Maps::getTileType(df::coord(nx, ny, wz));
        if (!nt) continue;
        int m = get_tile_mat(nb, nx & 15, ny & 15, *nt);
        if (m >= 0 && (size_t)m < material_tints.size()) {
            key.mat[i] = (int16_t)m;
            out_consumed_mask |=
                0xffULL << (ROUGH_SIDES[i].byte_offset * 8);
        }
    }
    if (out_consumed_mask == 0) return 0;

    auto it = composite_cache.find(key);
    if (it != composite_cache.end()) return it->second;
    TexposHandle h = make_composite(key);
    composite_cache[key] = h;
    return h;
}

// ---------------------------------------------------------------------------
// Render hook

// Tint one viewport's screentexpos arrays at z-level `z`. Used for both
// gps->main_viewport (at *window_z) and gps->lower_viewport[i] (at z =
// *window_z - (i+1)), which DF maintains as separate per-z-level
// graphic_viewportst structs to render depth-fogged sprites from below.
static void process_viewport(df::graphic_viewportst *vp,
                             int z,
                             const std::pair<df::coord2d, df::coord2d> &dims)
{
    if (!vp || !vp->screentexpos_background) return;

    for (int y = dims.first.y; y <= dims.second.y; y++) {
        for (int x = dims.first.x; x <= dims.second.x; x++) {
            size_t idx = (size_t)(x * vp->dim_y + y);
            int32_t src_texpos = vp->screentexpos_background[idx];
            if (src_texpos <= 0) continue;

            int wx = *window_x + x;
            int wy = *window_y + y;
            df::coord pos(wx, wy, z);

            if (!Maps::isTileVisible(pos)) continue;

            df::tiletype *tt = Maps::getTileType(pos);
            if (!tt) continue;
            // Tint dug/carved stone surfaces: open floors plus mined
            // ramps and stairs. Walls and Open shapes don't draw a
            // floor sprite — skip them.
            if (!isFloorTerrain(*tt) && !isRampTerrain(*tt) &&
                !isStairTerrain(*tt)) continue;
            auto shape = tileShapeBasic(tileShape(*tt));
            // Stone-like materials get base-sprite tinting. Other
            // floor surfaces (constructions, fungus, moss, grass)
            // keep their own appearance but still need rough-edge
            // processing — DF composites rough-edge overlays from
            // neighbouring rough cavern tiles onto *any* adjacent
            // floor, regardless of what that floor is made of.
            auto tile_mat_enum = tileMaterial(*tt);
            bool is_stone_like =
                tile_mat_enum == df::tiletype_material::STONE ||
                tile_mat_enum == df::tiletype_material::MINERAL ||
                tile_mat_enum == df::tiletype_material::LAVA_STONE;

            df::map_block *block = Maps::getTileBlock(wx, wy, z);
            if (!block) continue;

            int mat = -1;
            if (is_stone_like) {
                mat = get_tile_mat(block, wx & 15, wy & 15, *tt);
                if (mat < 0 ||
                    (size_t)mat >= material_tints.size())
                    mat = -1;
            }

            // Floor-like (FLOOR/RAMP/STAIR). Two paths:
            //  - composite: when the cell has rough-edge bits set in
            //    floor_flag and rough-edge processing is enabled. The
            //    composite handles both base-mat tint (if any) and the
            //    per-side overlay re-tinted by the rough neighbour's
            //    material.
            //  - plain tint: cell has no rough-edge bits, just tint by
            //    base material. Skipped when base is non-stone (e.g.
            //    constructions) since we have no material to tint by.
            uint64_t flag = 0;
            if (rough_edges_enabled &&
                shape == df::tiletype_shape_basic::Floor &&
                vp->screentexpos_floor_flag) {
                // Only take the composite path if we have at least one
                // directional overlay; otherwise the composite would
                // strip DF's own rough-edge draw and replace it with
                // nothing.
                bool have_any = false;
                for (auto &o : overlay_by_dir)
                    if (o.valid) { have_any = true; break; }
                if (have_any)
                    flag = vp->screentexpos_floor_flag[idx];
            }

            TexposHandle h = 0;
            uint64_t consumed = 0;
            if (flag != 0) {
                h = get_composite(src_texpos, flag, wx, wy, z, mat,
                                  consumed);
            }
            if (!h && mat >= 0) {
                // No rough-stone sides to composite (or rough-edges
                // disabled). Fall back to plain base tinting so
                // DF's natural bleed overlays (grass, etc.) keep
                // rendering through floor_flag.
                h = get_tinted(src_texpos, mat);
            }
            if (!h) continue;
            long texpos = Textures::getTexposByHandle(h);
            if (texpos > 0) {
                vp->screentexpos_background[idx] = (int32_t)texpos;
                if (consumed) {
                    // Suppress DF's own overlay redraw for the
                    // sides we already baked into the composite.
                    // Leave other sides' bytes intact so DF can
                    // still render grass/other natural bleed for
                    // those sides.
                    vp->screentexpos_floor_flag[idx] = flag & ~consumed;
                }
            }
        }
    }
}

struct cavern_colors_hook : df::viewscreen_dwarfmodest {
    typedef df::viewscreen_dwarfmodest interpose_base;

    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t curtick)) {
        INTERPOSE_NEXT(render)(curtick);

        if (!Screen::inGraphicsMode()) return;
        if (!gps || !gps->main_viewport) return;
        if (material_tints.empty()) return;

        auto dims = Gui::getDwarfmodeViewDims().map();

        // Main viewport: tiles at the current view z.
        process_viewport(gps->main_viewport, *window_z, dims);

        // Lower viewports: DF maintains a separate per-z-level
        // graphic_viewportst for each fogged floor below the current
        // view, used to render sprites that show through "Open" tiles
        // at the current z. We tint these too so the depth-fogged
        // sprites pick up the same per-material colors as the current
        // z-level. DF's shader applies the depth fog after our texpos
        // substitution, so the dimming composes naturally.
        if (z_fog_enabled) {
            for (int i = 0; i < 8; i++) {
                df::graphic_viewportst *lvp = gps->lower_viewport[i];
                if (!lvp) continue;
                process_viewport(lvp, *window_z - (i + 1), dims);
            }
        }
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(cavern_colors_hook, render);

// ---------------------------------------------------------------------------
// Sample one map cell: dump everything we can read about that position so we
// can find where per-material wall coloring lives in memory.
//
// In graphics mode the gps screen char-tile grid (gps->dimx/dimy) and the
// viewport map-cell grid (vp->dim_x/dim_y) are different spaces, related by
// `map_tile_pixels = viewport_zoom_factor / 4`. We side-step the conversion
// dance by going through Gui::getMousePos() (or an explicit world coord),
// then walking the *viewport* arrays at vp_idx = vx * vp->dim_y + vy. The
// gps->screen byte buffer is also sampled at one screen-tile inside the
// cell's footprint just to confirm whether that path carries per-map-cell
// color data or is unused in graphics mode.
//
// Lua-callable. wx<0 means "use the mouse position"; otherwise (wx, wy, wz)
// is the explicit world coord to sample.
static void sample_cell(color_ostream &out, int wx, int wy, int wz) {
    if (!gps || !world || !window_x || !window_y || !window_z) {
        out.printerr("globals not available\n");
        return;
    }

    df::coord world_pos;
    if (wx >= 0) {
        world_pos.x = wx;
        world_pos.y = wy;
        world_pos.z = wz;
    } else {
        world_pos = Gui::getMousePos(true);
        if (!world_pos.isValid()) {
            out.printerr("Mouse is not over a valid map position. Move the "
                         "cursor over the cell you want, or pass world "
                         "coords: cavern-colors sample-cell <wx> <wy> "
                         "[<wz>]\n");
            return;
        }
    }
    out.print("Sampling world=({},{},{})\n",
              world_pos.x, world_pos.y, world_pos.z);

    // World -> viewport-relative.
    auto *vp = gps->main_viewport;
    if (!vp) {
        out.printerr("main_viewport not available\n");
        return;
    }
    int vx = world_pos.x - *window_x;
    int vy = world_pos.y - *window_y;
    bool in_vp = vx >= 0 && vx < vp->dim_x && vy >= 0 && vy < vp->dim_y;
    out.print("  vp: screen_origin=({},{})  dim=({}x{}) map-cells  "
              "vp-rel=({},{}) in_vp={}  zoom_factor={}\n",
              vp->screen_x, vp->screen_y, vp->dim_x, vp->dim_y,
              vx, vy, in_vp ? "yes" : "no", gps->viewport_zoom_factor);
    if (!in_vp) {
        out.print("  (cell isn't currently in the viewport)\n");
        return;
    }

    // Viewport texpos arrays at vp_idx.
    size_t vp_idx = (size_t)vx * vp->dim_y + vy;
    out.print("  vp_idx={}\n", vp_idx);
    out.print("  vp->screentexpos_background[{}]      = {}\n",
              vp_idx, vp->screentexpos_background[vp_idx]);
    out.print("  vp->screentexpos_background_two[{}]  = {}\n",
              vp_idx, vp->screentexpos_background_two[vp_idx]);
    out.print("  vp->screentexpos[{}]                 = {}\n",
              vp_idx, vp->screentexpos[vp_idx]);
    out.print("  vp->screentexpos_item[{}]            = {}\n",
              vp_idx, vp->screentexpos_item[vp_idx]);
    out.print("  vp->screentexpos_building_one[{}]    = {}\n",
              vp_idx, vp->screentexpos_building_one[vp_idx]);
    out.print("  vp->screentexpos_floor_flag[{}]      = 0x{:x}\n",
              vp_idx, vp->screentexpos_floor_flag[vp_idx]);
    out.print("  vp->screentexpos_ramp_flag[{}]       = 0x{:x}\n",
              vp_idx, vp->screentexpos_ramp_flag[vp_idx]);
    out.print("  vp->screentexpos_shadow_flag[{}]     = 0x{:x}\n",
              vp_idx, vp->screentexpos_shadow_flag[vp_idx]);

    // Lower viewports: one per fogged-down z-level. Same vp_idx slot.
    for (int i = 0; i < 8; i++) {
        df::graphic_viewportst *lvp = gps->lower_viewport[i];
        if (!lvp || !lvp->screentexpos_background) {
            out.print("  lower_viewport[{}] (z-{}): (null)\n", i, i + 1);
            continue;
        }
        size_t lidx = (size_t)vx * lvp->dim_y + vy;
        out.print("  lower_viewport[{}] (z-{}): bg={} bg2={} "
                  "tex={} floor_flag=0x{:x} ramp_flag=0x{:x} "
                  "shadow_flag=0x{:x}\n",
                  i, i + 1,
                  lvp->screentexpos_background[lidx],
                  lvp->screentexpos_background_two[lidx],
                  lvp->screentexpos[lidx],
                  lvp->screentexpos_floor_flag[lidx],
                  lvp->screentexpos_ramp_flag[lidx],
                  lvp->screentexpos_shadow_flag[lidx]);
    }

    // gps->screen at the screen-tile that sits at the cell's top-left.
    // map_tile_pixels comes from Gui::getMousePos (viewport_zoom_factor / 4),
    // but for gps->screen we want char-tile coords. The viewport's screen_x/y
    // are in char-tile units and gps->dimx/dimy is char-tile space.
    int sx = vp->screen_x + vx * (gps->viewport_zoom_factor / 4);
    int sy = vp->screen_y + vy * (gps->viewport_zoom_factor / 4);
    out.print("  sampling gps->screen at char-tile ({},{}) "
              "(may not match cell exactly):\n", sx, sy);
    if (gps->screen && sx >= 0 && sy >= 0 && sx < gps->dimx && sy < gps->dimy) {
        size_t gps_idx = (size_t)sx * gps->dimy + sy;
        if ((gps_idx * 8 + 7) < (size_t)(gps->screen_limit - gps->screen + 1)) {
            uint8_t *c = &gps->screen[gps_idx * 8];
            out.print("    gps->screen[{}*8] = char={:3} "
                      "fg=({:3},{:3},{:3}) bg=({:3},{:3},{:3}) pad={:3}\n",
                      gps_idx, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
            if (gps->screentexpos)
                out.print("    gps->screentexpos[{}]       = {}\n",
                          gps_idx, gps->screentexpos[gps_idx]);
            if (gps->screentexpos_flag)
                out.print("    gps->screentexpos_flag[{}]  = 0x{:x}\n",
                          gps_idx, gps->screentexpos_flag[gps_idx]);
        }
    }

    // Tile + material at the world cell.
    df::tiletype *tt = Maps::getTileType(world_pos);
    if (!tt) {
        out.print("  (no tile type)\n");
        return;
    }
    out.print("  tile shape:    {}\n",
              ENUM_KEY_STR(tiletype_shape, tileShape(*tt)));
    out.print("  tile material: {}\n",
              ENUM_KEY_STR(tiletype_material, tileMaterial(*tt)));
    df::map_block *block = Maps::getTileBlock(world_pos.x, world_pos.y,
                                              world_pos.z);
    if (!block) return;
    int tx = world_pos.x & 15, ty = world_pos.y & 15;
    auto &dsgn = block->designation[tx][ty];

    // Geology-cache key (what get_layer_mat reads).
    out.print("  designation: biome={}, geolayer_index={}, "
              "feature_local={}, feature_global={}, subterranean={}, "
              "light={}, outside={}\n",
              (int)dsgn.bits.biome, (int)dsgn.bits.geolayer_index,
              (int)dsgn.bits.feature_local, (int)dsgn.bits.feature_global,
              (int)dsgn.bits.subterranean, (int)dsgn.bits.light,
              (int)dsgn.bits.outside);

    // region_offset[] translates the per-tile biome slot to the cardinal
    // slot that layer_mats is indexed by. This is the value get_layer_mat
    // actually uses; print the full row so a wrong tint can be diagnosed
    // against the geology cache directly.
    {
        uint8_t bslot = dsgn.bits.biome;
        out.print("  region_offset[{}]", (int)bslot);
        if (bslot < 9) {
            int resolved = block->region_offset[bslot];
            out.print(" = {}", resolved);
            if (resolved >= 0 && (size_t)resolved < layer_mats.size()) {
                auto &row = layer_mats[resolved];
                int gl = dsgn.bits.geolayer_index;
                if (gl >= 0 && (size_t)gl < row.size()) {
                    int m = row[gl];
                    const char *name = "?";
                    if (m >= 0 && (size_t)m < world->raws.inorganics.all.size())
                        name = world->raws.inorganics.all[m]->id.c_str();
                    out.print("  -> layer_mats[{}][{}] = {} ({})",
                              resolved, gl, m, name);
                }
            }
        } else {
            out.print(" (out of range)");
        }
        out.print("\n");
        // Full row of region_offset for context (only 9 entries).
        out.print("  region_offset = [");
        for (int i = 0; i < 9; ++i)
            out.print("{}{}", (int)block->region_offset[i], i == 8 ? "" : ",");
        out.print("]\n");
    }

    // Block-level feature indices.
    out.print("  block: region_pos=({},{}), local_feature={}, "
              "global_feature={}\n",
              (int)block->region_pos.x, (int)block->region_pos.y,
              block->local_feature, block->global_feature);

    // If this tile claims to be a feature tile, resolve the feature's
    // material via the same path MapCache.cpp uses internally.
    int16_t feat_mat_type = -1;
    int32_t feat_mat_index = -1;
    if (dsgn.bits.feature_local && block->local_feature != -1) {
        auto *fi = Maps::getLocalInitFeature(block->region_pos,
                                             block->local_feature);
        if (fi) fi->getMaterial(&feat_mat_type, &feat_mat_index);
        out.print("  feature(local): init={}  -> mat_type={} mat_index={}",
                  (void *)fi, feat_mat_type, feat_mat_index);
    } else if (dsgn.bits.feature_global && block->global_feature != -1) {
        auto *fi = Maps::getGlobalInitFeature(block->global_feature);
        if (fi) fi->getMaterial(&feat_mat_type, &feat_mat_index);
        out.print("  feature(global): init={}  -> mat_type={} mat_index={}",
                  (void *)fi, feat_mat_type, feat_mat_index);
    } else {
        out.print("  feature: (none)");
    }
    if (feat_mat_index >= 0 &&
        (size_t)feat_mat_index < world->raws.inorganics.all.size())
        out.print(" ({})",
                  world->raws.inorganics.all[feat_mat_index]->id);
    out.print("\n");

    int mat = get_tile_mat(block, tx, ty, *tt);
    if (mat >= 0 && (size_t)mat < world->raws.inorganics.all.size()) {
        out.print("  get_tile_mat -> mat_id={} ({})\n",
                  mat, world->raws.inorganics.all[mat]->id);
        auto &m = world->raws.inorganics.all[mat]->material;
        out.print("  mat_rgb=({:.3f},{:.3f},{:.3f})  -> as 0-255: "
                  "({:3},{:3},{:3})\n",
                  m.mat_rgb[0], m.mat_rgb[1], m.mat_rgb[2],
                  (int)(m.mat_rgb[0] * 255 + 0.5f),
                  (int)(m.mat_rgb[1] * 255 + 0.5f),
                  (int)(m.mat_rgb[2] * 255 + 0.5f));
    } else {
        out.print("  get_tile_mat -> (unresolved)\n");
    }
}


// ---------------------------------------------------------------------------
// Dump a histogram of pixel colors at the given texpos. Useful for inspecting
// dynamically-generated textures referenced from vp->screentexpos_background_two
// (and friends) without saving to disk: a small distinct-color count plus a
// recognizable palette tells us at a glance whether the texture is a per-
// material tinted overlay vs. some other kind of sprite.
static void dump_texture(color_ostream &out, int texpos) {
    if (!enabler) {
        out.printerr("enabler not available\n");
        return;
    }
    if (texpos <= 0 ||
        (size_t)texpos >= enabler->textures.raws.size()) {
        out.printerr("texpos {} out of range\n", texpos);
        return;
    }
    SDL_Surface *src = (SDL_Surface *)enabler->textures.raws[texpos];
    if (!src) {
        out.printerr("no surface at texpos {}\n", texpos);
        return;
    }
    SDL_PixelFormat *fmt = DFSDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!fmt) return;
    SDL_Surface *conv = DFSDL_ConvertSurface(src, fmt, 0);
    if (!conv) return;
    int w = conv->w, h = conv->h;
    std::unordered_map<uint32_t, int> hist;
    for (int y = 0; y < h; y++) {
        uint8_t *row = (uint8_t *)conv->pixels + y * conv->pitch;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + x * 4;
            hist[rgba(p[0], p[1], p[2], p[3])]++;
        }
    }
    DFSDL_FreeSurface(conv);
    std::vector<std::pair<uint32_t, int>> sorted(hist.begin(), hist.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<uint32_t, int> &a,
                 const std::pair<uint32_t, int> &b) {
                  return a.second > b.second;
              });
    int transparent = 0, translucent = 0;
    for (auto &kv : sorted) {
        int a = (int)((kv.first >> 24) & 0xff);
        if (a == 0) transparent += kv.second;
        else if (a < 255) translucent += kv.second;
    }
    out.print("texpos {}: {}x{}\n", texpos, w, h);
    out.print("  distinct RGBA colors: {}   "
              "fully-transparent: {}   translucent: {}   total: {}\n",
              sorted.size(), transparent, translucent, w * h);
    out.print("  top colors (count, R,G,B,A):\n");
    size_t N = std::min<size_t>(sorted.size(), 16);
    for (size_t i = 0; i < N; i++) {
        uint32_t c = sorted[i].first;
        out.print("    {:5} : ({:3},{:3},{:3},{:3})\n",
                  sorted[i].second,
                  (int)(c & 0xff), (int)((c >> 8) & 0xff),
                  (int)((c >> 16) & 0xff), (int)((c >> 24) & 0xff));
    }
}

// ---------------------------------------------------------------------------
// Lua API: argument parsing and dispatch live in plugins/lua/cavern-colors.lua;
// these are the typed setters and queries it calls into.

static void print_status(color_ostream &out) {
    out.print("Current mode:     {}\n", mode_name(color_mode));
    out.print("Brightness boost: {}\n", brightness_boost);
    out.print("Tint strength:    {}\n", tint_strength);
    out.print("Enabled:          {}\n", is_enabled ? "yes" : "no");
    auto dir_label = [](const directional_overlay &o) {
        if (!o.valid) return std::string("-");
        // source_texpos here holds the floors.png cell id we stamped at
        // load: col*100 + row (1-based).
        int col = o.source_texpos / 100;
        int row = o.source_texpos % 100;
        return fmt::format("c{}r{}", col, row);
    };
    out.print("Rough edges:      {} "
              "({} composite(s) cached, fringes "
              "S:{} W:{} E:{} N:{} "
              "NE:{} SE:{} SW:{} NW:{})\n",
              rough_edges_enabled ? "on" : "off",
              composite_cache.size(),
              dir_label(overlay_by_dir[0]),
              dir_label(overlay_by_dir[1]),
              dir_label(overlay_by_dir[2]),
              dir_label(overlay_by_dir[3]),
              dir_label(overlay_by_dir[4]),
              dir_label(overlay_by_dir[5]),
              dir_label(overlay_by_dir[6]),
              dir_label(overlay_by_dir[7]));
    out.print("Z-fog tinting:    {}\n", z_fog_enabled ? "on" : "off");
}

// Returns true on success. Lua side validates the string against the allowed
// names before calling, so an unknown value here is a programming bug.
static bool set_mode(color_ostream &out, std::string name) {
    ColorMode new_mode;
    if      (name == "hybrid")      new_mode = ColorMode::hybrid;
    else if (name == "mat_rgb")     new_mode = ColorMode::mat_rgb;
    else if (name == "basic_color") new_mode = ColorMode::basic_color;
    else {
        out.printerr("Unknown mode '{}'\n", name);
        return false;
    }
    if (new_mode != color_mode) {
        color_mode = new_mode;
        clear_tinted_cache();
        clear_composite_cache();
        build_material_tints();
        out.print("cavern-colors mode set to '{}'\n", name);
    }
    return true;
}

static void set_boost(color_ostream &out, double v) {
    brightness_boost = (float)std::max(v, 0.0);
    clear_tinted_cache();
    clear_composite_cache();
    out.print("cavern-colors brightness boost set to {}\n", brightness_boost);
}

static void set_strength(color_ostream &out, double v) {
    tint_strength = (float)std::clamp(v, 0.0, 1.0);
    clear_tinted_cache();
    clear_composite_cache();
    out.print("cavern-colors tint strength set to {}\n", tint_strength);
}

static void set_rough_edges(color_ostream &out, bool on) {
    rough_edges_enabled = on;
    if (!on) {
        clear_composite_cache();
        out.print("rough-edge tinting: off "
                  "(composite cache cleared; rough edges will show DF's "
                  "default untinted overlay until you scroll past them so "
                  "DF re-paints)\n");
    } else {
        out.print("rough-edge tinting: on\n");
    }
}

static void set_z_fog(color_ostream &out, bool on) {
    z_fog_enabled = on;
    out.print("z-fog tinting: {}\n", on ? "on" : "off");
}

DFHACK_PLUGIN_LUA_FUNCTIONS {
    DFHACK_LUA_FUNCTION(print_status),
    DFHACK_LUA_FUNCTION(set_mode),
    DFHACK_LUA_FUNCTION(set_boost),
    DFHACK_LUA_FUNCTION(set_strength),
    DFHACK_LUA_FUNCTION(set_rough_edges),
    DFHACK_LUA_FUNCTION(set_z_fog),
    DFHACK_LUA_FUNCTION(sample_cell),
    DFHACK_LUA_FUNCTION(dump_texture),
    DFHACK_LUA_END
};

// ---------------------------------------------------------------------------
// Plugin lifecycle

static command_result do_command(color_ostream &out,
                                 std::vector<std::string> &parameters) {
    bool ok = false;
    if (!Lua::CallLuaModuleFunction(out, "plugins.cavern-colors",
            "parse_commandline", std::make_tuple(parameters),
            1, [&](lua_State *L) { ok = lua_toboolean(L, 1); })) {
        return CR_FAILURE;
    }
    return ok ? CR_OK : CR_WRONG_USAGE;
}

DFhackCExport command_result plugin_init(color_ostream &out,
                                         std::vector<PluginCommand> &commands)
{
    commands.push_back(PluginCommand(
        plugin_name,
        "Restore per-mineral floor colors in premium graphics mode.",
        do_command));
    return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {
    if (!INTERPOSE_HOOK(cavern_colors_hook, render).apply(enable))
        return CR_FAILURE;
    is_enabled = enable;
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    clear_tinted_cache();
    return CR_OK;
}

DFhackCExport void plugin_onstatechange(color_ostream &out, state_change_event event) {
    switch (event) {
    case SC_WORLD_LOADED:
        build_material_tints();
        build_geology();
        snapshot_overlays();
        break;
    case SC_WORLD_UNLOADED:
        clear_tinted_cache();
        clear_composite_cache();
        material_tints.clear();
        layer_mats.clear();
        for (auto &o : overlay_by_dir) o = {};
        break;
    default:
        break;
    }
}
