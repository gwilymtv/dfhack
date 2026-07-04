// - full automation of handling mini-pastures over nestboxes:
//   go through all pens, check if they are empty and placed over a nestbox
//   find female tame egg-layer who is not assigned to another pen and assign it to nestbox pasture
//   if a nestbox is claimed by an animal other than the zone's occupant, reassign the zone to the
//   claimer when possible, and warn the player about claims that cannot be reconciled
//   maybe check for minimum age? it's not that useful to fill nestboxes with freshly hatched birds
//   state and sleep setting is saved the first time autonestbox is started (to avoid writing stuff if the plugin is never used)

#include <set>

#include "Debug.h"
#include "LuaTools.h"
#include "PluginManager.h"
#include "PluginLua.h"

#include "modules/Buildings.h"
#include "modules/Gui.h"
#include "modules/Persistence.h"
#include "modules/Units.h"
#include "modules/World.h"

#include "df/building_cagest.h"
#include "df/building_civzonest.h"
#include "df/building_nest_boxst.h"
#include "df/general_ref_building_civzone_assignedst.h"
#include "df/unit.h"
#include "df/world.h"

using std::string;
using std::vector;

using namespace DFHack;

DFHACK_PLUGIN("autonestbox");
DFHACK_PLUGIN_IS_ENABLED(is_enabled);

REQUIRE_GLOBAL(cur_season);
REQUIRE_GLOBAL(world);

namespace DFHack {
    // for configuration-related logging
    DBG_DECLARE(autonestbox, control, DebugCategory::LINFO);
    // for logging during the periodic scan
    DBG_DECLARE(autonestbox, cycle, DebugCategory::LINFO);
}

static const string CONFIG_KEY = string(plugin_name) + "/config";
static PersistentDataItem config;
enum ConfigValues {
    CONFIG_IS_ENABLED = 0,
};

static bool did_complain = false; // avoids message spam
static bool did_complain_unreconciled = false; // ditto, for unreconciled nestbox claims
static const int32_t CYCLE_TICKS = 6067;
static int32_t cycle_timestamp = 0;  // world->frame_counter at last cycle

static command_result df_autonestbox(color_ostream &out, vector<string> &parameters);
static void autonestbox_cycle(color_ostream &out);

DFhackCExport command_result plugin_init(color_ostream &out, std::vector <PluginCommand> &commands) {
    commands.push_back(PluginCommand(
        plugin_name,
        "Auto-assign egg-laying female pets to nestbox zones.",
        df_autonestbox));
    return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {
    if (!Core::getInstance().isMapLoaded() || !World::isFortressMode()) {
        out.printerr("Cannot enable {} without a loaded fort.\n", plugin_name);
        return CR_FAILURE;
    }

    if (enable != is_enabled) {
        is_enabled = enable;
        DEBUG(control,out).print("{} from the API; persisting\n",
                                is_enabled ? "enabled" : "disabled");
        config.set_bool(CONFIG_IS_ENABLED, is_enabled);
    } else {
        DEBUG(control,out).print("{} from the API, but already {}; no action\n",
                                is_enabled ? "enabled" : "disabled",
                                is_enabled ? "enabled" : "disabled");
    }
    return CR_OK;
}

DFhackCExport command_result plugin_load_site_data (color_ostream &out) {
    cycle_timestamp = 0;
    config = World::GetPersistentSiteData(CONFIG_KEY);

    if (!config.isValid()) {
        DEBUG(control,out).print("no config found in this save; initializing\n");
        config = World::AddPersistentSiteData(CONFIG_KEY);
        config.set_bool(CONFIG_IS_ENABLED, is_enabled);
    }

    // we have to copy our enabled flag into the global plugin variable, but
    // all the other state we can directly read/modify from the persistent
    // data structure.
    is_enabled = config.get_bool(CONFIG_IS_ENABLED);
    DEBUG(control,out).print("loading persisted enabled state: {}\n",
                            is_enabled ? "true" : "false");
    did_complain = false;
    did_complain_unreconciled = false;
    return CR_OK;
}

DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event) {
    if (event == DFHack::SC_WORLD_UNLOADED) {
        if (is_enabled) {
            DEBUG(control,out).print("world unloaded; disabling {}\n",
                                    plugin_name);
            is_enabled = false;
        }
    }
    return CR_OK;
}

DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    if (world->frame_counter - cycle_timestamp >= CYCLE_TICKS)
        autonestbox_cycle(out);
    return CR_OK;
}

/////////////////////////////////////////////////////
// configuration interface
//

struct autonestbox_options {
    // whether to display help
    bool help = false;

    // whether to run a cycle right now
    bool now = false;

    static struct_identity _identity;
};
static const struct_field_info autonestbox_options_fields[] = {
    { struct_field_info::PRIMITIVE, "help",  offsetof(autonestbox_options, help),  &df::identity_traits<bool>::identity,    0, 0 },
    { struct_field_info::PRIMITIVE, "now",   offsetof(autonestbox_options, now),   &df::identity_traits<bool>::identity,    0, 0 },
    { struct_field_info::END }
};
struct_identity autonestbox_options::_identity(sizeof(autonestbox_options), &df::allocator_fn<autonestbox_options>, NULL, "autonestbox_options", NULL, autonestbox_options_fields);

static command_result df_autonestbox(color_ostream &out, vector<string> &parameters) {
    if (!Core::getInstance().isMapLoaded() || !World::isFortressMode()) {
        out.printerr("Cannot run {} without a loaded fort.\n", plugin_name);
        return CR_FAILURE;
    }

    autonestbox_options opts;
    if (!Lua::CallLuaModuleFunction(out, "plugins.autonestbox", "parse_commandline", std::make_tuple(&opts, parameters))
            || opts.help)
        return CR_WRONG_USAGE;

    if (opts.now) {
        did_complain = false;
        did_complain_unreconciled = false;
        autonestbox_cycle(out);
    }
    else {
        out << "autonestbox is " << (is_enabled ? "" : "not ") << "running" << std::endl;
    }
    return CR_OK;
}

/////////////////////////////////////////////////////
// cycle logic
//

static bool isInBuiltCage(df::unit *unit) {
    for (auto building : world->buildings.all) {
        if (building->getType() == df::building_type::Cage) {
            df::building_cagest* cage = (df::building_cagest *)building;
            for (auto unitid : cage->assigned_units) {
                if (unitid == unit->id)
                    return true;
            }
        }
    }
    return false;
}

// check if assigned to pen, pit, (built) cage or chain
// note: BUILDING_CAGED is not set for animals (maybe it's used for dwarves who get caged as sentence)
// animals in cages (no matter if built or on stockpile) get the ref CONTAINED_IN_ITEM instead
// removing them from cages on stockpiles is no problem even without clearing the ref
// and usually it will be desired behavior to do so.
static bool isAssigned(df::unit *unit) {
    for (auto ref : unit->general_refs) {
        auto rtype = ref->getType();
        if(rtype == df::general_ref_type::BUILDING_CIVZONE_ASSIGNED
                || rtype == df::general_ref_type::BUILDING_CAGED
                || rtype == df::general_ref_type::BUILDING_CHAIN
                || (rtype == df::general_ref_type::CONTAINED_IN_ITEM && isInBuiltCage(unit))) {
            return true;
        }
    }
    return false;
}

static bool unlikely_to_revert_to_wild(df::unit *unit) {
    if (Units::isDomesticated(unit))
        return true;
    return Units::isTame(unit) && Units::isMarkedForTraining(unit);
}

// units that autonestbox is willing to assign to a nestbox zone,
// disregarding whether they are already assigned somewhere
static bool isEgglayerCandidate(df::unit *unit) {
    return Units::isActive(unit)
        && !Units::isUndead(unit)
        && Units::isFemale(unit)
        && Units::isAdult(unit)
        && unlikely_to_revert_to_wild(unit)
        && Units::isOwnCiv(unit)
        && Units::isEggLayer(unit)
        && !Units::isGrazer(unit) // exclude grazing birds because they're messy
        && !Units::isMerchant(unit) // don't steal merchant mounts
        && !Units::isForest(unit);  // don't steal birds from traders, they hate that
}

