// Restores ocean-wave / sea-foam sprites in Steam/premium graphics mode.
//
// In ASCII DF, ocean waves and sea foam are visible flow_info objects of
// flow_type::OceanWave / SeaFoam. The Steam renderer's graphics_flows.txt
// table doesn't include sprites for those types (the binary's hardcoded
// FLOW_* lookup table terminates at FLOW_VAPOR), so the renderer drops them
// silently. The flow_info objects themselves continue to exist at runtime —
// they just don't render. This plugin patches screentexpos_high_flow (the
// viewport's dedicated surface-flow layer) after DF renders each frame,
// writing wave/foam sprite texposes at the cells where ocean-related flows
// live.
//
// Status: SKELETON. The render hook and flow iteration are wired up; sprite
// loading and animation are placeholders. See the debug-command section for
// the validation steps still pending before real sprites are loaded.

#include "Debug.h"
#include "LuaTools.h"
#include "PluginLua.h"
#include "PluginManager.h"
#include "TileTypes.h"
#include "VTableInterpose.h"

#include "modules/Gui.h"
#include "modules/Maps.h"
#include "modules/Screen.h"
#include "modules/Textures.h"

#include "df/block_square_event.h"
#include "df/enabler.h"
#include "df/flow_guide.h"
#include "df/flow_guide_trailing_flowst.h"
#include "df/flow_guide_type.h"
#include "df/flow_info.h"
#include "df/flow_type.h"
#include "df/graphic.h"
#include "df/graphic_viewportst.h"
#include "df/map_block.h"
#include "df/ocean_wave.h"
#include "df/ocean_wave_maker.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/world.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

using namespace DFHack;
using df::global::enabler;
using df::global::gps;
using df::global::window_x;
using df::global::window_y;
using df::global::window_z;
using df::global::world;

DFHACK_PLUGIN("ocean-waves");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(enabler);
REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);
REQUIRE_GLOBAL(world);

namespace DFHack {
    DBG_DECLARE(ocean_waves, log, DebugCategory::LINFO);
}

// ---------------------------------------------------------------------------
// Configuration / state

// When true, paint the placeholder wave sprite at every viewport cell (no
// material filter, no flow_info dependency). Loud and unmissable — if the
// whole viewport doesn't fill with magenta, the high_flow layer or the
// index math is wrong, not the flow scan. Earlier versions filtered by
// tiletype_material::POOL/RIVER/BROOK but DF doesn't classify ocean tiles
// that way (they're typically SOIL/STONE floors with water in the
// designation), so the filter excluded the cells we wanted to test.
static bool paint_test_enabled = false;

// When true, write a sentinel texpos to screentexpos_high_flow[idx] *before*
// INTERPOSE_NEXT(render), then check after whether DF clobbered it. Tells us
// whether DF clears the layer per frame (in which case we're safe writing
// post-render) or preserves it (in which case we may need to suppress DF's
// own writes the way cavern-colors zeroes floor_flag bytes).
static bool clobber_check_enabled = false;
static int clobber_writes = 0;     // sentinel write count for the in-progress frame
static int clobber_survived = 0;   // cells where sentinel survived DF's render
static const int32_t CLOBBER_SENTINEL = 0x7eaffeed;

// Placeholder palette layout. See SPRITE COMBINATIONS comment near
// load_sprites() for the canonical enumeration that a pixel artist would
// commission against.
//
// Dimensions exercised:
//   - ocean_wave (open-ocean front):    direction × intensity × frame
//   - flow_info OceanWave (surf):       expanding × density × frame
//   - flow_info SeaFoam:                expanding × density × frame
//   - liquid_flow ripple (unused so far): timer (single frame)
struct sprite_set {
    TexposHandle ocean_wave[8][2][2];     // [dir 0-7][intensity][frame]
    TexposHandle surf[8][2][3][2];        // [dir][expanding][density][frame]
    TexposHandle foam[2][3][2];           // [expanding][density][frame]
    TexposHandle ripple[4];               // [timer_bucket]
};
static sprite_set sprites;
static bool sprites_loaded = false;

static int density_bucket(int density) {
    // Thresholds derived from sample-flow observations: receding cells with
    // density < ~10 read as foamy white in ASCII; density ≥ 10 reads as
    // base-water blue. Bucket 0 is the foamy/visible cutoff; buckets 1
    // and 2 split the rest into medium and heavy body.
    if (density < 10) return 0;
    if (density < 50) return 1;
    return 2;
}

static int timer_bucket(int timer) {
    return std::clamp((timer - 1) / 2, 0, 3);
}

static int intensity_bucket_for_vis(int vd) {
    // Inverted from vis_duration's natural lifecycle: a freshly-spawned
    // wave (vis_duration=120, far out at sea) renders DIM; a wave nearing
    // shore (vis_duration→0) renders BRIGHT. Matches the way real ocean
    // waves swell as they reach shallow water, and uses the existing
    // sprite buckets as a coarse alpha ramp without needing more variants.
    return vd < 60 ? 1 : 0;
}

// Direction encoding (8 sectors, clockwise from N):
//   0=N  1=NE  2=E  3=SE  4=S  5=SW  6=W  7=NW
// Picked from the sign of (dest - cur) along each axis — diagonals get
// their own bucket. (Earlier 4-direction version forced diagonals into
// E/W via the tie-break, so every NE-bound wave on this embark rendered
// as East-moving.)
static int wave_direction(const df::ocean_wave *w) {
    int dx = w->dest.x - w->cur.x;
    int dy = w->dest.y - w->cur.y;
    int sx = (dx > 0) - (dx < 0);   // -1, 0, +1
    int sy = (dy > 0) - (dy < 0);
    //   sy:   -1  0  +1
    //   sx=-1: NW  W  SW           = 7  6  5
    //   sx= 0:  N  N   S           = 0  0  4
    //   sx=+1: NE  E  SE           = 1  2  3
    static const int table[3][3] = {
        { 7, 6, 5 },
        { 0, 0, 4 },
        { 1, 2, 3 },
    };
    return table[sx + 1][sy + 1];
}

// Unit motion vector per direction, matching wave_direction's encoding.
static const int wave_motion[8][2] = {
    { 0, -1}, { 1, -1}, { 1, 0}, { 1, 1},
    { 0,  1}, {-1,  1}, {-1, 0}, {-1, -1},
};

// Per-maker direction cache. Each ocean_wave_maker has a coastline path
// (where its waves break) and a wave_origin path (where they spawn). The
// vector from origin centroid to coastline centroid is the macro motion
// direction that all waves spawned by this maker travel. Stable across
// the entire maker's region — every surf cell in that region gets the
// same direction, no matter how far inland the splash spreads.
//
// Rebuilt at SC_WORLD_LOADED. Makers don't change during play (they're
// fixed worldgen state) so per-frame caching isn't needed.
struct maker_info {
    int dir;            // 0..7 via wave_direction's encoding (computed)
    int dir_override;   // -1 = no override; else 0..7 used in place of `dir`
    int cx, cy;         // coastline centroid
    int z;
};
static std::vector<maker_info> maker_cache;

static const char *dir_name(int d) {
    static const char *names[8] = {"N","NE","E","SE","S","SW","W","NW"};
    if (d < 0 || d > 7) return "?";
    return names[d];
}

