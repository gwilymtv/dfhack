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
#include "PluginManager.h"
#include "TileTypes.h"
#include "VTableInterpose.h"

#include "modules/Gui.h"
#include "modules/Maps.h"
#include "modules/Screen.h"
#include "modules/Textures.h"

#include "modules/DFSDL.h"

#include "df/block_square_event.h"
#include "df/block_square_event_mineralst.h"
#include "df/boulder_floor_graphics_infost.h"
#include "df/descriptor_handlerst.h"
#include "df/enabler.h"
#include "df/feature_init.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/inorganic_raw.h"
#include "df/map_block.h"
#include "df/renderer_2d_base.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/wall_graphics_infost.h"
#include "df/world.h"

#include <SDL_pixels.h>
#include <SDL_surface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
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

enum class ColorMode { hybrid, mat_rgb, basic_color, build_color, tile_color };
static ColorMode color_mode = ColorMode::hybrid;

static const char *mode_name(ColorMode m) {
    switch (m) {
    case ColorMode::hybrid:      return "hybrid";
    case ColorMode::mat_rgb:     return "mat_rgb";
    case ColorMode::basic_color: return "basic_color";
    case ColorMode::build_color: return "build_color";
    case ColorMode::tile_color:  return "tile_color";
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

// build_color and tile_color are [fg, bg, bright] triples. We only sample the
// foreground for tinting; the background slot would only matter if we wanted
// to render the tile's ASCII glyph composite.
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
    switch (color_mode) {
    case ColorMode::build_color:
        palette_tint(mat.build_color[0], mat.build_color[2], tr, tg, tb);
        return true;
    case ColorMode::tile_color:
        palette_tint(mat.tile_color[0], mat.tile_color[2], tr, tg, tb);
        return true;
    default:
        // hybrid fallback and basic_color mode both use basic_color
        palette_tint(mat.basic_color[0], mat.basic_color[1], tr, tg, tb);
        return true;
    }
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
static float tint_strength = 1.0f;

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
        // Dump every color field DF carries on a material so they can be
        // compared/experimented with in an image editor. mat_rgb is the
        // modern premium float RGB (0..1). basic_color is [fg, bright] into
        // the 16-color CGA palette. build_color/tile_color are [fg, bg, bright]
        // triples used by the legacy renderer for constructed items and tiles.
        int bc_idx = std::clamp((int)m.basic_color[0], 0, 7) + (m.basic_color[1] ? 8 : 0);
        DEBUG(log).print(
            "material {}: tint={}/{}/{} mat_rgb={:.3f}/{:.3f}/{:.3f} "
            "basic_color=[{},{}] (palette#{} -> {}/{}/{}) "
            "build_color=[{},{},{}] tile_color=[{},{},{}]\n",
            inorganics[i]->id,
            material_tints[i][0], material_tints[i][1], material_tints[i][2],
            m.mat_rgb[0], m.mat_rgb[1], m.mat_rgb[2],
            m.basic_color[0], m.basic_color[1],
            bc_idx, DF_PALETTE[bc_idx][0], DF_PALETTE[bc_idx][1], DF_PALETTE[bc_idx][2],
            m.build_color[0], m.build_color[1], m.build_color[2],
            m.tile_color[0], m.tile_color[1], m.tile_color[2]);
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
// Rough-edge bleed ("leaks")
//
// DF composites a per-side overlay onto cells adjacent to rough cavern
// tiles. The overlay sprite is read from
// world->raws.descriptors.wall_graphics_info — despite the name, this
// table holds both full rough-cavern wall sprites *and* the rough-edge
// fragments used to draw leaks onto adjacent floor cells. Every entry's
// texpos points at a 32×32 RGBA sprite already baked with the embark's
// primary cavern material palette (in practice shale), regardless of who
// the actual rough source is on the map. Result: every leak renders in
// that one baked palette.
//
// The neighbour (leak-receiver) cell selects which overlay to composite
// via screentexpos_floor_flag[idx], a uint64 packed as one byte per
// cardinal:
//
//   byte 0 (bits  0-7):  S
//   byte 1 (bits  8-15): W
//   byte 2 (bits 16-23): E
//   byte 3 (bits 24-31): N
//
// Within each byte: bits 0-2 = texture_index, bit 3 = enable. Bytes 4-7
// unobserved (likely diagonals).
//
// To find which wall_graphics_info entry DF uses for a given (side, byte)
// pair we exploit the observed layout of the table's own 64-bit flags:
//
//   bits 0-7:   matches the floor_flag byte value (texture_index | enable)
//   bits 20-23: 0..3, hypothesised to encode side (0=S, 1=W, 2=E, 3=N to
//               start; permute if shapes render mis-rotated)
//
// DF's renderer uploads each overlay sprite to GPU at world-load and
// ignores subsequent SDL_Surface edits (verified with paint-overlay), so
// we can't tint in place. Fix: bake a composite per cell — base sprite
// tinted by base mat plus alpha-blended (overlay re-tinted by rough
// neighbour mat), register via Textures::createTile, replace
// screentexpos_background[idx], zero floor_flag[idx] to suppress DF's
// untinted overlay redraw.

static bool leaks_enabled = true;

// Snapshot of one wall_graphics_info entry: pixels (RGBA32), original
// flag and texpos preserved for lookup and debugging.
struct overlay_snapshot {
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;
    uint64_t flag = 0;
    int32_t texpos = 0;
};
static std::vector<overlay_snapshot> overlay_snapshots;
// (side << 8) | byte_value → first matching snapshot index. Same key
// shape DF uses to look up an entry from the cell's floor_flag.
static std::unordered_map<int, int> overlay_lookup;
// Last observed size of wall_graphics_info. DF populates the table at
// world-load but we re-check each frame in case it grows.
static size_t overlay_snapshot_table_size = 0;

struct rough_side_info {
    int dx, dy;
    int byte_offset;       // floor_flag byte index
    int wall_graphics_side; // wall_graphics_info flag bits 20-23 value
};
// Mapping hypothesis: wall_graphics_info side bits 20-23 use same order
// as our floor_flag bytes (0=S, 1=W, 2=E, 3=N). If leak shapes appear
// mirrored or rotated when this lands, permute the wall_graphics_side
// field below.
static const std::array<rough_side_info, 4> ROUGH_SIDES = {{
    { 0, +1, 0, 0}, // S
    {-1,  0, 1, 1}, // W
    {+1,  0, 2, 2}, // E
    { 0, -1, 3, 3}, // N
}};

// Composite cache key. (base_texpos, floor_flag, per-side rough neighbour
// mats, base mat) uniquely determines the composite output.
struct composite_key {
    int32_t base_texpos;
    uint64_t floor_flag;
    int16_t base_mat;
    int16_t mat_s, mat_w, mat_e, mat_n;
    bool operator==(const composite_key &o) const {
        return base_texpos == o.base_texpos && floor_flag == o.floor_flag &&
               base_mat == o.base_mat &&
               mat_s == o.mat_s && mat_w == o.mat_w &&
               mat_e == o.mat_e && mat_n == o.mat_n;
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
        mix((uint16_t)k.mat_s);
        mix((uint16_t)k.mat_w);
        mix((uint16_t)k.mat_e);
        mix((uint16_t)k.mat_n);
        return h;
    }
};
static std::unordered_map<composite_key, TexposHandle, composite_key_hash>
    composite_cache;

static void snapshot_overlays() {
    overlay_snapshots.clear();
    overlay_lookup.clear();
    overlay_snapshot_table_size = 0;
    if (!world || !enabler) return;
    auto &table = world->raws.descriptors.wall_graphics_info;
    overlay_snapshot_table_size = table.size();
    overlay_snapshots.reserve(table.size());
    int n = 0;
    for (auto *info : table) {
        if (!info) continue;
        if (info->texpos <= 0 ||
            (size_t)info->texpos >= enabler->textures.raws.size()) continue;
        SDL_Surface *src =
            (SDL_Surface *)enabler->textures.raws[info->texpos];
        if (!src) continue;
        SDL_PixelFormat *fmt = DFSDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
        if (!fmt) continue;
        SDL_Surface *conv = DFSDL_ConvertSurface(src, fmt, 0);
        if (!conv) continue;
        overlay_snapshot snap;
        snap.w = conv->w;
        snap.h = conv->h;
        snap.flag = info->flags.whole;
        snap.texpos = info->texpos;
        snap.pixels.assign((size_t)snap.w * snap.h, 0);
        for (int y = 0; y < snap.h; y++) {
            uint8_t *row = (uint8_t *)conv->pixels + y * conv->pitch;
            memcpy(&snap.pixels[(size_t)y * snap.w], row, (size_t)snap.w * 4);
        }
        DFSDL_FreeSurface(conv);
        // Index by (side bits 20-23, low byte). First match wins; the
        // table commonly has multiple variants per key, and we have no
        // signal for picking among them, so deterministically pick the
        // first encountered.
        int side = (int)((snap.flag >> 20) & 0xf);
        int byte_value = (int)(snap.flag & 0xff);
        int key = (side << 8) | byte_value;
        if (!overlay_lookup.count(key))
            overlay_lookup[key] = (int)overlay_snapshots.size();
        overlay_snapshots.push_back(std::move(snap));
        n++;
    }
    color_ostream_proxy c(Core::getInstance().getConsole());
    c.print("[cavern-colors] snapshotted {} wall overlay sprite(s) "
            "from a table of {} ({} unique (side, byte) key(s))\n",
            n, table.size(), overlay_lookup.size());
}

static void clear_composite_cache();

// Re-snapshot if the table has grown since the last snapshot. Called from
// the render hook so we pick up overlays DF populates lazily.
static void maybe_resnapshot_overlays() {
    if (!world) return;
    size_t sz = world->raws.descriptors.wall_graphics_info.size();
    if (sz != overlay_snapshot_table_size) {
        snapshot_overlays();
        // Any composites built while the snapshot was empty produced
        // un-leaked sprites; drop them so we re-bake with proper overlays.
        clear_composite_cache();
    }
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

// layer_mats[biome_idx][geolayer_idx] -> inorganic_mat. Built once per world.
static std::vector<std::vector<int16_t>> layer_mats;

static void build_geology() {
    layer_mats.clear();
    std::vector<df::coord2d> geoidx;
    Maps::ReadGeology(&layer_mats, &geoidx);
    DEBUG(log).print("loaded geology for {} biomes\n", layer_mats.size());
}

static int get_vein_mat(df::map_block *block, int tx, int ty) {
    int last = -1;
    for (auto *ev : block->block_events) {
        if (ev->getType() != df::block_square_event_type::mineral) continue;
        auto *m = (df::block_square_event_mineralst *)ev;
        if (m->getassignment(tx, ty)) last = m->inorganic_mat;
    }
    return last;
}

static int get_layer_mat(df::map_block *block, int tx, int ty) {
    auto &des = block->designation[tx][ty];
    // For subterranean tiles, override the per-tile biome bits with eHere
    // (=4, the embark's home region). DF's display uses the home-region
    // geology for cavern/cave stone regardless of per-tile biome attribution,
    // so per-tile bits at biome boundaries can mis-assign cavern floors to a
    // neighbouring region's layer material (e.g. LIMESTONE in the south
    // neighbour where the home region is SHALE). The per-tile bits remain
    // authoritative for above-ground tiles where surface biome actually
    // varies meaningfully.
    int biome = des.bits.subterranean ? 4 /* eHere */ : des.bits.biome;
    int geolayer = des.bits.geolayer_index;
    if (biome < 0 || (size_t)biome >= layer_mats.size()) return -1;
    auto &row = layer_mats[biome];
    if (geolayer < 0 || (size_t)geolayer >= row.size()) return -1;
    return row[geolayer];
}

static int get_tile_mat(df::map_block *block, int tx, int ty, df::tiletype tt) {
    switch (tileMaterial(tt)) {
    case df::tiletype_material::MINERAL:    return get_vein_mat(block, tx, ty);
    case df::tiletype_material::STONE:      return get_layer_mat(block, tx, ty);
    case df::tiletype_material::LAVA_STONE: return get_vein_mat(block, tx, ty);
    default:                                return -1;
    }
}

// ---------------------------------------------------------------------------
// Investigation: walk visible wall tiles and record (material -> {texpos})
// so we can later filter DF's tile_cache to just the entries DF rendered for
// natural stone walls. With that filter we can read out the (fg, bg) color
// pairs DF actually passes to its renderer per material, which is information
// the material struct doesn't carry directly.

static bool collect_walls = false;
static std::unordered_map<int /* inorganic_mat */,
                          std::unordered_set<int32_t /* texpos */>>
    wall_texposes_by_mat;
// Per-material wall *overlay* texposes, harvested from
// vp->screentexpos_background_two. These are the dynamically-generated 32x32
// sprites DF pre-renders at world-load, one per material. Their pixel
// palettes are DF's authoritative per-material color ramp.
static std::unordered_map<int /* inorganic_mat */,
                          std::unordered_set<int32_t /* texpos */>>
    wall_overlay_texposes_by_mat;

// ---------------------------------------------------------------------------
// Per-material palette cache. Each entry is a list of (RGBA, pixel-count)
// pairs sorted by alpha-asc then count-desc, matching extract-palette output.
//
// Population strategy:
//   1. At world-load, seed palette_by_mat from BAKED (compile-time table built
//      from prior observations of vanilla DF inorganics).
//   2. The render hook reads a material's actual overlay sprites the first
//      time it sees each (material, overlay_texpos) pair, and unions the
//      observed colors into palette_by_mat[mat]. wall_stone.png provides
//      multiple shape variants per material; accumulating the union across
//      variants captures the material's full color palette regardless of
//      which variants happen to render first.
//   3. When a variant adds colors not yet represented in the cache (i.e.
//      neither baked nor previously observed this session), a one-liner
//      is printed to the console so a maintainer knows to refresh BAKED
//      from the live cache via `dump-baked`.
//
// Floor tinting still uses material_tints (the simple RGB) for now; the
// palette cache is built but not yet consumed by the tinting math.
using palette_t = std::vector<std::pair<uint32_t /* rgba */, int /* count */>>;

#define BC(r, g, b, a) \
    ((uint32_t)(r) | ((uint32_t)(g) << 8) | \
     ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

static const std::unordered_map<std::string, palette_t> BAKED = {
    {"ALEXANDRITE", {
        {BC(0,0,0,0),929}, {BC(81,73,85,71),44}, {BC(100,90,103,168),21},
        {BC(78,46,76,255),16}, {BC(139,127,140,255),6}, {BC(189,211,214,255),4},
        {BC(119,79,119,255),2}, {BC(209,179,208,255),1}, {BC(169,127,170,255),1},
    }},
    {"BAUXITE", {
        {BC(0,0,0,0),29073}, {BC(112,60,50,71),7117}, {BC(120,70,60,168),5131},
        {BC(149,76,60,255),2890}, {BC(202,102,76,255),792}, {BC(227,151,130,255),53},
    }},
    {"BLACK PYROPE", {
        {BC(0,0,0,0),14053}, {BC(119,94,118,71),2277}, {BC(140,109,138,168),1338},
        {BC(40,33,44,255),1955}, {BC(174,132,172,255),610}, {BC(41,40,44,255),463},
        {BC(111,109,104,255),385}, {BC(199,173,198,255),207}, {BC(164,168,171,255),203},
        {BC(233,218,229,255),13},
    }},
    {"BLACK ZIRCON", {
        {BC(0,0,0,0),2045}, {BC(86,86,89,71),277}, {BC(104,104,106,168),158},
        {BC(40,33,44,255),320}, {BC(131,131,130,255),88}, {BC(41,40,44,255),76},
        {BC(111,109,104,255),56}, {BC(164,168,171,255),27}, {BC(191,186,175,255),24},
        {BC(241,241,240,255),1},
    }},
    {"BROWN JASPER", {
        {BC(0,0,0,0),12858}, {BC(108,78,65,71),1088}, {BC(64,56,50,71),894},
        {BC(134,110,90,168),781}, {BC(74,63,55,168),556}, {BC(59,48,53,255),1002},
        {BC(161,141,112,255),303}, {BC(99,85,73,255),300}, {BC(96,74,72,255),218},
        {BC(176,154,129,255),156}, {BC(204,192,155,255),86}, {BC(210,196,170,255),81},
        {BC(134,114,97,255),77}, {BC(198,184,165,255),18}, {BC(199,184,158,255),7},
        {BC(229,221,196,255),6}, {BC(230,220,207,255),1},
    }},
    {"BROWN ZIRCON", {
        {BC(0,0,0,0),12366}, {BC(79,90,103,71),1724}, {BC(143,145,154,71),725},
        {BC(105,111,135,168),1036}, {BC(158,161,169,168),404}, {BC(63,51,56,255),2127},
        {BC(157,153,148,255),514}, {BC(115,92,89,255),512}, {BC(192,172,150,255),433},
        {BC(220,209,187,255),218}, {BC(179,182,191,255),171}, {BC(222,227,225,255),165},
        {BC(227,227,228,255),68}, {BC(255,255,255,255),17},
    }},
    {"CARNELIAN", {
        {BC(0,0,0,0),2123}, {BC(79,90,103,71),346}, {BC(105,111,135,168),257},
        {BC(79,47,42,255),149}, {BC(157,153,148,255),94}, {BC(222,227,225,255),34},
        {BC(127,74,48,255),29}, {BC(198,157,99,255),27}, {BC(228,191,138,255),11},
        {BC(255,255,255,255),2},
    }},
    {"CHALK", {
        {BC(0,0,0,0),79902}, {BC(108,78,65,71),23218}, {BC(134,110,90,168),16466},
        {BC(161,141,112,255),9702}, {BC(198,184,165,255),2630}, {BC(230,220,207,255),178},
    }},
    {"CITRINE", {
        {BC(0,0,0,0),6988}, {BC(65,48,49,71),712}, {BC(64,56,50,71),406},
        {BC(70,51,51,168),449}, {BC(74,63,55,168),287}, {BC(86,52,49,255),622},
        {BC(91,63,55,255),223}, {BC(153,126,100,255),142}, {BC(99,85,73,255),127},
        {BC(209,195,176,255),118}, {BC(145,107,70,255),64}, {BC(243,232,218,255),54},
        {BC(134,114,97,255),44}, {BC(202,172,119,255),3}, {BC(199,184,158,255),1},
    }},
    {"CLAYSTONE", {
        {BC(0,0,0,0),63237}, {BC(65,48,49,71),18291}, {BC(70,51,51,168),13079},
        {BC(91,63,55,255),7625}, {BC(145,107,70,255),2075}, {BC(202,172,119,255),141},
    }},
    {"CLEAR TOURMALINE", {
        {BC(0,0,0,0),28824}, {BC(81,73,85,71),6314}, {BC(80,59,55,71),656},
        {BC(100,90,103,168),4152}, {BC(88,66,61,168),455}, {BC(54,49,60,255),4559},
        {BC(139,127,140,255),2123}, {BC(189,211,214,255),1385}, {BC(100,90,103,255),1062},
        {BC(238,244,240,255),399}, {BC(108,72,65,255),186}, {BC(162,95,80,255),58},
        {BC(215,154,140,255),3},
    }},
    {"CRYSTAL_ROCK", {
        {BC(0,0,0,0),11509}, {BC(143,145,154,71),2224}, {BC(108,78,65,71),101},
        {BC(158,161,169,168),1500}, {BC(134,110,90,168),61}, {BC(54,49,60,255),1404},
        {BC(179,182,191,255),687}, {BC(100,90,103,255),317}, {BC(189,211,214,255),239},
        {BC(227,227,228,255),215}, {BC(238,244,240,255),111}, {BC(161,141,112,255),42},
        {BC(204,192,155,255),12}, {BC(255,255,255,255),10},
    }},
    {"DIAMOND_BLACK", {
        {BC(0,0,0,0),3214}, {BC(119,94,118,71),183}, {BC(143,145,154,71),146},
        {BC(81,73,85,71),87}, {BC(80,59,55,71),86}, {BC(140,109,138,168),97},
        {BC(158,161,169,168),79}, {BC(100,90,103,168),49}, {BC(88,66,61,168),39},
        {BC(40,33,44,255),617}, {BC(41,40,44,255),166}, {BC(111,109,104,255),134},
        {BC(164,168,171,255),72}, {BC(179,182,191,255),32}, {BC(174,132,172,255),28},
        {BC(108,72,65,255),27}, {BC(139,127,140,255),24}, {BC(199,173,198,255),18},
        {BC(227,227,228,255),10}, {BC(189,211,214,255),6}, {BC(162,95,80,255),5},
        {BC(215,154,140,255),1},
    }},
    {"DIAMOND_BLUE", {
        {BC(0,0,0,0),5372}, {BC(119,94,118,71),721}, {BC(81,73,85,71),87},
        {BC(140,109,138,168),380}, {BC(100,90,103,168),49}, {BC(48,44,72,255),862},
        {BC(72,78,127,255),219}, {BC(143,154,184,255),181}, {BC(174,132,172,255),133},
        {BC(194,199,207,255),102}, {BC(199,173,198,255),55}, {BC(139,127,140,255),24},
        {BC(189,211,214,255),6}, {BC(233,218,229,255),1},
    }},
    {"DIAMOND_CLEAR", {
        {BC(0,0,0,0),3475}, {BC(143,145,154,71),398}, {BC(119,94,118,71),368},
        {BC(158,161,169,168),214}, {BC(140,109,138,168),187}, {BC(54,49,60,255),815},
        {BC(100,90,103,255),210}, {BC(189,211,214,255),174}, {BC(238,244,240,255),106},
        {BC(174,132,172,255),81}, {BC(179,182,191,255),56}, {BC(227,227,228,255),30},
        {BC(199,173,198,255),29}, {BC(233,218,229,255),1},
    }},
    {"DIAMOND_FY", {
        {BC(0,0,0,0),2629}, {BC(119,94,118,71),432}, {BC(140,109,138,168),259},
        {BC(75,47,46,255),397}, {BC(134,110,90,255),103}, {BC(174,132,172,255),98},
        {BC(198,184,165,255),91}, {BC(199,173,198,255),45}, {BC(230,220,207,255),39},
        {BC(233,218,229,255),3},
    }},
    {"DIAMOND_GREEN", {
        {BC(0,0,0,0),3782}, {BC(119,94,118,71),287}, {BC(81,73,85,71),124},
        {BC(140,109,138,168),148}, {BC(100,90,103,168),68}, {BC(49,59,40,255),385},
        {BC(86,91,44,255),94}, {BC(141,172,80,255),75}, {BC(174,132,172,255),55},
        {BC(176,215,140,255),44}, {BC(139,127,140,255),32}, {BC(199,173,198,255),18},
        {BC(189,211,214,255),7}, {BC(233,218,229,255),1},
    }},
    {"DIAMOND_RED", {
        {BC(0,0,0,0),1880}, {BC(119,94,118,71),228}, {BC(81,73,85,71),124},
        {BC(140,109,138,168),125}, {BC(100,90,103,168),68}, {BC(77,37,45,255),357},
        {BC(116,63,60,255),94}, {BC(205,72,53,255),60}, {BC(174,132,172,255),46},
        {BC(231,154,145,255),37}, {BC(139,127,140,255),32}, {BC(199,173,198,255),14},
        {BC(189,211,214,255),7},
    }},
    {"DIAMOND_YELLOW", {
        {BC(0,0,0,0),3782}, {BC(119,94,118,71),533}, {BC(81,73,85,71),104},
        {BC(140,109,138,168),285}, {BC(100,90,103,168),57}, {BC(85,67,40,255),755},
        {BC(138,125,63,255),196}, {BC(229,204,87,255),154}, {BC(174,132,172,255),132},
        {BC(251,240,120,255),82}, {BC(199,173,198,255),42}, {BC(139,127,140,255),14},
        {BC(189,211,214,255),7}, {BC(233,218,229,255),1},
    }},
    {"DIORITE", {
        {BC(0,0,0,0),108879}, {BC(79,90,103,71),37275}, {BC(105,111,135,168),26200},
        {BC(157,153,148,255),15658}, {BC(222,227,225,255),4219}, {BC(255,255,255,255),281},
    }},
    {"EMERALD", {
        {BC(0,0,0,0),4209}, {BC(143,145,154,71),275}, {BC(81,73,85,71),254},
        {BC(119,94,118,71),191}, {BC(80,59,55,71),132}, {BC(100,90,103,168),152},
        {BC(158,161,169,168),147}, {BC(140,109,138,168),105}, {BC(88,66,61,168),74},
        {BC(40,45,49,255),843}, {BC(56,82,59,255),221}, {BC(95,164,98,255),186},
        {BC(154,211,156,255),93}, {BC(179,182,191,255),56}, {BC(174,132,172,255),55},
        {BC(108,72,65,255),51}, {BC(139,127,140,255),44}, {BC(189,211,214,255),25},
        {BC(227,227,228,255),20}, {BC(199,173,198,255),17}, {BC(162,95,80,255),14},
        {BC(233,218,229,255),2}, {BC(238,244,240,255),1}, {BC(215,154,140,255),1},
    }},
    {"GABBRO", {
        {BC(0,0,0,0),118262}, {BC(39,39,43,68),338}, {BC(86,86,89,71),47071},
        {BC(104,104,106,168),33167}, {BC(58,58,62,168),232}, {BC(131,131,130,255),19376},
        {BC(191,186,175,255),5270}, {BC(241,241,240,255),345}, {BC(72,72,75,255),153},
        {BC(86,86,89,255),39}, {BC(104,104,106,255),3},
    }},
    {"GALENA", {
        {BC(0,0,0,0),136593}, {BC(159,175,175,64),39209}, {BC(161,172,175,166),30459},
        {BC(222,227,225,255),20868}, {BC(105,111,135,255),15739}, {BC(163,176,178,255),15685},
        {BC(255,255,255,255),14320}, {BC(184,188,195,255),13161}, {BC(79,90,103,255),4306},
        {BC(227,225,212,255),3943}, {BC(157,153,148,255),3310}, {BC(250,250,250,255),391},
    }},
    {"GOLDEN BERYL", {
        {BC(0,0,0,0),3712}, {BC(143,145,154,71),729}, {BC(158,161,169,168),408},
        {BC(65,48,51,255),668}, {BC(179,182,191,255),203}, {BC(117,85,61,255),160},
        {BC(228,207,78,255),129}, {BC(227,227,228,255),66}, {BC(243,247,149,255),65},
        {BC(255,255,255,255),4},
    }},
    {"GOSHENITE", {
        {BC(0,0,0,0),9518}, {BC(143,145,154,71),1749}, {BC(158,161,169,168),1041},
        {BC(54,49,60,255),1577}, {BC(179,182,191,255),462}, {BC(100,90,103,255),375},
        {BC(189,211,214,255),306}, {BC(227,227,228,255),174}, {BC(238,244,240,255),150},
        {BC(255,255,255,255),8},
    }},
    {"GRANITE", {
        {BC(0,0,0,0),232165}, {BC(81,73,85,71),108349}, {BC(100,90,103,168),76181},
        {BC(139,127,140,255),44540}, {BC(189,211,214,255),12084}, {BC(238,244,240,255),793},
    }},
    {"GREEN TOURMALINE", {
        {BC(0,0,0,0),4349}, {BC(143,145,154,71),817}, {BC(158,161,169,168),452},
        {BC(49,59,40,255),839}, {BC(179,182,191,255),211}, {BC(86,91,44,255),197},
        {BC(141,172,80,255),153}, {BC(227,227,228,255),80}, {BC(176,215,140,255),69},
        {BC(255,255,255,255),1},
    }},
    {"GREEN ZIRCON", {
        {BC(0,0,0,0),2045}, {BC(86,86,89,71),277}, {BC(104,104,106,168),158},
        {BC(49,59,40,255),320}, {BC(131,131,130,255),88}, {BC(86,91,44,255),76},
        {BC(141,172,80,255),56}, {BC(176,215,140,255),27}, {BC(191,186,175,255),24},
        {BC(241,241,240,255),1},
    }},
    {"HEMATITE", {
        {BC(0,0,0,0),36768}, {BC(159,175,175,64),5959}, {BC(161,172,175,166),4608},
        {BC(174,77,66,255),3152}, {BC(95,46,63,255),2624}, {BC(214,122,100,255),2237},
        {BC(150,51,53,255),2177}, {BC(163,176,178,255),2095}, {BC(91,47,63,255),719},
        {BC(227,225,212,255),515}, {BC(121,45,51,255),513}, {BC(250,250,250,255),73},
    }},
    {"HORNBLENDE", {
        {BC(0,0,0,0),7326}, {BC(86,86,89,71),1743}, {BC(104,104,106,168),1313},
        {BC(131,131,130,255),679}, {BC(191,186,175,255),190}, {BC(241,241,240,255),13},
    }},
    {"JASPER OPAL", {
        {BC(0,0,0,0),18849}, {BC(81,73,85,71),5344}, {BC(100,90,103,168),3638},
        {BC(60,43,57,255),3191}, {BC(139,127,140,255),1691}, {BC(95,46,63,255),728},
        {BC(174,77,66,255),567}, {BC(189,211,214,255),522}, {BC(214,122,100,255),258},
        {BC(238,244,240,255),28},
    }},
    {"KAOLINITE", {
        {BC(0,0,0,0),78683}, {BC(104,83,91,71),22492}, {BC(123,94,102,168),15725},
        {BC(163,121,123,255),9422}, {BC(218,194,191,255),2534}, {BC(235,219,213,255),168},
    }},
    {"LAPIS LAZULI", {
        {BC(0,0,0,0),34769}, {BC(81,73,85,71),8698}, {BC(86,86,89,71),214},
        {BC(100,90,103,168),5883}, {BC(104,104,106,168),143}, {BC(36,31,59,255),5887},
        {BC(139,127,140,255),2938}, {BC(36,39,97,255),1389}, {BC(36,58,157,255),1058},
        {BC(189,211,214,255),855}, {BC(66,82,171,255),484}, {BC(131,131,130,255),77},
        {BC(238,244,240,255),44}, {BC(191,186,175,255),25},
    }},
    {"LIMESTONE", {
        {BC(0,0,0,0),63322}, {BC(79,90,103,71),16914}, {BC(105,111,135,168),12092},
        {BC(157,153,148,255),7005}, {BC(222,227,225,255),1914}, {BC(255,255,255,255),129},
    }},
    {"MALACHITE", {
        {BC(0,0,0,0),42099}, {BC(159,175,175,64),7906}, {BC(161,172,175,166),5988},
        {BC(91,180,155,255),4176}, {BC(55,110,86,255),3340}, {BC(163,176,178,255),3009},
        {BC(101,243,204,255),2937}, {BC(77,161,135,255),2788}, {BC(45,99,76,255),950},
        {BC(227,225,212,255),805}, {BC(60,134,102,255),662}, {BC(250,250,250,255),92},
    }},
    {"MARBLE", {
        {BC(0,0,0,0),139082}, {BC(143,145,154,71),52796}, {BC(158,161,169,168),37112},
        {BC(179,182,191,255),21743}, {BC(227,227,228,255),5903}, {BC(255,255,255,255),388},
    }},
    {"MICA", {
        {BC(0,0,0,0),222339}, {BC(106,73,65,71),103113}, {BC(41,37,42,71),518},
        {BC(119,82,67,168),72342}, {BC(41,40,44,168),393}, {BC(153,105,62,255),42379},
        {BC(222,173,77,255),11520}, {BC(248,227,137,255),754}, {BC(46,47,48,255),212},
        {BC(111,109,104,255),58}, {BC(164,168,171,255),4},
    }},
    {"MICROCLINE", {
        {BC(0,0,0,0),169524}, {BC(72,97,120,71),59024}, {BC(41,37,42,71),9457},
        {BC(86,117,141,168),41370}, {BC(41,40,44,168),6844}, {BC(103,147,163,255),24434},
        {BC(166,194,206,255),6603}, {BC(46,47,48,255),3750}, {BC(111,109,104,255),1051},
        {BC(219,230,233,255),436}, {BC(164,168,171,255),67},
    }},
    {"MILK QUARTZ", {
        {BC(0,0,0,0),6216}, {BC(79,90,103,71),994}, {BC(105,111,135,168),625},
        {BC(86,52,49,255),640}, {BC(157,153,148,255),342}, {BC(153,126,100,255),154},
        {BC(209,195,176,255),96}, {BC(222,227,225,255),93}, {BC(243,232,218,255),50},
        {BC(255,255,255,255),6},
    }},
    {"MOONSTONE", {
        {BC(0,0,0,0),2820}, {BC(80,59,55,71),458}, {BC(88,66,61,168),327},
        {BC(74,58,51,255),205}, {BC(108,72,65,255),127}, {BC(205,198,182,255),49},
        {BC(162,95,80,255),45}, {BC(142,131,108,255),43}, {BC(233,228,217,255),21},
        {BC(215,154,140,255),1},
    }},
    {"MORION", {
        {BC(0,0,0,0),83085}, {BC(119,94,118,71),20917}, {BC(79,90,103,71),1240},
        {BC(80,59,55,71),153}, {BC(140,109,138,168),14048}, {BC(105,111,135,168),817},
        {BC(88,66,61,168),101}, {BC(40,33,44,255),14355}, {BC(174,132,172,255),7058},
        {BC(41,40,44,255),3355}, {BC(111,109,104,255),2507}, {BC(199,173,198,255),2066},
        {BC(164,168,171,255),1150}, {BC(157,153,148,255),395}, {BC(222,227,225,255),111},
        {BC(233,218,229,255),109}, {BC(108,72,65,255),62}, {BC(162,95,80,255),14},
        {BC(255,255,255,255),8}, {BC(215,154,140,255),1},
    }},
    {"NATIVE_GOLD", {
        {BC(0,0,0,0),28327}, {BC(159,175,175,64),3872}, {BC(161,172,175,166),2914},
        {BC(208,174,89,255),1975}, {BC(111,78,65,255),1638}, {BC(244,235,150,255),1389},
        {BC(179,136,77,255),1362}, {BC(163,176,178,255),1328}, {BC(97,69,62,255),481},
        {BC(141,103,65,255),356}, {BC(227,225,212,255),346}, {BC(250,250,250,255),44},
    }},
    {"ORTHOCLASE", {
        {BC(0,0,0,0),250979}, {BC(78,55,60,71),112223}, {BC(41,37,42,71),2861},
        {BC(86,56,60,168),78865}, {BC(41,40,44,168),1901}, {BC(109,62,59,255),46192},
        {BC(173,105,71,255),12569}, {BC(46,47,48,255),1161}, {BC(224,163,107,255),822},
        {BC(111,109,104,255),313}, {BC(164,168,171,255),18},
    }},
    {"PERIDOT", {
        {BC(0,0,0,0),2973}, {BC(86,86,89,71),364}, {BC(104,104,106,168),204},
        {BC(55,56,40,255),268}, {BC(131,131,130,255),98}, {BC(92,79,40,255),67},
        {BC(180,178,77,255),57}, {BC(191,186,175,255),31}, {BC(206,223,126,255),31},
        {BC(241,241,240,255),3},
    }},
    {"PETRIFIED_WOOD", {
        {BC(0,0,0,0),486}, {BC(101,65,56,71),242}, {BC(117,75,58,168),152},
        {BC(153,103,54,255),113}, {BC(228,183,72,255),29}, {BC(247,236,147,255),2},
    }},
    {"PHYLLITE", {
        {BC(0,0,0,0),67614}, {BC(86,86,89,71),18649}, {BC(104,104,106,168),13243},
        {BC(131,131,130,255),7804}, {BC(191,186,175,255),2115}, {BC(241,241,240,255),143},
    }},
    {"PINK TOURMALINE", {
        {BC(0,0,0,0),3902}, {BC(81,73,85,71),874}, {BC(100,90,103,168),527},
        {BC(74,53,65,255),985}, {BC(139,127,140,255),285}, {BC(132,86,96,255),236},
        {BC(209,148,153,255),183}, {BC(189,211,214,255),86}, {BC(246,224,220,255),85},
        {BC(238,244,240,255),5},
    }},
    {"PYROLUSITE", {
        {BC(0,0,0,0),12312}, {BC(48,46,50,71),3668}, {BC(52,51,53,168),2479},
        {BC(62,63,62,255),1575}, {BC(125,122,119,255),419}, {BC(167,169,170,255),27},
    }},
    {"QUARTZITE", {
        {BC(0,0,0,0),251206}, {BC(119,94,118,71),116040}, {BC(140,109,138,168),81416},
        {BC(174,132,172,255),47489}, {BC(199,173,198,255),12965}, {BC(233,218,229,255),836},
    }},
    {"RED PYROPE", {
        {BC(0,0,0,0),2770}, {BC(80,59,55,71),428}, {BC(88,66,61,168),254},
        {BC(75,35,40,255),302}, {BC(108,72,65,255),122}, {BC(102,54,44,255),76},
        {BC(206,76,61,255),66}, {BC(162,95,80,255),42}, {BC(229,160,156,255),32},
        {BC(215,154,140,255),4},
    }},
    {"RED TOURMALINE", {
        {BC(0,0,0,0),26635}, {BC(119,94,118,71),4112}, {BC(143,145,154,71),725},
        {BC(140,109,138,168),2403}, {BC(158,161,169,168),444}, {BC(54,46,52,255),4448},
        {BC(174,132,172,255),1192}, {BC(86,56,60,255),1065}, {BC(173,105,71,255),867},
        {BC(224,163,107,255),425}, {BC(199,173,198,255),380}, {BC(179,182,191,255),213},
        {BC(227,227,228,255),69}, {BC(233,218,229,255),24}, {BC(255,255,255,255),6},
    }},
    {"RED ZIRCON", {
        {BC(0,0,0,0),2544}, {BC(79,90,103,71),444}, {BC(105,111,135,168),256},
        {BC(75,35,40,255),436}, {BC(157,153,148,255),120}, {BC(102,54,44,255),110},
        {BC(206,76,61,255),92}, {BC(229,160,156,255),51}, {BC(222,227,225,255),41},
        {BC(255,255,255,255),2},
    }},
    {"RUBY", {
        {BC(0,0,0,0),4491}, {BC(119,94,118,71),324}, {BC(81,73,85,71),195},
        {BC(140,109,138,168),176}, {BC(100,90,103,168),97}, {BC(77,37,45,255),455},
        {BC(116,63,60,255),115}, {BC(205,72,53,255),86}, {BC(174,132,172,255),63},
        {BC(139,127,140,255),57}, {BC(231,154,145,255),44}, {BC(199,173,198,255),24},
        {BC(189,211,214,255),16}, {BC(238,244,240,255),1},
    }},
    {"RUBY_STAR", {
        {BC(0,0,0,0),2967}, {BC(119,94,118,71),283}, {BC(81,73,85,71),51},
        {BC(140,109,138,168),154}, {BC(100,90,103,168),24}, {BC(75,35,40,255),341},
        {BC(102,54,44,255),86}, {BC(206,76,61,255),66}, {BC(174,132,172,255),54},
        {BC(229,160,156,255),39}, {BC(199,173,198,255),23}, {BC(139,127,140,255),6},
        {BC(189,211,214,255),2},
    }},
    {"RUTILE", {
        {BC(0,0,0,0),8740}, {BC(101,72,61,71),2504}, {BC(124,107,78,168),1695},
        {BC(149,133,82,255),1093}, {BC(208,202,127,255),285}, {BC(225,233,177,255),19},
    }},
    {"SAPPHIRE", {
        {BC(0,0,0,0),2884}, {BC(119,94,118,71),302}, {BC(81,73,85,71),105},
        {BC(140,109,138,168),158}, {BC(100,90,103,168),49}, {BC(36,31,86,255),299},
        {BC(120,165,205,255),69}, {BC(39,84,150,255),69}, {BC(174,132,172,255),60},
        {BC(187,208,219,255),34}, {BC(139,127,140,255),31}, {BC(199,173,198,255),25},
        {BC(189,211,214,255),9}, {BC(238,244,240,255),1}, {BC(233,218,229,255),1},
    }},
    {"SAPPHIRE_STAR", {
        {BC(0,0,0,0),3324}, {BC(81,73,85,71),224}, {BC(119,94,118,71),222},
        {BC(143,145,154,71),51}, {BC(100,90,103,168),121}, {BC(140,109,138,168),108},
        {BC(158,161,169,168),24}, {BC(66,63,94,255),556}, {BC(165,170,199,255),146},
        {BC(104,108,144,255),131}, {BC(195,201,231,255),61}, {BC(139,127,140,255),54},
        {BC(174,132,172,255),49}, {BC(189,211,214,255),20}, {BC(199,173,198,255),19},
        {BC(179,182,191,255),6}, {BC(227,227,228,255),2}, {BC(238,244,240,255),1},
        {BC(233,218,229,255),1},
    }},
    {"SARD", {
        {BC(0,0,0,0),5774}, {BC(64,56,50,71),710}, {BC(74,63,55,168),503},
        {BC(59,48,57,255),326}, {BC(99,85,73,255),200}, {BC(76,57,66,255),175},
        {BC(90,60,70,255),154}, {BC(136,76,76,255),127}, {BC(156,95,77,255),73},
        {BC(134,114,97,255),72}, {BC(110,64,67,255),45}, {BC(196,148,118,255),29},
        {BC(199,184,158,255),4},
    }},
    {"SCHORL", {
        {BC(0,0,0,0),7985}, {BC(108,78,65,71),1442}, {BC(134,110,90,168),905},
        {BC(45,37,40,255),917}, {BC(161,141,112,255),462}, {BC(61,52,44,255),213},
        {BC(122,94,75,255),147}, {BC(188,170,139,255),84}, {BC(204,192,155,255),83},
        {BC(198,184,165,255),41}, {BC(229,221,196,255),5}, {BC(230,220,207,255),4},
    }},
    {"SHALE", {
        {BC(0,0,0,0),196352}, {BC(64,56,50,71),34302}, {BC(74,63,55,168),24238},
        {BC(114,97,83,255),35288}, {BC(56,47,40,255),32719}, {BC(99,85,73,255),24662},
        {BC(74,63,55,255),16232}, {BC(134,114,97,255),7663}, {BC(199,184,158,255),256},
    }},
    {"SILTSTONE", {
        {BC(0,0,0,0),93405}, {BC(108,78,65,71),29958}, {BC(134,110,90,168),21309},
        {BC(161,141,112,255),12497}, {BC(204,192,155,255),3371}, {BC(229,221,196,255),228},
    }},
    {"SLATE", {
        {BC(0,0,0,0),70217}, {BC(80,59,55,71),19321}, {BC(88,66,61,168),13832},
        {BC(108,72,65,255),7972}, {BC(162,95,80,255),2175}, {BC(215,154,140,255),147},
    }},
    {"SMOKY QUARTZ", {
        {BC(0,0,0,0),3927}, {BC(86,86,89,71),436}, {BC(104,104,106,168),233},
        {BC(44,47,42,255),242}, {BC(131,131,130,255),134}, {BC(85,85,66,255),57},
        {BC(170,163,113,255),33}, {BC(191,186,175,255),30}, {BC(212,209,169,255),25},
        {BC(241,241,240,255),3},
    }},
    {"SPHALERITE", {
        {BC(0,0,0,0),41497}, {BC(159,175,175,64),6542}, {BC(161,172,175,166),4818},
        {BC(193,136,164,255),3287}, {BC(106,72,91,255),2628}, {BC(163,176,178,255),2331},
        {BC(223,194,206,255),2292}, {BC(171,119,141,255),2182}, {BC(93,61,84,255),781},
        {BC(227,225,212,255),583}, {BC(136,97,114,255),573}, {BC(250,250,250,255),70},
    }},
    {"SPINEL_RED", {
        {BC(0,0,0,0),921}, {BC(143,145,154,71),313}, {BC(158,161,169,168),177},
        {BC(75,35,40,255),330}, {BC(179,182,191,255),91}, {BC(102,54,44,255),82},
        {BC(206,76,61,255),68}, {BC(229,160,156,255),32}, {BC(227,227,228,255),32},
        {BC(255,255,255,255),2},
    }},
    {"TETRAHEDRITE", {
        {BC(0,0,0,0),31362}, {BC(159,175,175,64),4670}, {BC(161,172,175,166),3430},
        {BC(222,227,225,255),2377}, {BC(105,111,135,255),1976}, {BC(255,255,255,255),1674},
        {BC(184,188,195,255),1631}, {BC(163,176,178,255),1616}, {BC(79,90,103,255),570},
        {BC(227,225,212,255),411}, {BC(157,153,148,255),405}, {BC(250,250,250,255),54},
    }},
    {"WHITE CHALCEDONY", {
        {BC(0,0,0,0),7743}, {BC(65,48,49,71),964}, {BC(108,78,65,71),305},
        {BC(70,51,51,168),647}, {BC(134,110,90,168),226}, {BC(89,88,100,255),595},
        {BC(91,63,55,255),273}, {BC(158,161,169,255),127}, {BC(227,227,228,255),117},
        {BC(145,107,70,255),89}, {BC(161,141,112,255),85}, {BC(255,255,255,255),55},
        {BC(198,184,165,255),32}, {BC(202,172,119,255),5}, {BC(230,220,207,255),1},
    }},
    {"WHITE JADE", {
        {BC(0,0,0,0),7302}, {BC(65,48,49,71),1411}, {BC(70,51,51,168),985},
        {BC(89,88,100,255),686}, {BC(91,63,55,255),391}, {BC(158,161,169,255),146},
        {BC(227,227,228,255),137}, {BC(145,107,70,255),136}, {BC(255,255,255,255),63},
        {BC(202,172,119,255),7},
    }},
    {"YELLOW JASPER", {
        {BC(0,0,0,0),3756}, {BC(108,78,65,71),815}, {BC(134,110,90,168),585},
        {BC(85,67,40,255),424}, {BC(161,141,112,255),247}, {BC(229,204,87,255),97},
        {BC(138,125,63,255),95}, {BC(198,184,165,255),85}, {BC(251,240,120,255),38},
        {BC(230,220,207,255),2},
    }},
    {"YELLOW ZIRCON", {
        {BC(0,0,0,0),698}, {BC(80,59,55,71),108}, {BC(88,66,61,168),64},
        {BC(85,67,40,255),79}, {BC(138,125,63,255),19}, {BC(108,72,65,255),18},
        {BC(229,204,87,255),17}, {BC(251,240,120,255),11}, {BC(162,95,80,255),10},
    }},
};
#undef BC

static std::unordered_map<int /* inorganic_mat */, palette_t> palette_by_mat;
// Track every (mat, overlay_texpos) pair we've already merged into the cache,
// so each unique overlay variant is read exactly once even though the render
// hook may walk past the same wall many times. wall_stone.png ships ~40
// directional variants per material, and different variants can contribute
// different colors to the same material's palette, so we accumulate the
// union across variants rather than stopping after the first one we see.
static std::unordered_set<int64_t /* (mat<<32)|texpos */> observed_overlays;

// Forward declarations for helpers used by the render hook below; the
// definitions live further down with the other texture/cache utilities.
static bool read_texture_histogram(int32_t texpos,
                                   std::vector<std::pair<uint32_t, int>> &out,
                                   int &w, int &h);
static void sort_palette(palette_t &p);
static std::string format_palette_cpp(const std::string &id,
                                      const palette_t &p);

// ---------------------------------------------------------------------------
// Composite sprite generation for rough-edge leaks. See the "Rough-edge
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

    // 2. Alpha-blend each enabled side's overlay (re-tinted by the rough
    // neighbour's mat) on top.
    int16_t side_mats[4] = { key.mat_s, key.mat_w, key.mat_e, key.mat_n };
    for (size_t i = 0; i < ROUGH_SIDES.size(); i++) {
        int byte_value = (int)((key.floor_flag >>
                                (ROUGH_SIDES[i].byte_offset * 8)) & 0xff);
        if (!(byte_value & 0x08)) continue;
        int lookup_key =
            (ROUGH_SIDES[i].wall_graphics_side << 8) | byte_value;
        auto it = overlay_lookup.find(lookup_key);
        if (it == overlay_lookup.end()) continue;
        const auto &snap = overlay_snapshots[it->second];
        if (snap.pixels.empty()) continue;
        if (snap.w != w || snap.h != h) continue;
        int16_t mat = side_mats[i];
        if (mat < 0) continue;
        for (size_t p = 0; p < snap.pixels.size(); p++) {
            uint32_t ov = retint_overlay_pixel(snap.pixels[p], mat);
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
    }

    return Textures::createTile(pixels, w, h, true);
}

static TexposHandle get_composite(int32_t base_texpos, uint64_t floor_flag,
                                  int wx, int wy, int wz, int base_mat) {
    composite_key key{};
    key.base_texpos = base_texpos;
    key.floor_flag = floor_flag;
    key.base_mat = (int16_t)base_mat;
    int16_t side_mats[4] = { -1, -1, -1, -1 };
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
        if (m >= 0 && (size_t)m < material_tints.size())
            side_mats[i] = (int16_t)m;
    }
    key.mat_s = side_mats[0];
    key.mat_w = side_mats[1];
    key.mat_e = side_mats[2];
    key.mat_n = side_mats[3];

    auto it = composite_cache.find(key);
    if (it != composite_cache.end()) return it->second;
    TexposHandle h = make_composite(key);
    composite_cache[key] = h;
    return h;
}

// ---------------------------------------------------------------------------
// Render hook

struct cavern_colors_hook : df::viewscreen_dwarfmodest {
    typedef df::viewscreen_dwarfmodest interpose_base;

    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t curtick)) {
        INTERPOSE_NEXT(render)(curtick);

        if (!Screen::inGraphicsMode()) return;
        if (!gps || !gps->main_viewport) return;
        if (material_tints.empty()) return;

        if (leaks_enabled) maybe_resnapshot_overlays();

        auto *vp = gps->main_viewport;
        auto dims = Gui::getDwarfmodeViewDims().map();

        for (int y = dims.first.y; y <= dims.second.y; y++) {
            for (int x = dims.first.x; x <= dims.second.x; x++) {
                size_t idx = (size_t)(x * vp->dim_y + y);
                int32_t src_texpos = vp->screentexpos_background[idx];
                if (src_texpos <= 0) continue;

                int wx = *window_x + x;
                int wy = *window_y + y;
                int wz = *window_z;
                df::coord pos(wx, wy, wz);

                if (!Maps::isTileVisible(pos)) continue;

                df::tiletype *tt = Maps::getTileType(pos);
                if (!tt) continue;
                // Tint dug/carved stone surfaces: open floors plus mined ramps
                // and stairs. Walls and Open shapes don't draw a floor sprite,
                // but we want to record wall texposes for cache investigation.
                auto shape = tileShapeBasic(tileShape(*tt));
                bool is_floor_like =
                    shape == df::tiletype_shape_basic::Floor ||
                    shape == df::tiletype_shape_basic::Ramp ||
                    shape == df::tiletype_shape_basic::Stair;
                bool is_wall = shape == df::tiletype_shape_basic::Wall;
                // Walls always go through the observation path so we keep
                // palette_by_mat in sync with what DF is actually drawing.
                // collect_walls only gates the verbose texpos-tracking maps.
                if (!is_floor_like && !is_wall)
                    continue;
                // Stone-like materials get base-sprite tinting. Other floor
                // surfaces (constructions, fungus, moss, grass) keep their
                // own appearance but still need leak processing — DF
                // composites rough-edge overlays from neighbouring rough
                // cavern tiles onto *any* adjacent floor, regardless of
                // what that floor is made of.
                auto tile_mat_enum = tileMaterial(*tt);
                bool is_stone_like =
                    tile_mat_enum == df::tiletype_material::STONE ||
                    tile_mat_enum == df::tiletype_material::MINERAL ||
                    tile_mat_enum == df::tiletype_material::LAVA_STONE;
                if (is_wall && !is_stone_like)
                    continue;

                df::map_block *block = Maps::getTileBlock(wx, wy, wz);
                if (!block) continue;

                int mat = -1;
                if (is_stone_like) {
                    mat = get_tile_mat(block, wx & 15, wy & 15, *tt);
                    if (mat < 0 ||
                        (size_t)mat >= material_tints.size())
                        mat = -1;
                }

                if (is_wall) {
                    if (mat < 0) continue;
                    int32_t overlay =
                        vp->screentexpos_background_two[idx];

                    // First time we see this (mat, overlay) pair: read the
                    // overlay's pixels and merge its colors into the cached
                    // palette for the material. Different wall-shape variants
                    // can contribute different colors, so we union across
                    // variants. If the resulting color set is wider than
                    // baked (extra colors observed), print a one-liner so a
                    // maintainer knows to refresh BAKED.
                    int64_t obs_key =
                        ((int64_t)mat << 32) | (uint32_t)overlay;
                    if (overlay > 0 && !observed_overlays.count(obs_key)) {
                        palette_t obs;
                        int w, h;
                        if (read_texture_histogram(overlay, obs, w, h)) {
                            observed_overlays.insert(obs_key);
                            // Merge obs into palette_by_mat[mat] by color:
                            // matching rgba accumulates count, new rgba is
                            // appended. After merge we re-sort canonically.
                            auto &cur = palette_by_mat[mat];
                            size_t before = cur.size();
                            for (auto &kv : obs) {
                                auto it = std::find_if(
                                    cur.begin(), cur.end(),
                                    [&](const std::pair<uint32_t,int> &p) {
                                        return p.first == kv.first;
                                    });
                                if (it != cur.end()) it->second += kv.second;
                                else cur.push_back(kv);
                            }
                            sort_palette(cur);
                            // Diff fires only when this variant added colors
                            // the cache (= baked seed + earlier observations
                            // this session) didn't yet have. Stays quiet for
                            // variants whose colors were already represented.
                            if (cur.size() > before) {
                                auto &id =
                                    world->raws.inorganics.all[mat]->id;
                                color_ostream_proxy c(
                                    Core::getInstance().getConsole());
                                c.print("[cavern-colors] {}: variant added "
                                        "{} new color(s) (run dump-baked "
                                        "to export)\n",
                                        id, cur.size() - before);
                            }
                        }
                    }

                    if (collect_walls) {
                        wall_texposes_by_mat[mat].insert(src_texpos);
                        if (overlay > 0)
                            wall_overlay_texposes_by_mat[mat].insert(overlay);
                    }
                    continue;
                }

                // Floor-like (FLOOR/RAMP/STAIR). Two paths:
                //  - composite: when the cell has rough-edge bits set in
                //    floor_flag and leak processing is enabled. The composite
                //    handles both base-mat tint (if any) and the per-side
                //    overlay re-tinted by the rough neighbour's material.
                //  - plain tint: cell has no leak bits, just tint by base
                //    material. Skipped when base is non-stone (e.g.
                //    constructions) since we have no material to tint by.
                uint64_t flag = 0;
                if (leaks_enabled &&
                    shape == df::tiletype_shape_basic::Floor &&
                    vp->screentexpos_floor_flag) {
                    // Only take the composite path if we have at least one
                    // overlay snapshot to use; otherwise the composite would
                    // strip DF's own leak draw and replace it with nothing.
                    bool have_any = false;
                    for (auto &s : overlay_snapshots)
                        if (!s.pixels.empty()) { have_any = true; break; }
                    if (have_any)
                        flag = vp->screentexpos_floor_flag[idx];
                }

                if (flag != 0) {
                    TexposHandle h = get_composite(src_texpos, flag,
                                                   wx, wy, wz, mat);
                    if (!h) continue;
                    long texpos = Textures::getTexposByHandle(h);
                    if (texpos > 0) {
                        vp->screentexpos_background[idx] = (int32_t)texpos;
                        // Suppress DF's own overlay redraw — the composite
                        // already bakes in the tinted version.
                        vp->screentexpos_floor_flag[idx] = 0;
                    }
                } else if (mat >= 0) {
                    TexposHandle h = get_tinted(src_texpos, mat);
                    if (!h) continue;
                    long texpos = Textures::getTexposByHandle(h);
                    if (texpos > 0)
                        vp->screentexpos_background[idx] = (int32_t)texpos;
                }
            }
        }
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(cavern_colors_hook, render);

// ---------------------------------------------------------------------------
// Cache dump: walk DF's renderer tile_cache and print every entry whose texpos
// is in wall_texposes_by_mat (collected by the render hook above). Each entry
// in the cache corresponds to a unique (sprite, fg-rgb, bg-rgb, flag) combo
// DF has rendered. For natural stone walls this should yield one combo per
// material visible since collection started, revealing the actual color pair
// DF passes to its renderer for each material.
static command_result dump_cache(color_ostream &out) {
    if (!enabler) {
        out.printerr("enabler not available\n");
        return CR_FAILURE;
    }
    auto *r2d = virtual_cast<df::renderer_2d_base>(enabler->renderer);
    if (!r2d) {
        out.printerr("renderer is not a renderer_2d_base subclass; "
                     "tile_cache unreachable\n");
        return CR_FAILURE;
    }
    if (wall_texposes_by_mat.empty()) {
        out.print("No wall texposes collected yet. Run "
                  "'cavern-colors collect-walls on' first, scroll past some "
                  "stone walls, then dump-cache.\n");
        return CR_OK;
    }

    // Invert the collected map: texpos -> {mat ids it was seen for}. A given
    // sprite may legitimately appear for multiple materials (the directional
    // wall variants are shared across stones), so attribution is many-to-many.
    std::unordered_map<int32_t, std::set<int>> mats_by_texpos;
    for (auto &kv : wall_texposes_by_mat)
        for (int32_t t : kv.second)
            mats_by_texpos[t].insert(kv.first);

    out.print("Collected {} wall texposes across {} materials\n",
              mats_by_texpos.size(), wall_texposes_by_mat.size());
    out.print("Per material:\n");
    for (auto &kv : wall_texposes_by_mat) {
        const auto &id = (size_t)kv.first < world->raws.inorganics.all.size()
                             ? world->raws.inorganics.all[kv.first]->id
                             : std::string("?");
        out.print("  {:>20}: {} texpos(es)\n", id, kv.second.size());
    }

    size_t n_matched = 0;
    out.print("\nMatching tile_cache entries:\n");
    out.print("  {:>6} {:>5} {:>5} {:>5} {:>5} {:>5} {:>5} {:>4}  {}\n",
              "texpos", "r", "g", "b", "br", "bg", "bb", "flag", "mats");
    for (auto &entry : r2d->tile_cache.tile_cache) {
        const df::texture_fullid &k = entry.first;
        auto it = mats_by_texpos.find(k.texpos);
        if (it == mats_by_texpos.end()) continue;
        n_matched++;
        std::string mats;
        for (int m : it->second) {
            if (!mats.empty()) mats += ",";
            mats += (size_t)m < world->raws.inorganics.all.size()
                        ? world->raws.inorganics.all[m]->id
                        : std::string("?");
        }
        out.print("  {:>6} {:5.3f} {:5.3f} {:5.3f} {:5.3f} {:5.3f} {:5.3f} "
                  "{:>4x}  {}\n",
                  k.texpos, k.r, k.g, k.b, k.br, k.bg, k.bb,
                  k.flag.whole, mats);
    }
    out.print("\n{} cache entries matched\n", n_matched);
    return CR_OK;
}

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
static command_result sample_cell(color_ostream &out,
                                  std::vector<std::string> &params) {
    if (!gps || !world || !window_x || !window_y || !window_z) {
        out.printerr("globals not available\n");
        return CR_FAILURE;
    }

    df::coord world_pos;
    if (params.size() >= 3) {
        // explicit world coords: sample-cell <wx> <wy>   (z = current)
        world_pos.x = std::atoi(params[1].c_str());
        world_pos.y = std::atoi(params[2].c_str());
        world_pos.z = params.size() >= 4 ? std::atoi(params[3].c_str())
                                         : *window_z;
    } else {
        world_pos = Gui::getMousePos(true);
        if (!world_pos.isValid()) {
            out.printerr("Mouse is not over a valid map position. Move the "
                         "cursor over the cell you want, or pass world "
                         "coords: cavern-colors sample-cell <wx> <wy> "
                         "[<wz>]\n");
            return CR_WRONG_USAGE;
        }
    }
    out.print("Sampling world=({},{},{})\n",
              world_pos.x, world_pos.y, world_pos.z);

    // World -> viewport-relative.
    auto *vp = gps->main_viewport;
    if (!vp) {
        out.printerr("main_viewport not available\n");
        return CR_FAILURE;
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
        return CR_OK;
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
        return CR_OK;
    }
    out.print("  tile shape:    {}\n",
              ENUM_KEY_STR(tiletype_shape, tileShape(*tt)));
    out.print("  tile material: {}\n",
              ENUM_KEY_STR(tiletype_material, tileMaterial(*tt)));
    df::map_block *block = Maps::getTileBlock(world_pos.x, world_pos.y,
                                              world_pos.z);
    if (!block) return CR_OK;
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
    return CR_OK;
}

static void seed_palettes_from_baked() {
    palette_by_mat.clear();
    observed_overlays.clear();
    if (!world) return;
    auto &inorganics = world->raws.inorganics.all;
    for (size_t i = 0; i < inorganics.size(); i++) {
        if (!inorganics[i]) continue;
        auto it = BAKED.find(inorganics[i]->id);
        if (it != BAKED.end())
            palette_by_mat[(int)i] = it->second;
    }
    DEBUG(log).print("seeded {} palettes from BAKED ({} mats total)\n",
                     palette_by_mat.size(), inorganics.size());
}

// Emit a palette as a C++ initializer block formatted to match the BAKED
// table verbatim (4-space indent, 3 stops per line, trailing commas), so the
// output of `observations on` can be pasted straight into BAKED without
// editing.
static std::string format_palette_cpp(const std::string &id,
                                      const palette_t &p) {
    std::string s = fmt::format("    {{\"{}\", {{\n        ", id);
    constexpr int per_line = 3;
    for (size_t i = 0; i < p.size(); i++) {
        if (i > 0 && i % per_line == 0)
            s += "\n        ";
        else if (i > 0)
            s += " ";
        uint32_t c = p[i].first;
        s += fmt::format("{{BC({},{},{},{}),{}}},",
                         (int)(c & 0xff), (int)((c >> 8) & 0xff),
                         (int)((c >> 16) & 0xff), (int)((c >> 24) & 0xff),
                         p[i].second);
    }
    s += "\n    }},";
    return s;
}

// Sort a histogram into the same alpha-asc / count-desc order as BAKED uses.
static void sort_palette(palette_t &p) {
    std::sort(p.begin(), p.end(),
              [](const std::pair<uint32_t, int> &a,
                 const std::pair<uint32_t, int> &b) {
                  int aa = (a.first >> 24) & 0xff;
                  int ab = (b.first >> 24) & 0xff;
                  if (aa != ab) return aa < ab;
                  return a.second > b.second;
              });
}

// Read the surface at texpos, convert to RGBA, return a list of
// (color, count) pairs sorted by count descending. Returns true on success.
static bool read_texture_histogram(
    int32_t texpos,
    std::vector<std::pair<uint32_t, int>> &out_sorted,
    int &out_w, int &out_h)
{
    if (!enabler || texpos <= 0 ||
        (size_t)texpos >= enabler->textures.raws.size())
        return false;
    SDL_Surface *src = (SDL_Surface *)enabler->textures.raws[texpos];
    if (!src) return false;
    SDL_PixelFormat *fmt = DFSDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!fmt) return false;
    SDL_Surface *conv = DFSDL_ConvertSurface(src, fmt, 0);
    if (!conv) return false;
    out_w = conv->w;
    out_h = conv->h;
    std::unordered_map<uint32_t, int> hist;
    for (int y = 0; y < out_h; y++) {
        uint8_t *row = (uint8_t *)conv->pixels + y * conv->pitch;
        for (int x = 0; x < out_w; x++) {
            uint8_t *p = row + x * 4;
            hist[rgba(p[0], p[1], p[2], p[3])]++;
        }
    }
    DFSDL_FreeSurface(conv);
    out_sorted.assign(hist.begin(), hist.end());
    std::sort(out_sorted.begin(), out_sorted.end(),
              [](const std::pair<uint32_t, int> &a,
                 const std::pair<uint32_t, int> &b) {
                  return a.second > b.second;
              });
    return true;
}

// ---------------------------------------------------------------------------
// Dump a histogram of pixel colors at the given texpos. Useful for inspecting
// dynamically-generated textures referenced from vp->screentexpos_background_two
// (and friends) without saving to disk: a small distinct-color count plus a
// recognizable palette tells us at a glance whether the texture is a per-
// material tinted overlay vs. some other kind of sprite.
static command_result dump_texture(color_ostream &out,
                                   std::vector<std::string> &params) {
    if (params.size() < 2) {
        out.printerr("Usage: cavern-colors dump-texture <texpos>\n");
        return CR_WRONG_USAGE;
    }
    if (!enabler) {
        out.printerr("enabler not available\n");
        return CR_FAILURE;
    }
    int32_t texpos = std::atoi(params[1].c_str());
    std::vector<std::pair<uint32_t, int>> sorted;
    int w, h;
    if (!read_texture_histogram(texpos, sorted, w, h)) {
        out.printerr("Could not read texpos {} (out of range or no surface)\n",
                     texpos);
        return CR_FAILURE;
    }
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
    return CR_OK;
}

// ---------------------------------------------------------------------------
// Debug: print the alpha mask of a sprite as a 32×32 ASCII grid. Used to
// see where opaque/translucent pixels are positioned in the sprite — vital
// for matching DF's overlay orientation when histograms can't tell us
// position. Glyph levels: ' '=fully transparent, '.'=alpha<64,
// ':'=alpha<128, 'o'=alpha<192, '#'=alpha 192+.
static command_result mask_texture(color_ostream &out,
                                   std::vector<std::string> &params) {
    if (params.size() < 2) {
        out.printerr("Usage: cavern-colors mask-texture <texpos>\n");
        return CR_WRONG_USAGE;
    }
    if (!enabler) {
        out.printerr("enabler not available\n");
        return CR_FAILURE;
    }
    int32_t texpos = std::atoi(params[1].c_str());
    if (texpos <= 0 ||
        (size_t)texpos >= enabler->textures.raws.size()) {
        out.printerr("texpos {} out of range\n", texpos);
        return CR_FAILURE;
    }
    SDL_Surface *src = (SDL_Surface *)enabler->textures.raws[texpos];
    if (!src) {
        out.printerr("no surface at texpos {}\n", texpos);
        return CR_FAILURE;
    }
    SDL_PixelFormat *fmt = DFSDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!fmt) {
        out.printerr("could not alloc RGBA32 format\n");
        return CR_FAILURE;
    }
    SDL_Surface *conv = DFSDL_ConvertSurface(src, fmt, 0);
    if (!conv) {
        out.printerr("could not convert surface\n");
        return CR_FAILURE;
    }
    int w = conv->w, h = conv->h;
    out.print("texpos {} alpha mask ({}x{}):\n", texpos, w, h);
    for (int y = 0; y < h; y++) {
        std::string line = "  ";
        uint8_t *row = (uint8_t *)conv->pixels + y * conv->pitch;
        for (int x = 0; x < w; x++) {
            uint8_t a = row[x * 4 + 3];
            char c = ' ';
            if (a == 0)         c = ' ';
            else if (a < 64)    c = '.';
            else if (a < 128)   c = ':';
            else if (a < 192)   c = 'o';
            else                c = '#';
            line += c;
        }
        out.print("{}\n", line);
    }
    DFSDL_FreeSurface(conv);
    return CR_OK;
}

// ---------------------------------------------------------------------------
// Debug: rewrite every opaque pixel of the SDL_Surface backing the given
// texpos to solid red, in place. Used to test whether DF's renderer actually
// re-reads pixel data from enabler->textures.raws on each frame (so an
// in-place edit of a boulder-overlay sprite would propagate to the rendered
// rough-edge bleed), or whether it has uploaded the bitmap once to GPU at
// world-load and ignores subsequent buffer changes. If the leak turns red
// in-game after this command, in-place tinting is viable for the real fix.
static command_result paint_overlay(color_ostream &out,
                                    std::vector<std::string> &params) {
    if (params.size() < 2) {
        out.printerr("Usage: cavern-colors paint-overlay <texpos>\n");
        return CR_WRONG_USAGE;
    }
    if (!enabler) {
        out.printerr("enabler not available\n");
        return CR_FAILURE;
    }
    int32_t texpos = std::atoi(params[1].c_str());
    if (texpos <= 0 || (size_t)texpos >= enabler->textures.raws.size()) {
        out.printerr("texpos {} out of range\n", texpos);
        return CR_FAILURE;
    }
    SDL_Surface *src = (SDL_Surface *)enabler->textures.raws[texpos];
    if (!src) {
        out.printerr("no surface at texpos {}\n", texpos);
        return CR_FAILURE;
    }

    int w = src->w, h = src->h;
    int bpp = src->format->BytesPerPixel;
    int pitch = src->pitch;
    uint32_t Amask = src->format->Amask;
    uint32_t Rmask = src->format->Rmask;
    uint32_t Gmask = src->format->Gmask;
    uint32_t Bmask = src->format->Bmask;

    int touched = 0;
    for (int y = 0; y < h; y++) {
        uint8_t *row = (uint8_t *)src->pixels + y * pitch;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + x * bpp;
            uint32_t pix = 0;
            for (int b = 0; b < bpp; b++) pix |= (uint32_t)p[b] << (b * 8);
            if (Amask && (pix & Amask) == 0) continue; // skip transparent
            // Keep original alpha, clear RGB, set R to all-ones in its mask.
            uint32_t newpix = (pix & Amask) | Rmask;
            (void)Gmask; (void)Bmask;
            for (int b = 0; b < bpp; b++) p[b] = (newpix >> (b * 8)) & 0xff;
            touched++;
        }
    }
    out.print("texpos {}: painted {} opaque pixels red "
              "(format bpp={}, Rmask=0x{:x}, Amask=0x{:x})\n",
              texpos, touched, bpp, Rmask, Amask);
    return CR_OK;
}