static bool isFreeEgglayer(df::unit *unit) {
    return isEgglayerCandidate(unit) && !isAssigned(unit);
}

static df::general_ref_building_civzone_assignedst * createCivzoneRef() {
    return (df::general_ref_building_civzone_assignedst *)
        df::general_ref_building_civzone_assignedst::_identity.instantiate();
}

static bool assignUnitToZone(color_ostream &out, df::unit *unit, df::building_civzonest *zone) {
    auto ref = createCivzoneRef();
    if (!ref) {
        ERR(cycle,out).print("Could not create activity zone reference!\n");
        return false;
    }

    ref->building_id = zone->id;
    unit->general_refs.push_back(ref);
    zone->assigned_units.push_back(unit->id);

    INFO(cycle,out).print("Unit {} ({}) assigned to nestbox zone {} ({})\n",
        unit->id, Units::getRaceName(unit),
        zone->id, zone->name);

    return true;
}

static void eraseUnitFromZoneList(df::building_civzonest *zone, int32_t unit_id) {
    auto &assigned = zone->assigned_units;
    for (size_t idx = 0; idx < assigned.size(); ++idx) {
        if (assigned[idx] == unit_id) {
            assigned.erase(assigned.begin() + idx);
            break;
        }
    }
}

static void removeUnitFromZone(color_ostream &out, df::unit *unit, df::building_civzonest *zone) {
    for (size_t idx = 0; idx < unit->general_refs.size(); ++idx) {
        auto ref = unit->general_refs[idx];
        if (ref->getType() != df::general_ref_type::BUILDING_CIVZONE_ASSIGNED
                || ref->getBuilding() != zone)
            continue;
        unit->general_refs.erase(unit->general_refs.begin() + idx);
        delete ref;
        break;
    }
    eraseUnitFromZoneList(zone, unit->id);

    INFO(cycle,out).print("Unit {} ({}) unassigned from nestbox zone {} ({})\n",
        unit->id, Units::getRaceName(unit),
        zone->id, zone->name);
}

static bool zoneHasUnit(df::building_civzonest *zone, int32_t unit_id) {
    for (int32_t id : zone->assigned_units) {
        if (id == unit_id)
            return true;
    }
    return false;
}

// nestbox must be in upper left corner
// this could be made more flexible
static df::building_nest_boxst * findNestboxAt(df::building_civzonest *zone) {
    df::coord pos(zone->x1, zone->y1, zone->z);
    auto bld = Buildings::findAtTile(pos);
    if (!bld || bld->getType() != df::building_type::NestBox)
        return NULL;
    return virtual_cast<df::building_nest_boxst>(bld);
}

// a zone is in scope for autonestbox if it is an active pen/pasture with a
// nestbox in its upper left corner; returns the nestbox, or NULL if the zone
// is out of scope
static df::building_nest_boxst * getInScopeNestbox(df::building_civzonest *zone) {
    if (!zone || !Buildings::isPenPasture(zone) || !Buildings::isActive(zone))
        return NULL;
    return findNestboxAt(zone);
}

static df::building_civzonest * getAssignedCivzone(df::unit *unit) {
    for (auto ref : unit->general_refs) {
        if (ref->getType() == df::general_ref_type::BUILDING_CIVZONE_ASSIGNED)
            return virtual_cast<df::building_civzonest>(ref->getBuilding());
    }
    return NULL;
}

// records a claimed nestbox that cannot be reconciled with its zone
static void note_unreconciled(color_ostream &out, vector<string> &problems,
        df::building_nest_boxst *nestbox, df::unit *claimer, const string &reason) {
    std::stringstream ss;
    ss << "Nestbox at (" << nestbox->x1 << "," << nestbox->y1 << "," << nestbox->z
       << ") is claimed by " << Units::getReadableName(claimer) << ", but " << reason << ".";
    problems.push_back(ss.str());
    DEBUG(cycle,out).print("{}\n", problems.back());
}