static int parse_dir_name(const std::string &s) {
    if (s == "N")  return 0;
    if (s == "NE") return 1;
    if (s == "E")  return 2;
    if (s == "SE") return 3;
    if (s == "S")  return 4;
    if (s == "SW") return 5;
    if (s == "W")  return 6;
    if (s == "NW") return 7;
    return -1;
}

static void build_maker_cache() {
    maker_cache.clear();
    if (!world) return;
    static const int table[3][3] = {
        { 7, 6, 5 },
        { 0, 0, 4 },
        { 1, 2, 3 },
    };
    for (auto *m : world->event.ocean_wave_makers) {
        if (!m) continue;
        size_t n_coast = m->coastline.x.size();
        size_t n_origin = m->wave_origin.x.size();
        if (n_coast == 0 || n_origin == 0) continue;
        long cx_sum = 0, cy_sum = 0, ox_sum = 0, oy_sum = 0;
        for (size_t i = 0; i < n_coast; i++) {
            cx_sum += m->coastline.x[i];
            cy_sum += m->coastline.y[i];
        }
        for (size_t i = 0; i < n_origin; i++) {
            ox_sum += m->wave_origin.x[i];
            oy_sum += m->wave_origin.y[i];
        }
        int cx = (int)(cx_sum / (long)n_coast);
        int cy = (int)(cy_sum / (long)n_coast);
        int ox = (int)(ox_sum / (long)n_origin);
        int oy = (int)(oy_sum / (long)n_origin);
        int dx = cx - ox;
        int dy = cy - oy;
        int sx = (dx > 0) - (dx < 0);
        int sy = (dy > 0) - (dy < 0);
        maker_info mi;
        mi.dir = table[sx + 1][sy + 1];
        mi.dir_override = -1;
        mi.cx = cx;
        mi.cy = cy;
        mi.z = m->pos.z;
        maker_cache.push_back(mi);
    }
}

// Surf direction strategy. Configurable via `ocean-waves surf-mode`.
//   maker — closest ocean_wave_maker by coastline centroid. Stable across
//           each maker's whole region; all surf inland of one maker's
//           shore section gets the same direction.
//   wave  — nearest spawning ocean_wave by Euclidean distance. More
//           variation, but adjacent surf cells can flip direction based
//           on whichever wave happened to be closest.
// Maker mode is the default; wave mode is closer to per-flow truth at
// the cost of consistency. flow_info.dest is uninitialized garbage for
// ocean flows — confirmed by earlier list-flows dumps — so it's not a
// viable third option.
enum class surf_mode_t { maker, wave };
static surf_mode_t current_surf_mode = surf_mode_t::maker;

// Returns -1 if no candidate found.
static int surf_direction_maker(const df::flow_info *f) {
    int best_dist_sq = INT_MAX;
    int best_dir = -1;
    for (const auto &mi : maker_cache) {
        if (mi.z != f->pos.z) continue;
        int dx = mi.cx - f->pos.x;
        int dy = mi.cy - f->pos.y;
        int dist_sq = dx * dx + dy * dy;
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_dir = mi.dir_override >= 0 ? mi.dir_override : mi.dir;
        }
    }
    return best_dir;
}

// Returns -1 if no candidate found.
static int surf_direction_wave(const df::flow_info *f) {
    df::ocean_wave *best = nullptr;
    int best_dist_sq = INT_MAX;
    for (auto *w : world->event.ocean_waves) {
        if (!w || w->z != f->pos.z || !w->spawn_flows) continue;
        int dx = w->cur.x - f->pos.x;
        int dy = w->cur.y - f->pos.y;
        int dist_sq = dx * dx + dy * dy;
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best = w;
        }
    }
    return best ? wave_direction(best) : -1;
}

static int surf_direction(const df::flow_info *f) {
    int d = -1;
    if (current_surf_mode == surf_mode_t::maker) {
        d = surf_direction_maker(f);
        if (d < 0) d = surf_direction_wave(f); // fallback if no makers
    } else {
        d = surf_direction_wave(f);
        if (d < 0) d = surf_direction_maker(f); // fallback if no waves
    }
    return d >= 0 ? d : 0; // last-resort default
}

// Animation frame derived from per-object data. The chosen fields tick
// only while the simulation is running, so the animation naturally
// freezes when the game is paused — same as DF's own water shimmer.
//
//   ocean_wave  → vis_duration (decrements over the wave's lifetime,
//                 0..120). Picked over move_timer because move_timer's
//                 reset value varies per-wave (random 0..~40) so any
//                 fixed divisor flips frames at very different rates
//                 across waves — produced a fast flicker. vis_duration
//                 has a uniform range across all waves so /16 gives
//                 ~7 frame flips per lifetime, matching surf's pace.
//   flow_info   → density (evolves as the splash builds/dissipates).
//
// Earlier version used enabler->gputicks, which advances in real time
// and produced motion during pause. Replaced for parity with the rest
// of the engine's animations.
static int ocean_wave_anim_frame(const df::ocean_wave *w) {
    return (w->vis_duration / 16) & 1;
}

static int flow_info_anim_frame(const df::flow_info *f) {
    return (f->density / 4) & 1;
}

static int32_t handle_to_texpos(TexposHandle h) {
    long tp = Textures::getTexposByHandle(h);
    return tp > 0 ? (int32_t)tp : 0;
}

static int32_t pick_ocean_wave_texpos(const df::ocean_wave *w) {
    if (!sprites_loaded) return 0;
    int dir = wave_direction(w);
    int intensity = intensity_bucket_for_vis(w->vis_duration);
    int frame = ocean_wave_anim_frame(w);
    return handle_to_texpos(sprites.ocean_wave[dir][intensity][frame]);
}

static int32_t pick_flow_texpos(const df::flow_info *f) {
    if (!sprites_loaded) return 0;
    int expanding = f->expanding ? 1 : 0;
    int density = density_bucket(f->density);
    int frame = flow_info_anim_frame(f);
    if (f->type == df::flow_type::SeaFoam) {
        // Foam is deposited residue — no direction, no motion. Animation
        // shows dissipation (fewer/dimmer dots over time), not streaking.
        return handle_to_texpos(sprites.foam[expanding][density][frame]);
    }
    int dir = surf_direction(f);
    return handle_to_texpos(sprites.surf[dir][expanding][density][frame]);
}

static int32_t pick_ripple_texpos(int timer) {
    if (!sprites_loaded || timer <= 0) return 0;
    return handle_to_texpos(sprites.ripple[timer_bucket(timer)]);
}

// ---------------------------------------------------------------------------
// Block scan

// Visible world-coord rectangle at a given z. Cavern-colors uses
// Gui::getDwarfmodeViewDims().map() for screen-tile bounds; we need
// world-coord bounds, which is (window_x, window_y) + viewport dims.
struct world_rect {
    int x0, y0, x1, y1; // inclusive
};

static world_rect viewport_world_rect(df::graphic_viewportst *vp) {
    world_rect r;
    r.x0 = *window_x;
    r.y0 = *window_y;
    r.x1 = *window_x + vp->dim_x - 1;
    r.y1 = *window_y + vp->dim_y - 1;
    return r;
}

static bool block_overlaps(df::map_block *b, const world_rect &r, int z) {
    if (b->map_pos.z != z) return false;
    if (b->map_pos.x + 15 < r.x0 || b->map_pos.x > r.x1) return false;
    if (b->map_pos.y + 15 < r.y0 || b->map_pos.y > r.y1) return false;
    return true;
}