// ---------------------------------------------------------------------------
// For each material with a collected wall-overlay texpos, dump the overlay's
// color palette. Each overlay encodes DF's authoritative per-material color
// ramp: a few alpha-sorted RGB stops representing shadow / midtone /
// highlight tints baked from mat_rgb at world-load. Output is one line per
// (mat, alpha-stop) tuple, sorted by alpha asc then by count desc — the
// alpha=255 entries are the visible stops; alpha<255 entries describe the
// edge-fading rims.
static command_result extract_palette(color_ostream &out) {
    if (wall_overlay_texposes_by_mat.empty()) {
        out.print("No overlay texposes collected yet. Run "
                  "'cavern-colors collect-walls on', scroll past stone walls "
                  "of the materials you want sampled, then run "
                  "extract-palette.\n");
        return CR_OK;
    }
    // Sort mats by id for stable output.
    std::vector<int> mats;
    mats.reserve(wall_overlay_texposes_by_mat.size());
    for (auto &kv : wall_overlay_texposes_by_mat) mats.push_back(kv.first);
    std::sort(mats.begin(), mats.end(),
              [](int a, int b) {
                  const auto &id_a = world->raws.inorganics.all[a]->id;
                  const auto &id_b = world->raws.inorganics.all[b]->id;
                  return id_a < id_b;
              });

    out.print("{:<24} {:>7} {:>7} {:>7}  {:>5} : {}\n",
              "material", "mat_R", "mat_G", "mat_B",
              "count", "(R,G,B,alpha)");
    for (int mat : mats) {
        const auto &id = world->raws.inorganics.all[mat]->id;
        auto &m = world->raws.inorganics.all[mat]->material;
        int mr = (int)(m.mat_rgb[0] * 255 + 0.5f);
        int mg = (int)(m.mat_rgb[1] * 255 + 0.5f);
        int mb = (int)(m.mat_rgb[2] * 255 + 0.5f);
        // If a material picked up multiple overlay texposes, sample the
        // smallest one (most likely to be a stable canonical overlay; the
        // others are typically directional variants with the same palette).
        const auto &set = wall_overlay_texposes_by_mat[mat];
        int32_t texpos = *std::min_element(set.begin(), set.end());

        std::vector<std::pair<uint32_t, int>> sorted;
        int w, h;
        if (!read_texture_histogram(texpos, sorted, w, h)) {
            out.print("{:<24} {:>7} {:>7} {:>7}  (could not read texpos {})\n",
                      id, mr, mg, mb, texpos);
            continue;
        }
        // Group by alpha, sort by alpha asc, within alpha by count desc.
        std::sort(sorted.begin(), sorted.end(),
                  [](const std::pair<uint32_t, int> &a,
                     const std::pair<uint32_t, int> &b) {
                      int aa = (a.first >> 24) & 0xff;
                      int ab = (b.first >> 24) & 0xff;
                      if (aa != ab) return aa < ab;
                      return a.second > b.second;
                  });
        out.print("{:<24} {:>7} {:>7} {:>7}  (overlay texpos {}, {}x{}, "
                  "{} colors)\n",
                  id, mr, mg, mb, texpos, w, h, sorted.size());
        for (auto &kv : sorted) {
            uint32_t c = kv.first;
            int r = c & 0xff, g = (c >> 8) & 0xff;
            int b = (c >> 16) & 0xff, a = (c >> 24) & 0xff;
            out.print("{:<24} {:>7} {:>7} {:>7}  {:>5} : ({:>3},{:>3},{:>3},"
                      "{:>3})\n",
                      "", "", "", "", kv.second, r, g, b, a);
        }
    }
    return CR_OK;
}