// clears a nestbox claim that the claiming unit cannot or will not use
static void clearRedundantClaim(color_ostream &out, df::building_nest_boxst *nestbox,
        df::unit *claimer, size_t &cleared)
{
    if (claimer)
        INFO(cycle,out).print("clearing redundant claim on nestbox {} by unit {} ({})\n",
            nestbox->id, claimer->id, Units::getReadableName(claimer));
    else
        INFO(cycle,out).print("clearing claim on nestbox {} by missing unit {}\n",
            nestbox->id, nestbox->claimed_by);
    nestbox->claimed_by = -1;
    ++cleared;
}

// scans nestbox zones, reassigning zones to the units that have claimed their
// nestboxes and clearing redundant claims where necessary and possible.
// unclaimed empty zones are collected in free_zones; the ids of all nestboxes
// that are in a manageable zone are collected in in_scope_nestboxes; claimed
// nestboxes that cannot be reconciled with their zones are described in
// problems. returns the number of units assigned to previously empty zones;
// reassigned counts zones where an existing assignment had to be changed to
// match the nestbox claim; cleared counts claims that were cleared
static size_t getFreeNestboxZones(color_ostream &out, vector<df::building_civzonest *> &free_zones,
        std::set<int32_t> &in_scope_nestboxes, vector<string> &problems, size_t &reassigned,
        size_t &cleared)
{
    size_t assigned = 0;
    for (auto zone : world->buildings.other.ZONE_PEN) {
        TRACE(cycle,out).print("scanning pasture {} ({})\n", zone->id, zone->name);
        if (!Buildings::isPenPasture(zone)) {
            TRACE(cycle,out).print("pasture {} is not a pen/pasture\n", zone->id);
            continue;
        }
        if (!Buildings::isActive(zone)) {
            TRACE(cycle,out).print("pasture {} is inactive\n", zone->id);
            continue;
        }

        auto nestbox = findNestboxAt(zone);
        if (!nestbox) {
            TRACE(cycle,out).print("pasture {} does not have nestbox in upper left corner\n", zone->id);
            continue;
        }
        TRACE(cycle,out).print("found nestbox {} in pasture {}\n", nestbox->id, zone->id);
        in_scope_nestboxes.insert(nestbox->id);

        if (nestbox->claimed_by < 0) {
            // an unclaimed zone is free if it has no occupant and no eggs in the nestbox
            if (zone->assigned_units.empty() && nestbox->contained_items.size() == 1)
                free_zones.push_back(zone);
            continue;
        }

        auto claimer = df::unit::find(nestbox->claimed_by);
        if (!claimer || !Units::isActive(claimer)) {
            // claimed by a unit that is dead or gone
            clearRedundantClaim(out, nestbox, claimer, cleared);
            if (zone->assigned_units.empty() && nestbox->contained_items.size() == 1)
                free_zones.push_back(zone);
            continue;
        }
        TRACE(cycle,out).print("nestbox {} is claimed by unit {} ({})\n", nestbox->id,
            nestbox->claimed_by, Units::getReadableName(claimer));

        if (zoneHasUnit(zone, claimer->id)) {
            TRACE(cycle,out).print("unit {} is already assigned to zone {}; nothing to do\n",
                claimer->id, zone->id);
            continue;
        }

        // the claimer may be pastured over this nestbox via a zone that
        // autonestbox does not manage; that arrangement works as it is
        auto cur_zone = getAssignedCivzone(claimer);
        if (cur_zone && cur_zone != zone && cur_zone->z == nestbox->z
                && Buildings::containsTile(cur_zone, df::coord2d(nestbox->x1, nestbox->y1))) {
            TRACE(cycle,out).print("unit {} is pastured over nestbox {} via zone {}; nothing to do\n",
                claimer->id, nestbox->id, cur_zone->id);
            continue;
        }

        // the nestbox is claimed by a unit not assigned to the zone. decide
        // whether to reconcile by reassigning the zone to the claimer or by
        // clearing a claim the claimer cannot or will not use
        bool reconcile_by_assignment = false;
        if (isEgglayerCandidate(claimer)) {
            if (!isAssigned(claimer)) {
                // free egg layer; give it the zone
                reconcile_by_assignment = true;
            } else if (cur_zone) {
                // pull the claimer out of another managed zone unless it is
                // already nesting there
                auto cur_nestbox = getInScopeNestbox(cur_zone);
                if (cur_nestbox && (cur_nestbox->claimed_by != claimer->id || cur_zone == zone))
                    reconcile_by_assignment = true;
            }
        } else if (!isAssigned(claimer)) {
            // a roaming animal that autonestbox will not manage may still come
            // back to use this nestbox; the player has to resolve this one
            note_unreconciled(out, problems, nestbox, claimer,
                "autonestbox will not assign them to a nestbox zone");
            continue;
        }

        if (!reconcile_by_assignment) {
            // the claimer is nesting in a different zone or is pastured,
            // caged, or chained somewhere it cannot use this nestbox
            clearRedundantClaim(out, nestbox, claimer, cleared);
            if (zone->assigned_units.empty() && nestbox->contained_items.size() == 1)
                free_zones.push_back(zone);
            continue;
        }

        if (zone->assigned_units.size() > 1) {
            note_unreconciled(out, problems, nestbox, claimer,
                "the zone has multiple animals assigned to it");
            continue;
        }

        // reassign the zone to the claimer. any displaced occupant becomes a
        // free egg layer again and can be matched to a free zone
        bool moved = false;
        if (cur_zone) {
            removeUnitFromZone(out, claimer, cur_zone);
            moved = true;
        }
        vector<int32_t> occupants = zone->assigned_units;
        for (int32_t unit_id : occupants) {
            moved = true;
            if (auto occupant = df::unit::find(unit_id))
                removeUnitFromZone(out, occupant, zone);
            else
                eraseUnitFromZoneList(zone, unit_id);
        }
        if (assignUnitToZone(out, claimer, zone)) {
            if (moved)
                ++reassigned;
            else
                ++assigned;
        }
    }
    return assigned;
}