static bool is_ocean_flow(const df::flow_info *f) {
    if (!f) return false;
    if (f->flags.bits.DEAD) return false;
    return f->type == df::flow_type::OceanWave ||
           f->type == df::flow_type::SeaFoam;
}

// Translate world (wx, wy) into the viewport's screentexpos index, or -1 if
// the cell is outside the viewport's clipping box. Column-major:
// idx = vx * vp->dim_y + vy. (Confirmed by cavern-colors sample-cell
// dumps — NOT row-major.)
static int vp_index(df::graphic_viewportst *vp, int wx, int wy) {
    int vx = wx - *window_x;
    int vy = wy - *window_y;
    if (vx < 0 || vx >= vp->dim_x) return -1;
    if (vy < 0 || vy >= vp->dim_y) return -1;
    return vx * vp->dim_y + vy;
}

// Tint a single viewport at z-level `z`. Used for the main viewport at
// *window_z and for each gps->lower_viewport[i] at *window_z - (i+1).
static void process_viewport(df::graphic_viewportst *vp, int z) {
    if (!vp || !vp->screentexpos_high_flow) return;

    const world_rect r = viewport_world_rect(vp);

    // paint-test path: stamp the brightest surf sprite at every viewport
    // cell. No filter or flow_info dependency — whole viewport should
    // fill if high_flow renders and the index math is right.
    if (paint_test_enabled && sprites_loaded) {
        long tp = Textures::getTexposByHandle(sprites.surf[2][1][2][0]);
        if (tp > 0) {
            for (int vx = 0; vx < vp->dim_x; vx++) {
                for (int vy = 0; vy < vp->dim_y; vy++) {
                    vp->screentexpos_high_flow[vx * vp->dim_y + vy] = (int32_t)tp;
                }
            }
        }
    }

    // Three-pass paint:
    //   Pass 1 (ripples): per-tile liquid_flow.temp_flow_timer > 0.
    //     Unused for waves on the saves checked so far — kept defensively.
    //   Pass 2 (open-ocean fronts): world.event.ocean_waves — discrete
    //     wave-front objects sweeping toward the coast. This is the
    //     wide-coverage data that flow_info doesn't have.
    //   Pass 3 (surf flow_info): wave heads / foam at the coastline.
    //     Painted last so it wins over open-ocean fronts at the shore.
    for (auto *b : world->map.map_blocks) {
        if (!block_overlaps(b, r, z)) continue;
        for (int bx = 0; bx < 16; bx++) {
            for (int by = 0; by < 16; by++) {
                int timer = b->liquid_flow[bx][by].bits.temp_flow_timer;
                if (timer <= 0) continue;
                int wx = b->map_pos.x + bx;
                int wy = b->map_pos.y + by;
                int idx = vp_index(vp, wx, wy);
                if (idx < 0) continue;
                int32_t tp = pick_ripple_texpos(timer);
                if (tp <= 0) continue;
                vp->screentexpos_high_flow[idx] = tp;
            }
        }
    }
    for (auto *w : world->event.ocean_waves) {
        if (!w) continue;
        if (w->z != z) continue;
        int idx = vp_index(vp, w->cur.x, w->cur.y);
        if (idx < 0) continue;
        int32_t tp = pick_ocean_wave_texpos(w);
        if (tp <= 0) continue;
        vp->screentexpos_high_flow[idx] = tp;
    }
    for (auto *b : world->map.map_blocks) {
        if (!block_overlaps(b, r, z)) continue;
        for (auto *f : b->flows) {
            if (!is_ocean_flow(f)) continue;
            int idx = vp_index(vp, f->pos.x, f->pos.y);
            if (idx < 0) continue;
            int32_t tp = pick_flow_texpos(f);
            if (tp <= 0) continue;
            vp->screentexpos_high_flow[idx] = tp;
        }
    }
}

// ---------------------------------------------------------------------------
// Render hook

