// prefer-stockpile: pre-attach job ingredients from preferred stockpiles.
//
// When a crafting job is created at a tracked workshop, searches the
// workshop's preferred stockpiles for a suitable item and pre-attaches it
// before DF runs its own nearest-to-dwarf selection.  Falls through to normal
// unrestricted selection if no suitable item is found in any preferred
// stockpile, unlike vanilla stockpile links which block the job entirely.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Debug.h"
#include "LuaTools.h"
#include "MiscUtils.h"
#include "PluginLua.h"
#include "PluginManager.h"

#include "modules/Buildings.h"
#include "modules/EventManager.h"
#include "modules/Items.h"
#include "modules/Job.h"
#include "modules/Materials.h"
#include "modules/Persistence.h"
#include "modules/World.h"

#include "df/building.h"
#include "df/building_furnacest.h"
#include "df/building_stockpilest.h"
#include "df/building_workshopst.h"
#include "df/item.h"
#include "df/item_flags.h"
#include "df/items_other_id.h"
#include "df/job.h"
#include "df/job_item.h"
#include "df/job_item_vector_id.h"
#include "df/job_role_type.h"
#include "df/world.h"

using namespace DFHack;
using df::global::world;

DFHACK_PLUGIN("prefer-stockpile");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);
REQUIRE_GLOBAL(world);

namespace DFHack {
    DBG_DECLARE(prefer_stockpile, log, DebugCategory::LINFO);
}

// ---------------------------------------------------------------------------
// State

// workshop_id -> list of preferred stockpile building IDs
static std::map<int32_t, std::vector<int32_t>> prefer_links;

// Tracks the highest seen job id so listNewlyCreated returns only fresh jobs.
static int last_job_id = 0;

// ---------------------------------------------------------------------------
// Persistence
//
// Site-scoped (per save) storage following the preserve-rooms.cpp pattern:
// one PersistentDataItem for config (enabled flag), another whose string
// payload is a comma-separated list of slash-separated link entries.
// Each entry is "sp_id1/sp_id2/.../ws_id" (workshop id last).

static const auto CONFIG_KEY = std::string(plugin_name) + "/config";
static const auto LINKS_KEY  = std::string(plugin_name) + "/links";

static PersistentDataItem config;

enum ConfigValues {
    CONFIG_IS_ENABLED = 0,
    CONFIG_SCHEMA_VERSION = 1,
};

// Bump on any backwards-incompatible change to the persisted format.
// Lives on the config item (outside the blob) so a migration step can
// inspect it without first parsing potentially-incompatible link data.
static const int SCHEMA_VERSION = 1;

static std::string serialize_links() {
    std::vector<std::string> elems;
    for (auto &kv : prefer_links) {
        std::vector<std::string> strs;
        for (int32_t sp : kv.second) strs.push_back(std::to_string(sp));
        strs.push_back(std::to_string(kv.first));
        elems.push_back(join_strings("/", strs));
    }
    return join_strings(",", elems);
}

static void deserialize_links(const std::string &s) {
    prefer_links.clear();
    if (s.empty()) return;
    std::vector<std::string> elems;
    split_string(&elems, s, ",");
    for (auto &elem : elems) {
        std::vector<std::string> parts;
        split_string(&parts, elem, "/");
        if (parts.size() <= 1) continue;
        int32_t ws_id = string_to_int(parts.back(), -1);
        parts.pop_back();
        if (ws_id < 0) continue;
        std::vector<int32_t> sps;
        for (auto &p : parts) {
            int32_t sp = string_to_int(p, -1);
            if (sp >= 0) sps.push_back(sp);
        }
        if (!sps.empty()) prefer_links[ws_id] = sps;
    }
}

static void persist_links() {
    auto item = World::GetPersistentSiteData(LINKS_KEY);
    if (!item.isValid()) item = World::AddPersistentSiteData(LINKS_KEY);
    item.set_str(serialize_links());
}