// ---------------------------------------------------------------------------
// Dump the pre-registered wall and boulder-floor graphics tables built at
// world-load. world->raws.descriptors holds vectors of {flags, texpos} pairs
// keyed by a flag word that encodes the material/variant. If we can decode
// that flag layout we get the full mat -> texpos map up-front, no scrolling
// required.
//
// boulder_floor_graphics_flag is already decoded: color_index (8 bits) +
// texture_index (8 bits). viewport_wall_flag is uint64_t with only a stub
// bit defined in df-structures; we'll need to infer the layout from
// observation - hence cross-referencing each entry against
// wall_overlay_texposes_by_mat so the user can correlate known mats with
// flag values.
static command_result dump_wall_graphics(color_ostream &out) {
    if (!world) {
        out.printerr("world not available\n");
        return CR_FAILURE;
    }
    auto &desc = world->raws.descriptors;

    // Invert overlay map: texpos -> mat-id-set, so we can label rows.
    std::unordered_map<int32_t, std::set<int>> mats_by_texpos;
    for (auto &kv : wall_overlay_texposes_by_mat)
        for (int32_t t : kv.second)
            mats_by_texpos[t].insert(kv.first);

    out.print("=== wall_graphics_info ({} entries) ===\n",
              desc.wall_graphics_info.size());
    out.print("  {:>5} {:>18} {:>7}  {}\n",
              "idx", "flags (hex)", "texpos", "observed mats");
    for (size_t i = 0; i < desc.wall_graphics_info.size(); i++) {
        auto *info = desc.wall_graphics_info[i];
        if (!info) continue;
        std::string mats;
        auto it = mats_by_texpos.find(info->texpos);
        if (it != mats_by_texpos.end()) {
            for (int m : it->second) {
                if (!mats.empty()) mats += ",";
                mats += (size_t)m < world->raws.inorganics.all.size()
                            ? world->raws.inorganics.all[m]->id
                            : std::string("?");
            }
        }
        out.print("  {:>5} {:>18x} {:>7}  {}\n",
                  i, info->flags.whole, info->texpos, mats);
    }

    out.print("\n=== boulder_floor_graphics_info ({} entries) ===\n",
              desc.boulder_floor_graphics_info.size());
    out.print("  {:>5} {:>10} {:>4} {:>4} {:>7}\n",
              "idx", "flags hex", "ci", "ti", "texpos");
    for (size_t i = 0; i < desc.boulder_floor_graphics_info.size(); i++) {
        auto *info = desc.boulder_floor_graphics_info[i];
        if (!info) continue;
        out.print("  {:>5} {:>10x} {:>4} {:>4} {:>7}\n",
                  i, info->flags.whole,
                  (int)info->flags.bits.color_index,
                  (int)info->flags.bits.texture_index,
                  info->texpos);
    }
    return CR_OK;
}