struct ocean_waves_hook : df::viewscreen_dwarfmodest {
    typedef df::viewscreen_dwarfmodest interpose_base;

    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t curtick)) {
        // Clobber check: stamp sentinels into a few high_flow slots BEFORE
        // letting DF render, then count how many survived afterwards. If
        // DF clears the layer each frame, none survive — confirms we're
        // safe writing post-render without needing a "suppress" mask.
        std::vector<std::pair<int, int32_t>> probes;
        if (clobber_check_enabled && gps && gps->main_viewport &&
            gps->main_viewport->screentexpos_high_flow) {
            auto *vp = gps->main_viewport;
            // Sample 16 cells spread across the viewport.
            for (int i = 0; i < 16; i++) {
                int vx = (i * 7) % vp->dim_x;
                int vy = (i * 11) % vp->dim_y;
                int idx = vx * vp->dim_y + vy;
                probes.push_back({idx, vp->screentexpos_high_flow[idx]});
                vp->screentexpos_high_flow[idx] = CLOBBER_SENTINEL;
            }
            clobber_writes = (int)probes.size();
        }

        INTERPOSE_NEXT(render)(curtick);

        if (!Screen::inGraphicsMode()) return;
        if (!gps || !gps->main_viewport) return;

        if (clobber_check_enabled && gps->main_viewport->screentexpos_high_flow) {
            auto *vp = gps->main_viewport;
            int survived = 0;
            for (auto &p : probes) {
                if (vp->screentexpos_high_flow[p.first] == CLOBBER_SENTINEL)
                    survived++;
                // Restore whatever DF expected to be there so we don't
                // leak the sentinel into the visible frame.
                vp->screentexpos_high_flow[p.first] = p.second;
            }
            clobber_survived = survived;
        }

        // Main viewport at *window_z.
        process_viewport(gps->main_viewport, *window_z);

        // Lower viewports: DF maintains a per-z graphic_viewportst for each
        // fogged floor below the current view (sprites bleeding up through
        // Open tiles). For ocean waves this is the camera-above-sea-level
        // case (looking down from a cliff). DF's shader applies the depth
        // fog after our texpos substitution.
        for (int i = 0; i < 8; i++) {
            df::graphic_viewportst *lvp = gps->lower_viewport[i];
            if (!lvp) continue;
            process_viewport(lvp, *window_z - (i + 1));
        }
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(ocean_waves_hook, render);

// ---------------------------------------------------------------------------
// Debug commands

// Count and dump OceanWave / SeaFoam flow_info objects in blocks overlapping
// the current viewport at the current z. Filterable so we can answer:
//   - do flow_info objects of these types actually exist on this map?
//   - are they concentrated near coastline tiles?
//   - what density / flags do they carry?
static void list_flows(color_ostream &out, bool include_dead, int max_print) {
    if (!world || !gps || !gps->main_viewport) {
        out.printerr("globals not available\n");
        return;
    }
    auto *vp = gps->main_viewport;
    const world_rect r = viewport_world_rect(vp);
    int z = *window_z;
    int waves = 0, foam = 0, dead = 0, printed = 0;
    for (auto *b : world->map.map_blocks) {
        if (!block_overlaps(b, r, z)) continue;
        for (auto *f : b->flows) {
            if (!f) continue;
            bool is_wave = f->type == df::flow_type::OceanWave;
            bool is_foam = f->type == df::flow_type::SeaFoam;
            if (!is_wave && !is_foam) continue;
            if (f->flags.bits.DEAD) { dead++; if (!include_dead) continue; }
            if (is_wave) waves++; else foam++;
            if (printed < max_print) {
                out.print("  {} at ({},{},{}) dest=({},{},{}) "
                          "density={} expanding={} guide_id={} flags={}{}{}\n",
                          is_wave ? "OceanWave" : "SeaFoam",
                          f->pos.x, f->pos.y, f->pos.z,
                          f->dest.x, f->dest.y, f->dest.z,
                          f->density,
                          f->expanding ? 1 : 0,
                          f->guide_id,
                          f->flags.bits.DEAD ? "DEAD " : "",
                          f->flags.bits.FAST ? "FAST " : "",
                          f->flags.bits.CREEPING ? "CREEPING " : "");
                printed++;
            }
        }
    }
    out.print("ocean-waves: viewport at z={} has {} OceanWave, {} SeaFoam "
              "({} dead {})\n",
              z, waves, foam, dead,
              include_dead ? "included" : "excluded");
}

// Dump screentexpos_high_flow at a world coord across the main viewport and
// all lower viewports — to see if DF writes anything there for ocean cells
// currently (we expect 0 / unwritten, that's the bug), and to confirm the
// index math against a coord we can hover over.
static void sample_flow(color_ostream &out, int wx, int wy, int wz) {
    if (!gps || !gps->main_viewport) {
        out.printerr("globals not available\n");
        return;
    }
    if (wx < 0) {
        df::coord m = Gui::getMousePos(true);
        if (!m.isValid()) {
            out.printerr("hover over a tile or pass <wx> <wy> [<wz>]\n");
            return;
        }
        wx = m.x; wy = m.y; wz = m.z;
    }
    out.print("Sampling world=({},{},{})\n", wx, wy, wz);

    // == Flow info objects whose pos matches this tile ==
    df::map_block *b = Maps::getTileBlock(wx, wy, wz);
    out.print("\n== flow_info at this tile ==\n");
    if (!b) {
        out.print("  (no block)\n");
    } else {
        int matches = 0;
        for (auto *f : b->flows) {
            if (!f) continue;
            if (f->pos.x != wx || f->pos.y != wy || f->pos.z != wz) continue;
            const char *kind = "?";
            switch (f->type) {
                case df::flow_type::OceanWave: kind = "OceanWave"; break;
                case df::flow_type::SeaFoam:   kind = "SeaFoam"; break;
                default: kind = "(other)"; break;
            }
            out.print("  [{}] type={}({}) density={} expanding={} "
                      "guide_id={}\n",
                      matches, kind, (int)f->type,
                      f->density, f->expanding ? 1 : 0, f->guide_id);
            out.print("       mat_type={} mat_index={} "
                      "dest=({},{},{})\n",
                      f->mat_type, f->mat_index,
                      f->dest.x, f->dest.y, f->dest.z);
            out.print("       flags: DEAD={} FAST={} CREEPING={}\n",
                      f->flags.bits.DEAD ? 1 : 0,
                      f->flags.bits.FAST ? 1 : 0,
                      f->flags.bits.CREEPING ? 1 : 0);
            matches++;
        }
        if (matches == 0) {
            int block_flows = (int)b->flows.size();
            int ocean = 0;
            for (auto *f : b->flows) if (is_ocean_flow(f)) ocean++;
            out.print("  (no flow_info at this exact tile; block has "
                      "{} flow(s), {} ocean-related)\n", block_flows, ocean);
        }
    }

    // == tile_designation ==
    out.print("\n== tile_designation ==\n");
    df::tile_designation *td = Maps::getTileDesignation(wx, wy, wz);
    if (!td) {
        out.print("  (unavailable)\n");
    } else {
        out.print("  flow_size={} liquid_type={} liquid_static={} "
                  "water_stagnant={} water_salt={}\n",
                  (int)td->bits.flow_size,
                  (int)td->bits.liquid_type,
                  (int)td->bits.liquid_static,
                  (int)td->bits.water_stagnant,
                  (int)td->bits.water_salt);
        out.print("  outside={} light={} subterranean={} hidden={} "
                  "rained={}\n",
                  (int)td->bits.outside,
                  (int)td->bits.light,
                  (int)td->bits.subterranean,
                  (int)td->bits.hidden,
                  (int)td->bits.rained);
    }

    // == liquid_flow ==
    out.print("\n== liquid_flow ==\n");
    if (!b) {
        out.print("  (no block)\n");
    } else {
        auto &lf = b->liquid_flow[wx & 15][wy & 15];
        out.print("  temp_flow_timer={} temp_dir={} perm_flow_dir={} "
                  "sink_dist={}\n",
                  (int)lf.bits.temp_flow_timer,
                  (int)lf.bits.temp_dir,
                  (int)lf.bits.perm_flow_dir,
                  (int)lf.bits.sink_dist);
    }

    // == ocean_wave objects at this position ==
    out.print("\n== ocean_wave at this tile ==\n");
    int wave_matches = 0;
    if (world) {
        for (auto *w : world->event.ocean_waves) {
            if (!w) continue;
            if (w->z != wz) continue;
            if (w->cur.x != wx || w->cur.y != wy) continue;
            out.print("  [{}] cur=({},{},{}) dest=({},{}) spawn_flows={} "
                      "move_timer={} vis_duration={}\n",
                      wave_matches,
                      w->cur.x, w->cur.y, w->z,
                      w->dest.x, w->dest.y,
                      (int)w->spawn_flows,
                      (int)w->move_timer,
                      (int)w->vis_duration);
            wave_matches++;
        }
    }
    if (wave_matches == 0) out.print("  (none)\n");

    // == viewport texpos ==
    out.print("\n== viewport texpos ==\n");
    auto dump_vp = [&](df::graphic_viewportst *vp, const char *label, int z) {
        if (!vp || !vp->screentexpos_high_flow) {
            out.print("  {}: (null)\n", label);
            return;
        }
        int idx = vp_index(vp, wx, wy);
        if (idx < 0) {
            out.print("  {} (z={}): out of viewport\n", label, z);
            return;
        }
        out.print("  {} (z={}) idx={}  high_flow={}  bg={}  floor_flag=0x{:x}\n",
                  label, z, idx,
                  vp->screentexpos_high_flow[idx],
                  vp->screentexpos_background[idx],
                  (unsigned long)(vp->screentexpos_floor_flag
                                  ? vp->screentexpos_floor_flag[idx]
                                  : 0));
    };
    dump_vp(gps->main_viewport, "main_viewport", *window_z);
    for (int i = 0; i < 8; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "lower_viewport[%d]", i);
        dump_vp(gps->lower_viewport[i], buf, *window_z - (i + 1));
    }
}