// handles nestboxes that are not in a zone autonestbox can manage (e.g. a
// nestbox with no zone at all, or one that is not in the upper left corner of
// its pasture) but are claimed by animals autonestbox should be managing.
// claims held by animals that are assigned to a managed nestbox zone are
// cleared; other claims are warned about
static void findStrayClaims(color_ostream &out, const std::set<int32_t> &in_scope_nestboxes,
        vector<string> &problems, size_t &cleared)
{
    for (auto nestbox : world->buildings.other.NEST_BOX) {
        if (nestbox->claimed_by < 0 || in_scope_nestboxes.count(nestbox->id))
            continue;
        auto claimer = df::unit::find(nestbox->claimed_by);
        if (!claimer || !isEgglayerCandidate(claimer)) {
            TRACE(cycle,out).print("ignoring claim on out of scope nestbox {}\n", nestbox->id);
            continue;
        }
        // if the claimer is pastured in a zone that contains the nestbox, the
        // arrangement works even though autonestbox is not managing it
        auto cur_zone = getAssignedCivzone(claimer);
        if (cur_zone && cur_zone->z == nestbox->z
                && Buildings::containsTile(cur_zone, df::coord2d(nestbox->x1, nestbox->y1))) {
            TRACE(cycle,out).print("unit {} is pastured over its claimed nestbox {}; nothing to do\n",
                claimer->id, nestbox->id);
            continue;
        }
        if (cur_zone && getInScopeNestbox(cur_zone)) {
            // the claimer is managed by autonestbox in another zone; this
            // outside claim just keeps it from nesting where it belongs
            clearRedundantClaim(out, nestbox, claimer, cleared);
            continue;
        }
        note_unreconciled(out, problems, nestbox, claimer,
            "the nestbox is not in a zone that autonestbox can manage");
    }
}

static vector<df::unit *> getFreeEggLayers(color_ostream &out) {
    vector<df::unit *> ret;
    for (auto unit : world->units.active) {
        if (isFreeEgglayer(unit))
            ret.push_back(unit);
    }
    return ret;
}