// Drops entries whose workshop or stockpile no longer exists.
// Returns true if anything was removed.
static bool cleanup_stale_links() {
    bool changed = false;
    for (auto it = prefer_links.begin(); it != prefer_links.end(); ) {
        if (!df::building::find(it->first)) {
            it = prefer_links.erase(it);
            changed = true;
            continue;
        }
        auto &vec = it->second;
        auto new_end = std::remove_if(vec.begin(), vec.end(),
            [](int32_t sp) { return !df::building::find(sp); });
        if (new_end != vec.end()) {
            vec.erase(new_end, vec.end());
            changed = true;
        }
        if (vec.empty()) {
            it = prefer_links.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

// Fires for both newly created and newly destroyed buildings; we only act
// on the destruction case (find() returns nullptr for a destroyed id).
static void on_building_event(color_ostream &out, void *data) {
    int32_t bld_id = (int32_t)(intptr_t)data;
    if (df::building::find(bld_id)) return;

    bool changed = false;
    if (prefer_links.erase(bld_id)) changed = true;
    for (auto it = prefer_links.begin(); it != prefer_links.end(); ) {
        auto &vec = it->second;
        auto new_end = std::remove(vec.begin(), vec.end(), bld_id);
        if (new_end != vec.end()) {
            vec.erase(new_end, vec.end());
            changed = true;
        }
        if (vec.empty()) it = prefer_links.erase(it);
        else ++it;
    }
    if (changed) persist_links();
}

// ---------------------------------------------------------------------------
// Forward declarations

static std::string building_label(int32_t id);

// ---------------------------------------------------------------------------
// Core logic

// Returns true if item's resolved position falls within sp's tile extents.
//
// Does not use item->getStockpile() (vtable slot 23): that only covers items
// explicitly assigned via DF's container mechanism. Free items like boulders
// use position-based tracking and return nullptr from getStockpile().
// Items::getPosition handles both free items and items inside containers.
//
// This is a simplified form of StockpileInfo::inStockpile() in uicommon.h.
// uicommon.h couldn't be included here because it requires a specific include
// order (full df::item definition + old World API) that conflicted with our
// headers. Revisit if uicommon.h is ever cleaned up.
static bool item_in_stockpile(df::item *item, df::building_stockpilest *sp) {
    if (item->flags.bits.in_inventory) return false;

    df::coord pos = Items::getPosition(item);
    if (!pos.isValid() || pos.z != sp->z) return false;

    int32_t rx = pos.x - sp->room.x;
    int32_t ry = pos.y - sp->room.y;
    if (rx < 0 || rx >= sp->room.width)  return false;
    if (ry < 0 || ry >= sp->room.height) return false;
    if (!sp->room.extents) return true;
    return sp->room.extents[rx + ry * sp->room.width] != 0;
}

// Compact human-readable description of a jitem filter: type, material,
// quantity if >1.  E.g. "boulder of inorganic", "bin of any material x2".
static std::string describe_jitem(const df::job_item *jitem) {
    std::string s = ItemTypeInfo(jitem).toString();
    MaterialInfo mat(jitem);
    if (mat.isValid())
        s += " of " + mat.toString();
    if (jitem->quantity > 1)
        s += fmt::format(" x{}", jitem->quantity);
    return s;
}

// Returns the first unclaimed item in one of the preferred stockpiles that
// passes all job_item filter criteria, or nullptr if none found.
// Logs a rejection-reason summary on the "not found" path to aid debugging.
static df::item *find_preferred_item(
    color_ostream &out,
    int slot_idx,
    const df::job *job,
    const df::job_item *jitem,
    const std::vector<int32_t> &stockpile_ids)
{
    auto other_id = ENUM_ATTR(job_item_vector_id, other, jitem->vector_id);
    // ANY (-1) means unrestricted type — too broad to search efficiently.
    if (other_id == df::items_other_id::ANY) {
        out.print("  slot {}: vector_id=ANY, skipping preferred search\n", slot_idx);
        return nullptr;
    }

    // Resolve building IDs up front; report any that are missing or wrong type.
    std::vector<df::building_stockpilest*> stockpiles;
    for (int32_t sp_id : stockpile_ids) {
        auto *bld = df::building::find(sp_id);
        auto *sp  = bld ? virtual_cast<df::building_stockpilest>(bld) : nullptr;
        if (sp)
            stockpiles.push_back(sp);
        else
            out.print("  slot {}: stockpile id={} not found\n", slot_idx, sp_id);
    }
    if (stockpiles.empty()) return nullptr;

    out.print("  slot {}: searching for {} (vector_id={}, quantity={})\n",
              slot_idx, describe_jitem(jitem),
              ENUM_KEY_STR(job_item_vector_id, jitem->vector_id),
              jitem->quantity);

    // Pre-checks borrowed from buildingplan/buildingplan_cycle.cpp's
    // matchesFilters().  We deliberately skip Job::isSuitableItem(jitem,
    // type, subtype): it constructs MaterialInfo from jitem (the filter's
    // mat_type/mat_index, e.g. "0,-1" = any inorganic) and runs bit-matches
    // against that *generic* material.  For job_item flags backed by a
    // specific material property (flags3.hard, flags2.fire_safe, ...) the
    // generic placeholder won't have those flags, so bits_match fails even
    // for items whose actual material does satisfy the requirement.
    // Job::isSuitableMaterial uses the actual item's material and runs the
    // same iinfo.matches() — both type-side and material-side bit checks —
    // so it covers what isSuitableItem would have done, correctly.
    int n_in_job = 0, n_forbid = 0, n_not_in_sp = 0;
    int n_type_mismatch = 0, n_subtype_mismatch = 0, n_not_buildmat = 0;
    int n_not_metal_ore = 0, n_no_tool_use = 0;
    int n_job_mat_mismatch = 0, n_bad_mat = 0;
    df::item *bad_mat_sample = nullptr;

    // Work orders (and many reaction-driven jobs) leave the jitem filter
    // generic (mat_type=0 mat_index=-1 = any inorganic) and pin the specific
    // material on the job itself.  DF's worker honours both — without this
    // check we'd happily pre-attach a shale boulder to a "make claystone
    // blocks" job.
    bool job_has_mat = job->mat_type >= 0 && job->mat_index >= 0;

    for (auto *item : world->items.other[other_id]) {
        if (!item) continue;
        if (item->flags.bits.in_job)  { n_in_job++;  continue; }
        if (item->flags.bits.forbid)  { n_forbid++;  continue; }

        bool in_preferred = false;
        for (auto *sp : stockpiles) {
            if (item_in_stockpile(item, sp)) { in_preferred = true; break; }
        }
        if (!in_preferred) { n_not_in_sp++; continue; }

        if (jitem->item_type > -1 && jitem->item_type != item->getType()) {
            n_type_mismatch++; continue;
        }
        if (jitem->item_subtype > -1 && jitem->item_subtype != item->getSubtype()) {
            n_subtype_mismatch++; continue;
        }
        if (jitem->flags2.bits.building_material && !item->isBuildMat()) {
            n_not_buildmat++; continue;
        }
        if (jitem->metal_ore > -1 && !item->isMetalOre(jitem->metal_ore)) {
            n_not_metal_ore++; continue;
        }
        if (jitem->has_tool_use > df::tool_uses::NONE
            && !item->hasToolUse(jitem->has_tool_use)) {
            n_no_tool_use++; continue;
        }
        if (job_has_mat
            && (item->getMaterial() != job->mat_type
                || item->getMaterialIndex() != job->mat_index)) {
            n_job_mat_mismatch++; continue;
        }

        if (!Job::isSuitableMaterial(jitem, item->getMaterial(),
                item->getMaterialIndex(), item->getType())) {
            if (!bad_mat_sample) bad_mat_sample = item;
            n_bad_mat++; continue;
        }

        return item;
    }

    out.print("  slot {}: no preferred item"
              " (scanned {} — in_job:{} forbid:{} not_in_sp:{}"
              " type_mismatch:{} subtype_mismatch:{} not_buildmat:{}"
              " not_metal_ore:{} no_tool_use:{}"
              " job_mat_mismatch:{} bad_mat:{})\n",
              slot_idx, (int)world->items.other[other_id].size(),
              n_in_job, n_forbid, n_not_in_sp,
              n_type_mismatch, n_subtype_mismatch, n_not_buildmat,
              n_not_metal_ore, n_no_tool_use,
              n_job_mat_mismatch, n_bad_mat);
    if (bad_mat_sample) {
        out.print("  slot {}: bad_mat sample — item {} type={} subtype={}"
                  " mat_type={} mat_index={}\n",
                  slot_idx, bad_mat_sample->id,
                  ENUM_KEY_STR(item_type, bad_mat_sample->getType()),
                  bad_mat_sample->getSubtype(),
                  bad_mat_sample->getMaterial(),
                  bad_mat_sample->getMaterialIndex());
        out.print("  slot {}: jitem item_type={} item_subtype={}"
                  " mat_type={} mat_index={}"
                  " flags1=0x{:x} flags2=0x{:x} flags3=0x{:x}\n",
                  slot_idx,
                  (int)jitem->item_type, jitem->item_subtype,
                  jitem->mat_type, jitem->mat_index,
                  jitem->flags1.whole, jitem->flags2.whole, jitem->flags3.whole);
    }
    return nullptr;
}

static void process_new_jobs(color_ostream &out) {
    std::vector<df::job*> new_jobs;
    if (!Job::listNewlyCreated(&new_jobs, &last_job_id))
        return;

    for (auto *job : new_jobs) {
        auto *holder = Job::getHolder(job);
        if (!holder) continue;

        auto it = prefer_links.find(holder->id);
        if (it == prefer_links.end()) continue;
        const auto &stockpile_ids = it->second;

        // Which filter slots already have an item committed?
        std::set<int> assigned;
        for (auto *iref : job->items)
            if (iref->job_item_idx >= 0)
                assigned.insert(iref->job_item_idx);

        auto &elems = job->job_items.elements;
        MaterialInfo job_mat(job->mat_type, job->mat_index);
        std::string job_mat_str = job_mat.isValid() ? job_mat.toString() : "(any)";
        out.print("prefer-stockpile: {} (job {}) at {} — {} slot(s), job material: {}\n",
                  Job::getName(job), job->id,
                  building_label(holder->id),
                  (int)elems.size(), job_mat_str);

        for (size_t k = 0; k < job->items.size(); k++) {
            auto *iref = job->items[k];
            out.print("  pre-existing ref[{}]: slot={} role={} item={}\n",
                      (int)k, iref->job_item_idx,
                      ENUM_KEY_STR(job_role_type, iref->role),
                      iref->item ? iref->item->id : -1);
        }

        for (int i = 0; i < (int)elems.size(); i++) {
            if (assigned.count(i)) continue;
            auto *jitem = elems[i];
            if (!jitem) continue;

            auto *item = find_preferred_item(out, i, job, jitem, stockpile_ids);
            if (!item) continue;

            if (Job::attachJobItem(job, item, df::job_role_type::Hauled, i)) {
                // Mirror buildingplan_cycle.cpp: decrement the filter slot's
                // quantity so DF treats it as fully satisfied and skips its
                // own search.  Without this DF runs its selection anyway,
                // attaching a second item as Reagent — that second item then
                // determines the output material, while ours just gets
                // hauled and consumed as a bonus.
                --jitem->quantity;
                out.print("  slot {}: pre-attached item {} ({})\n",
                          i, item->id, Items::getDescription(item, 0, false));
            } else {
                out.print("  slot {}: attachJobItem failed for item {} ({})\n",
                          i, item->id, Items::getDescription(item, 0, false));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Lua API

static std::string building_label(int32_t id) {
    auto *bld = df::building::find(id);
    if (bld)
        return "\"" + Buildings::getName(bld) + "\" (" + std::to_string(id) + ")";
    return "(" + std::to_string(id) + ")";
}

static void print_status(color_ostream &out) {
    if (prefer_links.empty()) {
        out.print("No prefer-stockpile links configured.\n");
        return;
    }
    out.print("prefer-stockpile links:\n");
    for (auto &kv : prefer_links) {
        out.print("  {} ->\n", building_label(kv.first));
        for (int32_t sp_id : kv.second)
            out.print("    {}\n", building_label(sp_id));
    }
}

static void add_link(color_ostream &out, int32_t workshop_id,
                     int32_t stockpile_id) {
    // operator[] default-inserts an empty vector if the key is new. Any
    // early return added before the push_back must erase the empty entry
    // or it becomes a ghost key that persists until world unload.
    auto &vec = prefer_links[workshop_id];
    for (int32_t id : vec) {
        if (id == stockpile_id) {
            out.print("link already exists: {} -> {}\n",
                      building_label(workshop_id), building_label(stockpile_id));
            return;
        }
    }
    vec.push_back(stockpile_id);
    persist_links();
    out.print("added: {} -> {}\n",
              building_label(workshop_id), building_label(stockpile_id));
}

static void remove_link(color_ostream &out, int32_t workshop_id,
                        int32_t stockpile_id) {
    auto it = prefer_links.find(workshop_id);
    if (it == prefer_links.end()) {
        out.print("no links for {}\n", building_label(workshop_id));
        return;
    }
    auto &vec = it->second;
    for (auto vit = vec.begin(); vit != vec.end(); ++vit) {
        if (*vit == stockpile_id) {
            vec.erase(vit);
            out.print("removed: {} -> {}\n",
                      building_label(workshop_id), building_label(stockpile_id));
            if (vec.empty())
                prefer_links.erase(it);
            persist_links();
            return;
        }
    }
    out.print("no link: {} -> {}\n",
              building_label(workshop_id), building_label(stockpile_id));
}

static void remove_all_for_workshop(color_ostream &out, int32_t workshop_id) {
    auto it = prefer_links.find(workshop_id);
    if (it == prefer_links.end()) {
        out.print("no links for {}\n", building_label(workshop_id));
        return;
    }
    out.print("removed all links for {}\n", building_label(workshop_id));
    prefer_links.erase(it);
    persist_links();
}

static void remove_stockpile_from_all(color_ostream &out, int32_t stockpile_id) {
    int count = 0;
    for (auto it = prefer_links.begin(); it != prefer_links.end(); ) {
        auto &vec = it->second;
        for (auto vit = vec.begin(); vit != vec.end(); ++vit) {
            if (*vit == stockpile_id) {
                out.print("removed: {} -> {}\n",
                          building_label(it->first), building_label(stockpile_id));
                vec.erase(vit);
                count++;
                break;
            }
        }
        if (vec.empty())
            it = prefer_links.erase(it);
        else
            ++it;
    }
    if (count == 0)
        out.print("no links found for {}\n", building_label(stockpile_id));
    else
        persist_links();
}

static void list_buildings(color_ostream &out) {
    if (!world) { out.printerr("world not available\n"); return; }

    out.print("Workshops:\n");
    for (auto *bld : world->buildings.all) {
        bool is_workshop = virtual_cast<df::building_workshopst>(bld) != nullptr;
        bool is_furnace  = virtual_cast<df::building_furnacest>(bld)  != nullptr;
        if (!is_workshop && !is_furnace) continue;
        out.print("  id={} \"{}\" pos=({},{},{})\n",
                  bld->id, Buildings::getName(bld),
                  bld->centerx, bld->centery, bld->z);
    }

    out.print("Stockpiles:\n");
    for (auto *bld : world->buildings.all) {
        auto *sp = virtual_cast<df::building_stockpilest>(bld);
        if (!sp) continue;
        out.print("  id={} \"{}\" pos=({},{},{})\n",
                  sp->id, Buildings::getName(sp),
                  sp->centerx, sp->centery, sp->z);
    }
}

// Pushes a flat array of {workshop_id, stockpile_id} pairs.  Lua side
// groups/filters as needed (e.g. by workshop for the workshop overlay,
// by stockpile for the stockpile overlay).
static int prefer_stockpile_getLinks(lua_State *L) {
    lua_newtable(L);
    int outer = lua_gettop(L);
    int idx = 1;
    for (auto &kv : prefer_links) {
        for (int32_t sp_id : kv.second) {
            lua_newtable(L);
            int row = lua_gettop(L);
            Lua::SetField(L, kv.first, row, "workshop_id");
            Lua::SetField(L, sp_id,    row, "stockpile_id");
            lua_rawseti(L, outer, idx++);
        }
    }
    return 1;
}

DFHACK_PLUGIN_LUA_FUNCTIONS {
    DFHACK_LUA_FUNCTION(print_status),
    DFHACK_LUA_FUNCTION(add_link),
    DFHACK_LUA_FUNCTION(remove_link),
    DFHACK_LUA_FUNCTION(remove_all_for_workshop),
    DFHACK_LUA_FUNCTION(remove_stockpile_from_all),
    DFHACK_LUA_FUNCTION(list_buildings),
    DFHACK_LUA_END
};

DFHACK_PLUGIN_LUA_COMMANDS {
    DFHACK_LUA_COMMAND(prefer_stockpile_getLinks),
    DFHACK_LUA_END
};

// ---------------------------------------------------------------------------
// Plugin lifecycle

static command_result do_command(color_ostream &out,
                                 std::vector<std::string> &parameters) {
    bool ok = false;
    if (!Lua::CallLuaModuleFunction(out, "plugins.prefer-stockpile",
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
        "Prefer nearby stockpile items for workshop jobs.",
        do_command));

    // Building event fires for both creation and destruction (we filter to
    // destruction by checking find() == nullptr).  Registered once at init;
    // EventManager keeps the registration across world load/unload cycles.
    EventManager::EventHandler bld_handler(plugin_self, on_building_event, 0);
    EventManager::registerListener(EventManager::EventType::BUILDING, bld_handler);

    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    EventManager::unregisterAll(plugin_self);
    return CR_OK;
}

DFhackCExport command_result plugin_load_site_data(color_ostream &out) {
    config = World::GetPersistentSiteData(CONFIG_KEY);
    if (!config.isValid()) {
        config = World::AddPersistentSiteData(CONFIG_KEY);
        config.set_bool(CONFIG_IS_ENABLED, false);
        config.set_int(CONFIG_SCHEMA_VERSION, SCHEMA_VERSION);
    }
    is_enabled = config.get_bool(CONFIG_IS_ENABLED);

    int saved_version = config.get_int(CONFIG_SCHEMA_VERSION);
    // saved_version == 0 means "pre-schema-marker" — same on-disk format as
    // v1, just unstamped — adopt as v1 silently.  Any other mismatch we
    // refuse to read until a migration is written.
    if (saved_version == 0) {
        config.set_int(CONFIG_SCHEMA_VERSION, SCHEMA_VERSION);
        saved_version = SCHEMA_VERSION;
    }
    if (saved_version != SCHEMA_VERSION) {
        WARN(log, out).print(
            "persisted schema version {} != current {}; migration not "
            "yet implemented, ignoring stored links\n",
            saved_version, SCHEMA_VERSION);
        prefer_links.clear();
    } else {
        auto links_item = World::GetPersistentSiteData(LINKS_KEY);
        deserialize_links(links_item.isValid() ? links_item.get_str() : "");
    }

    if (cleanup_stale_links())
        persist_links();

    // Drain new-job queue so we don't reprocess pre-existing jobs.
    std::vector<df::job*> dummy;
    Job::listNewlyCreated(&dummy, &last_job_id);

    return CR_OK;
}

DFhackCExport command_result plugin_save_site_data(color_ostream &out) {
    if (cleanup_stale_links())
        persist_links();
    return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {
    if (enable && !is_enabled) {
        // Drain pre-existing jobs so we only process ones created post-enable.
        std::vector<df::job*> dummy;
        Job::listNewlyCreated(&dummy, &last_job_id);
    }
    is_enabled = enable;
    if (config.isValid())
        config.set_bool(CONFIG_IS_ENABLED, is_enabled);
    return CR_OK;
}

DFhackCExport void plugin_onupdate(color_ostream &out) {
    if (!is_enabled) return;
    if (!world || !world->map.block_index) return;
    process_new_jobs(out);
}

DFhackCExport void plugin_onstatechange(color_ostream &out,
                                        state_change_event event) {
    // World load is handled by plugin_load_site_data (drains the new-job
    // queue and restores persisted state).  On unload we zero our in-memory
    // state so it can't leak into a subsequently loaded world.
    if (event == SC_WORLD_UNLOADED) {
        prefer_links.clear();
        last_job_id = 0;
        config = PersistentDataItem();
    }
}