// Dump the first `n` ocean flows in the viewport with their resolved
// flow_guide (if any). Each OceanWave/SeaFoam flow_info has a guide_id
// referencing world.flow_guides.all; for a flow_guide_trailing_flowst this
// resolves to a 15-coord trail. Hypothesis: ASCII renders the trail too, so
// 1 flow_info covers many tiles — accounting for the wider wave coverage
// than our flow_info count suggests.
static void dump_flow(color_ostream &out, int n) {
    if (!world || !gps || !gps->main_viewport) {
        out.printerr("globals not available\n");
        return;
    }
    auto *vp = gps->main_viewport;
    const world_rect r = viewport_world_rect(vp);
    int z = *window_z;
    int printed = 0;
    for (auto *b : world->map.map_blocks) {
        if (!block_overlaps(b, r, z)) continue;
        for (auto *f : b->flows) {
            if (!is_ocean_flow(f)) continue;
            if (printed >= n) break;
            const char *kind = (f->type == df::flow_type::OceanWave)
                                ? "OceanWave" : "SeaFoam";
            out.print("[{}] {} pos=({},{},{}) density={} expanding={} "
                      "guide_id={}\n",
                      printed, kind,
                      f->pos.x, f->pos.y, f->pos.z,
                      f->density, f->expanding ? 1 : 0, f->guide_id);
            df::flow_guide *g = (f->guide_id >= 0)
                ? df::flow_guide::find(f->guide_id) : nullptr;
            if (!g) {
                out.print("    (no guide)\n");
            } else {
                int gt = (int)g->getType();
                out.print("    guide type={}\n", gt);
                if (gt == (int)df::flow_guide_type::TrailingFlow) {
                    auto *tg = (df::flow_guide_trailing_flowst*)g;
                    int nonzero = 0;
                    for (int i = 0; i < 15; i++) {
                        const auto &c = tg->line[i];
                        if (c.x == 0 && c.y == 0 && c.z == 0) continue;
                        nonzero++;
                        out.print("    line[{}] = ({},{},{})\n",
                                  i, c.x, c.y, c.z);
                    }
                    if (nonzero == 0) {
                        out.print("    line[0..14] all zero\n");
                    }
                }
            }
            printed++;
        }
        if (printed >= n) break;
    }
    if (printed == 0) {
        out.print("no ocean flows in viewport at z={}\n", z);
    }
}

// List ocean_wave_makers with cached direction. Each maker spawns
// waves from `wave_origin` toward `coastline`; the cached direction is
// derived from the centroids of those two paths.
static void list_makers(color_ostream &out) {
    if (!world) { out.printerr("world not available\n"); return; }
    out.print("ocean_wave_makers: {} ({} cached)\n",
              world->event.ocean_wave_makers.size(), maker_cache.size());
    for (size_t i = 0; i < world->event.ocean_wave_makers.size(); i++) {
        auto *m = world->event.ocean_wave_makers[i];
        if (!m) continue;
        out.print("  [{}] pos=({},{},{}) interval={} coastline=[{}] origin=[{}]\n",
                  i, m->pos.x, m->pos.y, m->pos.z, (int)m->interval,
                  m->coastline.x.size(), m->wave_origin.x.size());
        if (i < maker_cache.size()) {
            auto &mi = maker_cache[i];
            out.print("      centroid=({},{},{}) dir={}({})",
                      mi.cx, mi.cy, mi.z, mi.dir, dir_name(mi.dir));
            if (mi.dir_override >= 0)
                out.print(" override={}({})",
                          mi.dir_override, dir_name(mi.dir_override));
            out.print("\n");
        }
    }
}

// PATCH-style maker editors. Each setter modifies exactly one field, so
// the Lua side can parse key=value pairs and dispatch field-by-field
// without juggling sentinel values.
static void set_maker_interval(color_ostream &out, int idx, int value) {
    if (!world) { out.printerr("world not available\n"); return; }
    auto &v = world->event.ocean_wave_makers;
    if (idx < 0 || (size_t)idx >= v.size() || !v[idx]) {
        out.printerr("maker index {} out of range (have {})\n",
                     idx, v.size());
        return;
    }
    int clamped = std::clamp(value, 1, 127);
    v[idx]->interval = (int8_t)clamped;
    out.print("maker[{}].interval = {}\n", idx, clamped);
}

// `dir` is plugin-side only — never modifies DF's coastline/origin paths.
// Overrides the direction surf_direction_maker uses for this maker.
// Pass "auto" or "clear" to remove the override.
static void set_maker_dir(color_ostream &out, int idx, std::string dir_str) {
    if (idx < 0 || (size_t)idx >= maker_cache.size()) {
        out.printerr("maker cache index {} out of range (have {})\n",
                     idx, maker_cache.size());
        return;
    }
    if (dir_str == "auto" || dir_str == "clear") {
        maker_cache[idx].dir_override = -1;
        out.print("maker[{}] dir override cleared (computed = {})\n",
                  idx, dir_name(maker_cache[idx].dir));
        return;
    }
    int d = parse_dir_name(dir_str);
    if (d < 0) {
        out.printerr("unknown direction {}: use N/NE/E/SE/S/SW/W/NW or auto\n",
                     dir_str);
        return;
    }
    maker_cache[idx].dir_override = d;
    out.print("maker[{}] dir override = {}\n", idx, dir_name(d));
}

// List world.event.ocean_waves: the open-ocean wave-front objects (one
// point per ocean_wave; multiple waves spawned together form a visible
// front line). Different data path from flow_info — flow_info is the
// surf-zone aftermath spawned when ocean_wave.spawn_flows transitions.
static void list_ocean_waves(color_ostream &out, int max_print) {
    if (!world || !gps || !gps->main_viewport) {
        out.printerr("globals not available\n");
        return;
    }
    auto *vp = gps->main_viewport;
    const world_rect r = viewport_world_rect(vp);
    int z = *window_z;
    int in_viewport = 0, total = 0, spawning = 0;
    int printed = 0;
    for (auto *w : world->event.ocean_waves) {
        if (!w) continue;
        total++;
        bool in = (w->z == z &&
                   w->cur.x >= r.x0 && w->cur.x <= r.x1 &&
                   w->cur.y >= r.y0 && w->cur.y <= r.y1);
        if (in) in_viewport++;
        if (w->spawn_flows) spawning++;
        if (in && printed < max_print) {
            out.print("  cur=({},{},{}) dest=({},{}) "
                      "spawn_flows={} move_timer={} vis_duration={}\n",
                      w->cur.x, w->cur.y, w->z,
                      w->dest.x, w->dest.y,
                      (int)w->spawn_flows,
                      (int)w->move_timer,
                      (int)w->vis_duration);
            printed++;
        }
    }
    out.print("ocean-waves: total ocean_wave objects: {}, in viewport at "
              "z={}: {}, spawning flows: {}\n",
              total, z, in_viewport, spawning);
    out.print("ocean_wave_makers: {}\n",
              world->event.ocean_wave_makers.size());
}