// rate limit to one message per season
// assumes this gets run at least once a season, which is true when enabled
// running manually with the "now" param also resets did_complain, so we
// won't miss the case where the player happens to run "autonestbox now"
// once a year in Spring
static void rate_limit_complaining() {
    static df::season old_season = df::season::None;
    df::season this_season = *cur_season;
    if (old_season != this_season) {
        did_complain = false;
        did_complain_unreconciled = false;
    }
    old_season = this_season;
}

static size_t assign_nestboxes(color_ostream &out, size_t &reassigned, size_t &cleared) {
    rate_limit_complaining();

    vector<df::building_civzonest *> free_zones;
    std::set<int32_t> in_scope_nestboxes;
    vector<string> problems;
    size_t assigned = getFreeNestboxZones(out, free_zones, in_scope_nestboxes, problems,
        reassigned, cleared);
    vector<df::unit *> free_units = getFreeEggLayers(out);

    const size_t max_idx = std::min(free_zones.size(), free_units.size());
    for (size_t idx = 0; idx < max_idx; ++idx) {
        if (!assignUnitToZone(out, free_units[idx], free_zones[idx])) {
            DEBUG(cycle,out).print("Failed to assign unit to building.\n");
            break;
        }
        DEBUG(cycle,out).print("assigned unit {} to zone {}\n",
                                free_units[idx]->id, free_zones[idx]->id);
        ++assigned;
    }

    // run after the assignment pass so that units that were just assigned to
    // a nestbox zone get any claims they hold on out of scope nestboxes
    // cleared right away
    findStrayClaims(out, in_scope_nestboxes, problems, cleared);

    if (free_zones.size() < free_units.size() && !did_complain) {
        size_t num_needed = free_units.size() - free_zones.size();
        std::stringstream ss;
        ss << "Not enough free nestbox zones! You need to make " << num_needed <<
            " more 1x1 pasture" << (num_needed == 1 ? " and build a nestbox in it." : "s and build nestboxes in them.");
        string announce = ss.str();
        out << announce << std::endl;
        Gui::showAnnouncement("[DFHack autonestbox] " + announce, COLOR_BROWN, true);
        did_complain = true;
    }

    if (!problems.empty() && !did_complain_unreconciled) {
        for (auto &problem : problems)
            out << problem << std::endl;
        string announce = problems[0];
        if (problems.size() > 1) {
            std::stringstream ss;
            ss << problems.size() << " nestboxes have claims that autonestbox"
                " cannot reconcile (see the DFHack console for details).";
            announce = ss.str();
        }
        Gui::showAnnouncement("[DFHack autonestbox] " + announce, COLOR_BROWN, true);
        did_complain_unreconciled = true;
    }
    return assigned;
}

static void autonestbox_cycle(color_ostream &out) {
    // mark that we have recently run
    cycle_timestamp = world->frame_counter;

    DEBUG(cycle,out).print("running autonestbox cycle\n");

    size_t reassigned = 0;
    size_t cleared = 0;
    size_t assigned = assign_nestboxes(out, reassigned, cleared);
    if (assigned > 0) {
        std::stringstream ss;
        ss << assigned << " nestbox" << (assigned == 1 ? " was" : "es were") << " assigned to roaming egg layers.";
        string announce = ss.str();
        out << announce << std::endl;
        Gui::showAnnouncement("[DFHack autonestbox] " + announce, COLOR_GREEN, false);
        // can complain again
        did_complain = false;
    }
    if (reassigned > 0) {
        std::stringstream ss;
        if (reassigned == 1)
            ss << "1 nestbox zone was reassigned to the animal nesting in it.";
        else
            ss << reassigned << " nestbox zones were reassigned to the animals nesting in them.";
        string announce = ss.str();
        out << announce << std::endl;
        Gui::showAnnouncement("[DFHack autonestbox] " + announce, COLOR_GREEN, false);
        // can complain again
        did_complain = false;
    }
    if (cleared > 0) {
        std::stringstream ss;
        ss << cleared << " redundant nestbox claim" << (cleared == 1 ? " was" : "s were") << " cleared.";
        string announce = ss.str();
        out << announce << std::endl;
        Gui::showAnnouncement("[DFHack autonestbox] " + announce, COLOR_GREEN, false);
        // can complain again
        did_complain = false;
    }
}
