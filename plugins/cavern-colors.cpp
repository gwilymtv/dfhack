// Restores per-mineral floor colors in Steam/premium graphics mode.
//
// In ASCII DF, mined floors inherit the mineral's color. In the Steam renderer
// all cavern floors use a single grey stone sprite. This plugin patches
// screentexpos_background (the floor layer) after DF renders each frame,
// replacing mineral floor texposes with pre-tinted variants.

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
#include "df/enabler.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/inorganic_raw.h"
#include "df/map_block.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/world.h"

#include <SDL_pixels.h>
#include <SDL_surface.h>

#include <algorithm>
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
// Base floor sprite (generated procedurally at init)
//
// 32×32 RGBA32 pixels representing a neutral grey stone floor.
// Pixels are stored as uint32_t with byte layout R,G,B,A (SDL_PIXELFORMAT_RGBA32).

static const int TILE_W = 32;
static const int TILE_H = 32;
static std::vector<uint32_t> base_pixels; // kept alive for tinting

static uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

// Fallback: noisy grey stone tile if we can't read DF's own floor art.
static void build_procedural_base() {
    base_pixels.resize(TILE_W * TILE_H);
    for (int y = 0; y < TILE_H; y++) {
        for (int x = 0; x < TILE_W; x++) {
            // Cheap deterministic per-pixel hash → ±16 grey variation
            uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663);
            h = ((h >> 16) ^ h) * 0x45d9f3b;
            int noise = (int)((h >> 5) & 31) - 16;
            int v = 150 + noise;
            if (x == 0 || y == 0 || x == TILE_W - 1 || y == TILE_H - 1) v -= 30;
            v = std::clamp(v, 0, 255);
            base_pixels[y * TILE_W + x] = rgba((uint8_t)v, (uint8_t)v, (uint8_t)v);
        }
    }
}

// Read pixels of DF's stone floor sprite from the texture atlas so tinted
// variants inherit the actual stone-art detail (cracks, shading, etc.) rather
// than a flat colored rectangle. Returns false if any step is unavailable.
static bool try_load_base_from_df() {
    if (!enabler) return false;

    int texpos = -1;
    if (!Screen::findGraphicsTile("FLOORS", 1, 4, &texpos) || texpos <= 0)
        return false;
    if ((size_t)texpos >= enabler->textures.raws.size())
        return false;

    SDL_Surface *src = (SDL_Surface *)enabler->textures.raws[texpos];
    if (!src) return false;

    SDL_PixelFormat *fmt = DFSDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!fmt) return false;
    SDL_Surface *conv = DFSDL_ConvertSurface(src, fmt, 0);
    if (!conv) return false;

    base_pixels.assign(TILE_W * TILE_H, rgba(150, 150, 150));
    int w = std::min(conv->w, TILE_W);
    int h = std::min(conv->h, TILE_H);
    for (int y = 0; y < h; y++) {
        uint8_t *row = (uint8_t *)conv->pixels + y * conv->pitch;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + x * 4;
            base_pixels[y * TILE_W + x] = rgba(p[0], p[1], p[2], p[3]);
        }
    }

    DFSDL_FreeSurface(conv);
    return true;
}

static void build_base_pixels() {
    if (try_load_base_from_df()) {
        DEBUG(log).print("cavern-colors: using DF stone floor sprite as base\n");
    } else {
        build_procedural_base();
        DEBUG(log).print("cavern-colors: falling back to procedural base sprite\n");
    }
}

// ---------------------------------------------------------------------------
// Per-material texpos table (indexed by inorganic_mat)

static std::vector<TexposHandle> material_handles;

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
    // fall back to (or use directly for basic_color mode) the 16-color palette
    int idx = std::clamp((int)mat.basic_color[0], 0, 7) + (mat.basic_color[1] ? 8 : 0);
    tr = DF_PALETTE[idx][0];
    tg = DF_PALETTE[idx][1];
    tb = DF_PALETTE[idx][2];
    return true;
}

// How strongly the tint overrides the original grey. 0.0 = no color change,
// 1.0 = full multiplicative (the harsh "almost solid color" look). 0.55 keeps
// most of the stone-art brightness/detail while still giving a clear color cue.
static const float TINT_STRENGTH = 0.55f;