// Dump map_block.liquid_flow[bx][by] at a world coord, plus a viewport-
// wide histogram of temp_flow_timer values. This is the per-tile data we
// expect to find on ocean cells without a flow_info: the ASCII renderer
// uses it for the ~ wave decoration outside the surf zone.
static void tile_flow(color_ostream &out, int wx, int wy, int wz) {
    if (!world || !gps || !gps->main_viewport) {
        out.printerr("globals not available\n");
        return;
    }
    if (wx < 0) {
        df::coord m = Gui::getMousePos(true);
        if (!m.isValid()) {
            out.printerr("hover over a tile or pass <wx> <wy> [<wz>]\n");
            return;
        }
        wx = m.x; wy = m.y; wz = m.z;
    }

    df::map_block *b = Maps::getTileBlock(wx, wy, wz);
    if (!b) {
        out.print("no block at ({},{},{})\n", wx, wy, wz);
    } else {
        int bx = wx & 15, by = wy & 15;
        auto &lf = b->liquid_flow[bx][by];
        out.print("tile ({},{},{}) liquid_flow: "
                  "temp_flow_timer={} temp_dir={} perm_flow_dir={} "
                  "sink_dist={}\n",
                  wx, wy, wz,
                  (int)lf.bits.temp_flow_timer,
                  (int)lf.bits.temp_dir,
                  (int)lf.bits.perm_flow_dir,
                  (int)lf.bits.sink_dist);
    }

    // Viewport histogram at the current z.
    auto *vp = gps->main_viewport;
    const world_rect r = viewport_world_rect(vp);
    int z = *window_z;
    int histo[8] = {0};
    int total = 0;
    for (auto *blk : world->map.map_blocks) {
        if (!block_overlaps(blk, r, z)) continue;
        for (int xi = 0; xi < 16; xi++) {
            for (int yi = 0; yi < 16; yi++) {
                int twx = blk->map_pos.x + xi;
                int twy = blk->map_pos.y + yi;
                if (twx < r.x0 || twx > r.x1) continue;
                if (twy < r.y0 || twy > r.y1) continue;
                int t = blk->liquid_flow[xi][yi].bits.temp_flow_timer;
                histo[t & 7]++;
                total++;
            }
        }
    }
    out.print("viewport z={} temp_flow_timer histogram "
              "(total tiles={}):\n", z, total);
    for (int i = 0; i < 8; i++) {
        if (histo[i] == 0) continue;
        out.print("  timer={}: {} tile(s){}\n",
                  i, histo[i], i == 0 ? "  (no ripple)" : "");
    }
}

// Synthesize a transient OceanWave flow_info at a coord. Useful when the
// engine isn't producing real ones yet (e.g. testing on a non-coastal map)
// — we just want to confirm the render path lights up. The synthesized flow
// lives in our own static storage; we DO NOT push it into block->flows so
// the engine's own flow management isn't disturbed.
static std::vector<df::flow_info> synthetic_flows;

// Returns count of synthetic flows after the operation.
static int force_spawn(color_ostream &out, int wx, int wy, int wz,
                       std::string kind) {
    df::flow_info f;
    f.type = (kind == "foam") ? df::flow_type::SeaFoam
                              : df::flow_type::OceanWave;
    f.pos = df::coord(wx, wy, wz);
    f.dest = df::coord(wx, wy, wz);
    f.density = 100;
    synthetic_flows.push_back(f);
    out.print("ocean-waves: spawned synthetic {} at ({},{},{}) — total {}\n",
              kind, wx, wy, wz, synthetic_flows.size());
    // TODO: actually render synthetic_flows in process_viewport. Skeleton
    // intentionally leaves this out until we decide whether to fold it
    // into the main iteration path or keep it as a separate test channel.
    return (int)synthetic_flows.size();
}

static void clear_synthetic(color_ostream &out) {
    synthetic_flows.clear();
    out.print("ocean-waves: cleared synthetic flows\n");
}

// ---------------------------------------------------------------------------
// Lua API

static void print_status(color_ostream &out) {
    out.print("Enabled:          {}\n", is_enabled ? "yes" : "no");
    out.print("Sprites loaded:   {} (24 diagnostic placeholders)\n",
              sprites_loaded ? "yes" : "no");
    out.print("Surf mode:        {}\n",
              current_surf_mode == surf_mode_t::maker ? "maker" : "wave");
    out.print("Paint test:       {}\n", paint_test_enabled ? "on" : "off");
    out.print("Clobber check:    {}",   clobber_check_enabled ? "on" : "off");
    if (clobber_check_enabled) {
        out.print("  (last frame: {}/{} sentinels survived DF render)",
                  clobber_survived, clobber_writes);
    }
    out.print("\n");
    out.print("Synthetic flows:  {}\n", synthetic_flows.size());
    out.print("\n");
    out.print("Diagnostic placeholders (32x32 procedural):\n");
    out.print("  ocean_wave (open ocean): blue-white chevron pointing\n");
    out.print("              along (dest - cur). 8 dir x 2 intensity x 2 frame\n");
    out.print("  surf (OceanWave):        chevron pointing along current motion\n");
    out.print("              (expanding=1 toward shore, =0 back to sea).\n");
    out.print("              8 dir x 2 x 3 density x 2 frame\n");
    out.print("  foam (SeaFoam):          omnidirectional stippled bubbles,\n");
    out.print("              dissipating in place. 2 x 3 x 2\n");
    out.print("  ripple (liquid_flow):    pale teal. 4 timer buckets\n");
    out.print("  Animation source: per-object data (ocean_wave.vis_duration,\n");
    out.print("                    flow_info.density) — pauses with the sim\n");
    out.print("\n");
    out.print("Subcommands:\n");
    out.print("  enable | disable\n");
    out.print("  list-flows [--include-dead] [-n MAX]\n");
    out.print("  list-ocean-waves [-n MAX]\n");
    out.print("  list-makers\n");
    out.print("  edit-maker <index> [interval=N] [dir=N|NE|...|NW|auto]\n");
    out.print("  surf-mode maker|wave\n");
    out.print("  dump-flow [-n MAX]\n");
    out.print("  sample-flow [<wx> <wy> [<wz>]]\n");
    out.print("  tile-flow [<wx> <wy> [<wz>]]\n");
    out.print("  paint-test on|off\n");
    out.print("  clobber-check on|off\n");
    out.print("  force-spawn <wx> <wy> <wz> [wave|foam]\n");
    out.print("  clear-synthetic\n");
}

static void set_surf_mode(color_ostream &out, std::string mode) {
    if (mode == "maker") {
        current_surf_mode = surf_mode_t::maker;
    } else if (mode == "wave") {
        current_surf_mode = surf_mode_t::wave;
    } else {
        out.printerr("unknown surf-mode {}: use 'maker' or 'wave'\n", mode);
        return;
    }
    out.print("ocean-waves surf-mode: {}\n", mode);
}

static void set_paint_test(color_ostream &out, bool on) {
    paint_test_enabled = on;
    out.print("ocean-waves paint-test: {}\n", on ? "on" : "off");
}

static void set_clobber_check(color_ostream &out, bool on) {
    clobber_check_enabled = on;
    clobber_writes = clobber_survived = 0;
    out.print("ocean-waves clobber-check: {}\n", on ? "on" : "off");
}

DFHACK_PLUGIN_LUA_FUNCTIONS {
    DFHACK_LUA_FUNCTION(print_status),
    DFHACK_LUA_FUNCTION(set_surf_mode),
    DFHACK_LUA_FUNCTION(set_paint_test),
    DFHACK_LUA_FUNCTION(set_clobber_check),
    DFHACK_LUA_FUNCTION(list_flows),
    DFHACK_LUA_FUNCTION(list_ocean_waves),
    DFHACK_LUA_FUNCTION(list_makers),
    DFHACK_LUA_FUNCTION(set_maker_interval),
    DFHACK_LUA_FUNCTION(set_maker_dir),
    DFHACK_LUA_FUNCTION(dump_flow),
    DFHACK_LUA_FUNCTION(sample_flow),
    DFHACK_LUA_FUNCTION(tile_flow),
    DFHACK_LUA_FUNCTION(force_spawn),
    DFHACK_LUA_FUNCTION(clear_synthetic),
    DFHACK_LUA_END
};