// ---------------------------------------------------------------------------
// Dump the live palette state as a pastable C++ initializer block — merged
// from BAKED (the compile-time seed) and palette_by_mat (which contains any
// observation-driven overrides from the current world). For ids present in
// both, observation wins. Entries are sorted by id for stable output.
//
// Workflow:
//   1. cavern-colors observations on
//   2. walk the map to populate the cache with current-world palettes
//   3. cavern-colors dump-baked
//   4. paste the block between `BAKED = {` and `};` in cavern-colors.cpp,
//      replacing the existing body. BC(...) is in scope inside that block.
static command_result dump_baked(color_ostream &out) {
    // Merge baked + observed. std::map for stable id-sorted iteration.
    std::map<std::string, palette_t> merged;
    for (auto &kv : BAKED) merged[kv.first] = kv.second;

    size_t observed_overrides = 0;
    size_t observed_new = 0;
    if (world) {
        for (auto &kv : palette_by_mat) {
            int mat = kv.first;
            if ((size_t)mat >= world->raws.inorganics.all.size()) continue;
            auto *ino = world->raws.inorganics.all[mat];
            if (!ino) continue;
            auto it = merged.find(ino->id);
            if (it == merged.end()) {
                observed_new++;
                merged[ino->id] = kv.second;
            } else if (it->second != kv.second) {
                observed_overrides++;
                it->second = kv.second;
            }
        }
    }

    out.print("// === Pastable BAKED contents "
              "({} entries: {} baked, {} observed-overrides, "
              "{} observed-new) ===\n",
              merged.size(), BAKED.size(),
              observed_overrides, observed_new);
    out.print("// Replace the existing body of BAKED = {{ ... }} with the "
              "block below.\n//\n");
    for (auto &kv : merged)
        out.print("{}\n", format_palette_cpp(kv.first, kv.second));
    return CR_OK;
}