static TexposHandle make_tinted_handle(const df::material &mat) {
    uint8_t tr, tg, tb;
    get_tint(mat, tr, tg, tb);

    // Lerp tint toward white by (1 - strength) so the multiply preserves more
    // of the base sprite's tonal range.
    uint8_t er = (uint8_t)(255 - (int)((255 - tr) * TINT_STRENGTH));
    uint8_t eg = (uint8_t)(255 - (int)((255 - tg) * TINT_STRENGTH));
    uint8_t eb = (uint8_t)(255 - (int)((255 - tb) * TINT_STRENGTH));

    std::vector<uint32_t> pixels(base_pixels.size());
    for (size_t i = 0; i < base_pixels.size(); i++) {
        uint32_t src = base_pixels[i];
        uint8_t r = (uint8_t)(((src >>  0) & 0xFF) * er / 255);
        uint8_t g = (uint8_t)(((src >>  8) & 0xFF) * eg / 255);
        uint8_t b = (uint8_t)(((src >> 16) & 0xFF) * eb / 255);
        uint8_t a =  (uint8_t)((src >> 24) & 0xFF);
        pixels[i] = rgba(r, g, b, a);
    }
    return Textures::createTile(pixels, TILE_W, TILE_H, true);
}

static void build_material_table() {
    if (!world || world->raws.inorganics.all.empty()) return;

    auto &inorganics = world->raws.inorganics.all;
    material_handles.resize(inorganics.size(), 0);
    for (size_t i = 0; i < inorganics.size(); i++) {
        if (!inorganics[i]) continue;
        material_handles[i] = make_tinted_handle(inorganics[i]->material);
    }
    DEBUG(log).print("built tinted floor tiles for {} inorganic materials\n",
                     material_handles.size());
}

static void clear_material_table() {
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
    material_handles.clear();
}

// ---------------------------------------------------------------------------
// Mineral lookup: last mineral event whose bitmask covers (tx, ty)

static int get_mineral_mat(df::map_block *block, int tx, int ty) {
    int last = -1;
    for (auto *ev : block->block_events) {
        if (ev->getType() != df::block_square_event_type::mineral) continue;
        auto *m = (df::block_square_event_mineralst *)ev;
        if (m->getassignment(tx, ty)) last = m->inorganic_mat;
    }
    return last;
}

// ---------------------------------------------------------------------------
// Render hook

struct cavern_colors_hook : df::viewscreen_dwarfmodest {
    typedef df::viewscreen_dwarfmodest interpose_base;

    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t curtick)) {
        INTERPOSE_NEXT(render)(curtick);

        if (!Screen::inGraphicsMode()) return;
        if (!gps || !gps->main_viewport) return;
        if (material_handles.empty()) return;

        auto *vp = gps->main_viewport;
        auto dims = Gui::getDwarfmodeViewDims().map();

        for (int y = dims.first.y; y <= dims.second.y; y++) {
            for (int x = dims.first.x; x <= dims.second.x; x++) {
                size_t idx = (size_t)(x * vp->dim_y + y);
                if (vp->screentexpos_background[idx] == 0) continue;

                int wx = *window_x + x;
                int wy = *window_y + y;
                int wz = *window_z;
                df::coord pos(wx, wy, wz);

                if (!Maps::isTileVisible(pos)) continue;

                df::tiletype *tt = Maps::getTileType(pos);
                if (!tt) continue;
                if (tileShapeBasic(tileShape(*tt)) != df::tiletype_shape_basic::Floor)
                    continue;
                // Player-built floors (e.g. constructed wooden/stone floor blocks)
                // shouldn't pick up the underlying mineral's color.
                if (tileMaterial(*tt) == df::tiletype_material::CONSTRUCTION)
                    continue;

                df::map_block *block = Maps::getTileBlock(wx, wy, wz);
                if (!block) continue;

                int mat = get_mineral_mat(block, wx & 15, wy & 15);
                if (mat < 0 || (size_t)mat >= material_handles.size()) continue;
                if (!material_handles[mat]) continue;

                long texpos = Textures::getTexposByHandle(material_handles[mat]);
                if (texpos > 0)
                    vp->screentexpos_background[idx] = (int32_t)texpos;
            }
        }
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(cavern_colors_hook, render);

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
                out.print("Usage: cavern-colors mode <hybrid|mat_rgb|basic_color>\n");
                out.print("       cavern-colors enable|disable\n");
                out.print("Current mode: {}\n",
                          color_mode == ColorMode::hybrid     ? "hybrid" :
                          color_mode == ColorMode::mat_rgb    ? "mat_rgb" : "basic_color");
                out.print("Enabled: {}\n", is_enabled ? "yes" : "no");
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
                else {
                    out.printerr("Unknown mode '{}'. Use: hybrid, mat_rgb, basic_color\n",
                                 params[1]);
                    return CR_WRONG_USAGE;
                }
                if (new_mode != color_mode) {
                    color_mode = new_mode;
                    clear_material_table();
                    build_material_table();
                    out.print("cavern-colors mode set to '{}'\n", params[1]);
                }
                return CR_OK;
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
    clear_material_table();
    return CR_OK;
}

DFhackCExport void plugin_onstatechange(color_ostream &out, state_change_event event) {
    switch (event) {
    case SC_WORLD_LOADED:
        build_base_pixels();
        build_material_table();
        break;
    case SC_WORLD_UNLOADED:
        clear_material_table();
        break;
    default:
        break;
    }
}