// ---------------------------------------------------------------------------
// Sprite loading
//
// Placeholders: synthesize 32×32 solid-color tiles via Textures::createTile
// so the render pipeline can be validated end-to-end before we have
// artwork. See the diagnostic palette description on `struct sprite_set`.
//
// Once real sprites land, replace with Textures::loadTileset() calls:
//   sprites.foo = Textures::loadTileset(path, 32, 32);
// loadTileset returns handles that survive DF re-allocating
// enabler->textures.raws mid-game. Do NOT call Textures::deleteHandle on
// these — cavern-colors documented a UAF in the refcount loop; the handles
// get released by Textures::cleanup() on DFHack shutdown.

// RGBA32 byte order: R=low, G, B, A=high.
static uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)r
         | ((uint32_t)g << 8)
         | ((uint32_t)b << 16)
         | ((uint32_t)a << 24);
}

static TexposHandle make_solid_tile(uint32_t pixel) {
    std::vector<uint32_t> pixels(32 * 32, pixel);
    return Textures::createTile(pixels, 32, 32);
}

// =========================================================================
// SPRITE COMBINATIONS — canonical enumeration for real-artwork commission
// =========================================================================
//
// Open-ocean wave fronts (df::ocean_wave at world.event.ocean_waves):
//   - Direction:  8 (N, NE, E, SE, S, SW, W, NW) from sign of (dest - cur).
//                 Confirmed needed in practice — every embark tested so
//                 far has waves traveling diagonally.
//   - Intensity:  2 (dim, bright) from vis_duration bucket (≥60 → bright).
//                 Could be 3 for finer fade if artist wants the headroom.
//   - Frames:     2 per (direction, intensity) for visible motion. Real
//                 art probably wants 3-4 for smoother animation.
//   Placeholder count: 8 × 2 × 2 = 32
//   Recommended commission target: 8 × 2 × 4 = 64.
//
// Surf splash (df::flow_info with type=OceanWave, on block->flows):
//   - Direction:      8 — inferred per-flow from the nearest spawning
//                     ocean_wave (flow_info.dest is garbage for ocean).
//                     The splash leans toward motion direction.
//   - Expanding flag: 2 (1 = incoming/approach; 0 = peaked/withdraw).
//                     Distinct shapes: build-up vs draw-back.
//   - Density:        3 (low / mid / high) from f->density / 34. In the
//                     placeholder this drives alpha only (constant
//                     chevron size for legibility). Real art can encode
//                     density via size, alpha, or both.
//   - Frames:         2 per (dir, expanding, density).
//   Placeholder count: 8 × 2 × 3 × 2 = 96
//   Recommended commission target: 8 × 2 × 3 × 4 = 192
//
// Sea foam (df::flow_info with type=SeaFoam):
//   - No direction. Foam is deposited residue at the spot where a wave
//     broke, then dissipates in place. Animation is dissipation (fewer
//     bubbles + lower alpha at the late frame), not motion.
//   - Expanding: 2 (still appearing vs already dissipating).
//   - Density: 3 (low / mid / high) — drives bubble count and alpha.
//   - Frames: 2 (full vs fading).
//   Placeholder count: 2 × 3 × 2 = 12
//   Recommended commission target: 2 × 3 × 4 = 24
//
// Liquid-flow ripple (map_block.liquid_flow[].temp_flow_timer > 0):
//   - Empty on every save tested so far. Plumbing kept defensively.
//   - If a save does populate it: 4 timer buckets, 1-2 frames.
//   Placeholder count: 4
//   Recommended commission target: skip until a save proves the field is
//   actually populated. If it is: 4 buckets × 2 frames = 8.
//
// Animation tick source: per-object data (ocean_wave.move_timer,
// flow_info.density). Frame advances when the sim does; freezes during
// pause. Matches the behavior of DF's own water/grass shimmer.
//
// All sprites are 32×32 RGBA32. createTile/loadTileset both accept that.
//
// Reducing the artist's commission:
//
//   1. Rotate-at-load. Artist supplies N-facing sprites only; the plugin
//      transposes for 90° rotations (exact) and bilinear-rotates for
//      diagonals (acceptable for water imagery).
//   2. Alpha-derive density and intensity variants. These dimensions are
//      pure visibility/strength; the plugin can generate them from a
//      single base sprite by multiplying alpha. Direction, expanding,
//      and frame all need distinct shapes and cannot be derived.
//
// Absolute-minimum commission with both techniques:
//   ocean_wave:  1 dir × 1 intensity × 4 frame =  4 sprites
//   surf:        1 dir × 2 expanding × 1 density × 4 frame =  8 sprites
//   foam:                2 expanding × 1 density × 4 frame =  8 sprites
//   ripple:      skip until populated                       =  0 sprites
//   --------------------------------------------------------------------
//   TOTAL:                                                  ≈ 20 sprites
//
// The plugin handles the multiplication out to ~228 internal variants.
// Artist can opt into hand-drawn density / intensity tiers later for
// finer fidelity if they want.
//
// Lower-viewport (z-fog) handling: DF applies depth fog after our texpos
// substitution, so the same sprite set works at all z without per-fog
// variants.
// =========================================================================

static void blit_dot(std::vector<uint32_t> &p, int cx, int cy, int radius,
                     uint32_t color) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx*dx + dy*dy > radius*radius) continue;
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= 32 || y < 0 || y >= 32) continue;
            p[y * 32 + x] = color;
        }
    }
}

// Thick line from (x0,y0) to (x1,y1) drawn as overlapping discs of the
// given radius. Float endpoints; rendering is rounded to the nearest pixel.
static void draw_thick_line(std::vector<uint32_t> &p,
                            double x0, double y0, double x1, double y1,
                            int radius, uint32_t color) {
    double dx = x1 - x0;
    double dy = y1 - y0;
    double len = std::sqrt(dx * dx + dy * dy);
    int steps = std::max(1, (int)std::ceil(len * 1.5));
    for (int i = 0; i <= steps; i++) {
        double t = (double)i / steps;
        int ix = (int)std::lround(x0 + t * dx);
        int iy = (int)std::lround(y0 + t * dy);
        blit_dot(p, ix, iy, radius, color);
    }
}

// Chevron pointed at (tip_x, tip_y) facing unit vector (fx, fy). Two arms
// extend back_dist back along -(fx, fy) and ±perp_dist along the
// perpendicular (fy, -fx). Drawn with `radius`-thick lines.
static void draw_chevron(std::vector<uint32_t> &p,
                         double tip_x, double tip_y,
                         double fx, double fy,
                         double back_dist, double perp_dist,
                         int radius, uint32_t color) {
    double left_x  = tip_x - back_dist * fx + perp_dist * fy;
    double left_y  = tip_y - back_dist * fy - perp_dist * fx;
    double right_x = tip_x - back_dist * fx - perp_dist * fy;
    double right_y = tip_y - back_dist * fy + perp_dist * fx;
    draw_thick_line(p, tip_x, tip_y, left_x,  left_y,  radius, color);
    draw_thick_line(p, tip_x, tip_y, right_x, right_y, radius, color);
}