// ---------------------------------------------------------------------------
// Plugin lifecycle

DFhackCExport command_result plugin_init(color_ostream &out,
                                         std::vector<PluginCommand> &commands)
{
    commands.push_back(PluginCommand(
        "cavern-colors",
        "Restore per-mineral floor colors in premium graphics mode.",
        [](color_ostream &out, std::vector<std::string> &params) -> command_result {
            if (params.empty()) {
                out.print("Usage: cavern-colors mode <hybrid|mat_rgb|basic_color|build_color|tile_color>\n");
                out.print("       cavern-colors boost <float>      (brightness multiplier, default 2.0)\n");
                out.print("       cavern-colors strength <0..1>    (tint saturation, default 1.0)\n");
                out.print("       cavern-colors enable|disable\n");
                out.print("       cavern-colors leaks on|off       (rough-edge bleed tinting; default on)\n");
                out.print("       cavern-colors collect-walls on|off\n");
                out.print("       cavern-colors dump-cache\n");
                out.print("       cavern-colors sample-cell [<wx> <wy> [<wz>]]   (default: mouse pos)\n");
                out.print("       cavern-colors dump-texture <texpos>\n");
                out.print("       cavern-colors mask-texture <texpos>\n");
                out.print("       cavern-colors paint-overlay <texpos>\n");
                out.print("       cavern-colors extract-palette\n");
                out.print("       cavern-colors dump-wall-graphics\n");
                out.print("       cavern-colors dump-baked\n");
                out.print("Current mode:     {}\n", mode_name(color_mode));
                out.print("Brightness boost: {}\n", brightness_boost);
                out.print("Tint strength:    {}\n", tint_strength);
                out.print("Enabled:          {}\n", is_enabled ? "yes" : "no");
                out.print("Leak tinting:     {} "
                          "({} composite(s) cached, "
                          "{} overlay snapshot(s), "
                          "{} (side, byte) lookup key(s), "
                          "last table size {})\n",
                          leaks_enabled ? "on" : "off",
                          composite_cache.size(),
                          overlay_snapshots.size(),
                          overlay_lookup.size(),
                          overlay_snapshot_table_size);
                out.print("Collecting walls: {} ({} mat(s) base / "
                         "{} mat(s) overlay)\n",
                         collect_walls ? "yes" : "no",
                         wall_texposes_by_mat.size(),
                         wall_overlay_texposes_by_mat.size());
                // Count distinct mats represented in observed_overlays.
                std::unordered_set<int> obs_mats;
                for (int64_t k : observed_overlays)
                    obs_mats.insert((int)(k >> 32));
                out.print("Palette cache:    {} mat(s)  "
                         "({} observed via {} overlay variant(s), baked "
                         "table has {})\n",
                         palette_by_mat.size(),
                         obs_mats.size(), observed_overlays.size(),
                         BAKED.size());
                return CR_OK;
            }

            if (params[0] == "enable" || params[0] == "disable") {
                bool want = (params[0] == "enable");
                return Core::getInstance().runCommand(out,
                    want ? "enable cavern-colors" : "disable cavern-colors");
            }

            if (params[0] == "mode" && params.size() >= 2) {
                ColorMode new_mode;
                if      (params[1] == "hybrid")      new_mode = ColorMode::hybrid;
                else if (params[1] == "mat_rgb")     new_mode = ColorMode::mat_rgb;
                else if (params[1] == "basic_color") new_mode = ColorMode::basic_color;
                else if (params[1] == "build_color") new_mode = ColorMode::build_color;
                else if (params[1] == "tile_color")  new_mode = ColorMode::tile_color;
                else {
                    out.printerr("Unknown mode '{}'. Use: hybrid, mat_rgb, basic_color, "
                                 "build_color, tile_color\n", params[1]);
                    return CR_WRONG_USAGE;
                }
                if (new_mode != color_mode) {
                    color_mode = new_mode;
                    clear_tinted_cache();
                    clear_composite_cache();
                    build_material_tints();
                    out.print("cavern-colors mode set to '{}'\n", params[1]);
                }
                return CR_OK;
            }

            if ((params[0] == "boost" || params[0] == "strength") && params.size() >= 2) {
                char *end = nullptr;
                float v = strtof(params[1].c_str(), &end);
                if (end == params[1].c_str() || !std::isfinite(v) || v < 0.0f) {
                    out.printerr("Expected a non-negative number, got '{}'\n", params[1]);
                    return CR_WRONG_USAGE;
                }
                if (params[0] == "boost") {
                    brightness_boost = v;
                    out.print("cavern-colors brightness boost set to {}\n", v);
                } else {
                    tint_strength = std::clamp(v, 0.0f, 1.0f);
                    out.print("cavern-colors tint strength set to {}\n", tint_strength);
                }
                clear_tinted_cache();
                clear_composite_cache();
                return CR_OK;
            }

            if (params[0] == "leaks" && params.size() >= 2) {
                if (params[1] == "on") {
                    leaks_enabled = true;
                    out.print("rough-edge leak tinting: on\n");
                } else if (params[1] == "off") {
                    leaks_enabled = false;
                    clear_composite_cache();
                    out.print("rough-edge leak tinting: off "
                              "(composite cache cleared; rough edges will "
                              "show DF's default untinted overlay until you "
                              "scroll past them so DF re-paints)\n");
                } else {
                    out.printerr("Expected 'on' or 'off'\n");
                    return CR_WRONG_USAGE;
                }
                return CR_OK;
            }

            if (params[0] == "collect-walls" && params.size() >= 2) {
                if (params[1] == "on") {
                    collect_walls = true;
                    wall_texposes_by_mat.clear();
                    wall_overlay_texposes_by_mat.clear();
                    out.print("Collecting wall texposes (base + overlay). "
                              "Scroll past stone walls to populate, then "
                              "run 'cavern-colors dump-cache' or "
                              "'extract-palette'.\n");
                } else if (params[1] == "off") {
                    collect_walls = false;
                    out.print("Stopped collecting wall texposes "
                              "({} mat(s) base / {} mat(s) overlay).\n",
                              wall_texposes_by_mat.size(),
                              wall_overlay_texposes_by_mat.size());
                } else {
                    out.printerr("Expected 'on' or 'off'\n");
                    return CR_WRONG_USAGE;
                }
                return CR_OK;
            }

            if (params[0] == "dump-cache") {
                return dump_cache(out);
            }

            if (params[0] == "sample-cell") {
                return sample_cell(out, params);
            }

            if (params[0] == "dump-texture") {
                return dump_texture(out, params);
            }

            if (params[0] == "mask-texture") {
                return mask_texture(out, params);
            }

            if (params[0] == "paint-overlay") {
                return paint_overlay(out, params);
            }

            if (params[0] == "extract-palette") {
                return extract_palette(out);
            }

            if (params[0] == "dump-wall-graphics") {
                return dump_wall_graphics(out);
            }

            if (params[0] == "dump-baked") {
                return dump_baked(out);
            }

            out.printerr("Unknown argument '{}'\n", params[0]);
            return CR_WRONG_USAGE;
        }
    ));

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
        seed_palettes_from_baked();
        snapshot_overlays();
        break;
    case SC_WORLD_UNLOADED:
        clear_tinted_cache();
        clear_composite_cache();
        material_tints.clear();
        layer_mats.clear();
        wall_texposes_by_mat.clear();
        wall_overlay_texposes_by_mat.clear();
        palette_by_mat.clear();
        observed_overlays.clear();
        overlay_snapshots.clear();
        overlay_lookup.clear();
        overlay_snapshot_table_size = 0;
        break;
    default:
        break;
    }
}