// Open-ocean wave: a chevron pointed in the direction of travel, sliding
// along that direction across frames. Wide perpendicular reach makes the
// V read as a wave-front sweep when neighbouring cells render their own
// chevrons too. Outer (dim, thick) pass plus crest (bright, thin) pass
// gives the V a soft fade.
static TexposHandle gen_ocean_wave(int dir, int intensity, int frame) {
    std::vector<uint32_t> px(32 * 32, 0);
    uint8_t crest_a = intensity ? 230 : 140;
    uint8_t outer_a = intensity ? 150 : 80;
    uint32_t crest = rgba(230, 245, 255, crest_a);
    uint32_t outer = rgba(140, 190, 230, outer_a);

    int mx = wave_motion[dir][0];
    int my = wave_motion[dir][1];
    double motion_len = std::sqrt((double)(mx * mx + my * my));
    double fx = mx / motion_len;
    double fy = my / motion_len;

    // Tip slides 6 pixels along motion direction across the cycle.
    double slide = frame ? 3.0 : -3.0;
    double tip_x = 16 + slide * fx;
    double tip_y = 16 + slide * fy;
    double back = 6.0;
    double perp = 10.0;

    draw_chevron(px, tip_x, tip_y, fx, fy, back, perp, 2, outer);
    draw_chevron(px, tip_x, tip_y, fx, fy, back, perp, 0, crest);

    return Textures::createTile(px, 32, 32);
}

// Surf splash: a colored tile background (white or blue) representing
// what ASCII shows as the cell color, with purple chevrons over it
// indicating direction. Solid colors only, no alpha.
//
// Background semantics from sample-flow observations:
//   expanding=1                            → white (incoming crest is always foamy)
//   expanding=0 + density bucket 0 (<10)   → white (foamy receding drag)
//   expanding=0 + density bucket 1+ (≥10)  → blue  (thick water-look body)
static TexposHandle gen_surf(int dir, int expanding, int density, int frame) {
    std::vector<uint32_t> px(32 * 32, 0);

    bool foam_visible = expanding || density == 0;
    uint32_t bg = foam_visible
        ? rgba(0xFF, 0xFF, 0xFF, 0xFF)    // #FFFFFF
        : rgba(0x26, 0x97, 0xFF, 0xFF);   // #2697FF
    std::fill(px.begin(), px.end(), bg);

    uint32_t chev_color = rgba(0x8C, 0x66, 0xFE, 0xFF); // #8C66FE

    int mx = wave_motion[dir][0];
    int my = wave_motion[dir][1];
    double motion_len = std::sqrt((double)(mx * mx + my * my));
    int sign = expanding ? 1 : -1;
    double fx = mx * sign / motion_len;
    double fy = my * sign / motion_len;

    double slide = 2.0 + frame * 3.0;
    double tip_x = 16 + slide * fx;
    double tip_y = 16 + slide * fy;
    draw_chevron(px, tip_x, tip_y, fx, fy, 5.0, 7.0, 1, chev_color);

    double trail_tip_x = 16 - 4 * fx;
    double trail_tip_y = 16 - 4 * fy;
    draw_chevron(px, trail_tip_x, trail_tip_y, fx, fy, 3.0, 4.0, 0, chev_color);

    return Textures::createTile(px, 32, 32);
}

// Sea foam: omnidirectional scattered bubbles, no motion. Foam is
// deposited residue at the spot where a wave broke. Frame 1 has fewer
// bubbles + lower alpha than frame 0, suggesting dissipation in place.
// Same RNG seed across frames so the bubbles stay at the same positions
// (the late frame just drops some of them) — this gives "fade" rather
// than "flicker".
static TexposHandle gen_foam(int expanding, int density, int frame) {
    std::vector<uint32_t> px(32 * 32, 0);
    int n_full = 6 + density * 5;                    // 6, 11, 16 at frame 0
    int n_dots = frame == 0 ? n_full : (n_full * 2 / 3);
    uint8_t base_alpha = std::clamp(180 + density * 25, 80, 240);
    uint8_t alpha = frame == 0 ? base_alpha : (uint8_t)(base_alpha * 2 / 3);
    uint32_t color = expanding
        ? rgba(255, 255, 240, alpha)
        : rgba(220, 230, 240, alpha);
    // Seed depends only on expanding+density so frame 0 and frame 1 share
    // dot positions; frame 1 just iterates fewer of them.
    uint32_t seed = (uint32_t)((expanding << 4) | density);
    uint32_t s = seed * 2654435761u + 1u;
    auto next = [&]() { s = s * 1664525u + 1013904223u; return s; };
    for (int i = 0; i < n_dots; i++) {
        int x = (int)(next() % 28) + 2;
        int y = (int)(next() % 28) + 2;
        blit_dot(px, x, y, 1, color);
    }
    return Textures::createTile(px, 32, 32);
}

static void load_sprites(color_ostream &out) {
    // ocean_wave: 8 dir × 2 intensity × 2 frame = 32
    for (int d = 0; d < 8; d++)
        for (int i = 0; i < 2; i++)
            for (int f = 0; f < 2; f++)
                sprites.ocean_wave[d][i][f] = gen_ocean_wave(d, i, f);
    // surf: 8 dir × 2 expanding × 3 density × 2 frame = 96
    for (int dir = 0; dir < 8; dir++)
        for (int e = 0; e < 2; e++)
            for (int d = 0; d < 3; d++)
                for (int f = 0; f < 2; f++)
                    sprites.surf[dir][e][d][f] = gen_surf(dir, e, d, f);
    // foam: 2 expanding × 3 density × 2 frame = 12 (no direction —
    // foam is deposited residue, dissipates in place)
    for (int e = 0; e < 2; e++)
        for (int d = 0; d < 3; d++)
            for (int f = 0; f < 2; f++)
                sprites.foam[e][d][f] = gen_foam(e, d, f);
    // ripple: 4 timer buckets, single frame. Pale teal.
    const uint8_t timer_alpha[4] = { 60, 100, 140, 180 };
    for (int b = 0; b < 4; b++)
        sprites.ripple[b] = make_solid_tile(rgba(80, 160, 190, timer_alpha[b]));
    sprites_loaded = true;
    DEBUG(log, out).print(
        "loaded 144 procedural placeholder sprites "
        "(32 ocean_wave + 96 surf + 12 foam + 4 ripple)\n");
}

// ---------------------------------------------------------------------------
// Plugin lifecycle

static command_result do_command(color_ostream &out,
                                 std::vector<std::string> &parameters) {
    bool ok = false;
    if (!Lua::CallLuaModuleFunction(out, "plugins.ocean-waves",
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
        "Restore ocean-wave and sea-foam sprites in premium graphics mode.",
        do_command));
    return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {
    if (!INTERPOSE_HOOK(ocean_waves_hook, render).apply(enable))
        return CR_FAILURE;
    is_enabled = enable;
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    return CR_OK;
}

DFhackCExport void plugin_onstatechange(color_ostream &out,
                                        state_change_event event) {
    switch (event) {
    case SC_WORLD_LOADED:
        load_sprites(out);
        build_maker_cache();
        break;
    case SC_WORLD_UNLOADED:
        sprites_loaded = false;
        maker_cache.clear();
        synthetic_flows.clear();
        break;
    default:
        break;
    }
}
