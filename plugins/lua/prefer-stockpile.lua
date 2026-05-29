local _ENV = mkmodule('plugins.prefer-stockpile')

local gui = require('gui')
local overlay = require('plugins.overlay')
local widgets = require('gui.widgets')

local function parse_int(s, label)
    local v = tonumber(s)
    if not v then
        qerror(('expected an integer for %s, got %q'):format(label, s or ''))
    end
    return math.floor(v)
end

local actions = {}

-- link-workshop <workshop-id> <stockpile-id>...
actions['link-workshop'] = function(args)
    if #args < 2 then
        qerror('link-workshop <workshop-id> <stockpile-id>...')
    end
    local ws = parse_int(args[1], 'workshop-id')
    for i = 2, #args do
        add_link(ws, parse_int(args[i], 'stockpile-id'))
    end
end

-- link-stockpile <stockpile-id> <workshop-id>...
actions['link-stockpile'] = function(args)
    if #args < 2 then
        qerror('link-stockpile <stockpile-id> <workshop-id>...')
    end
    local sp = parse_int(args[1], 'stockpile-id')
    for i = 2, #args do
        add_link(parse_int(args[i], 'workshop-id'), sp)
    end
end

-- unlink-workshop <workshop-id> [<stockpile-id>...]
-- Omitting stockpile IDs removes all links for that workshop.
actions['unlink-workshop'] = function(args)
    if #args < 1 then
        qerror('unlink-workshop <workshop-id> [<stockpile-id>...]')
    end
    local ws = parse_int(args[1], 'workshop-id')
    if #args == 1 then
        remove_all_for_workshop(ws)
    else
        for i = 2, #args do
            remove_link(ws, parse_int(args[i], 'stockpile-id'))
        end
    end
end

-- unlink-stockpile <stockpile-id> [<workshop-id>...]
-- Omitting workshop IDs removes this stockpile from all workshop links.
actions['unlink-stockpile'] = function(args)
    if #args < 1 then
        qerror('unlink-stockpile <stockpile-id> [<workshop-id>...]')
    end
    local sp = parse_int(args[1], 'stockpile-id')
    if #args == 1 then
        remove_stockpile_from_all(sp)
    else
        for i = 2, #args do
            remove_link(parse_int(args[i], 'workshop-id'), sp)
        end
    end
end

actions['list-buildings'] = function() list_buildings() end

function parse_commandline(args)
    if #args == 0 then
        print_status()
        return true
    end
    local action = args[1]
    if not actions[action] then
        local names = {}
        for k in pairs(actions) do table.insert(names, k) end
        table.sort(names)
        dfhack.printerr(('unknown subcommand: %s\navailable: %s')
            :format(action, table.concat(names, ', ')))
        return false
    end
    local rest = {}
    for i = 2, #args do rest[i-1] = args[i] end
    actions[action](rest)
    return true
end

-- ---------------------------------------------------------------------------
-- Shared helpers used by the overlays and edit dialog

local function building_name(bld)
    if not bld then return '?' end
    return dfhack.buildings.getName(bld)
end

local function get_links_grouped()
    -- C++ returns a flat list of {workshop_id, stockpile_id} pairs.  Group
    -- both ways so workshop and stockpile overlays can answer in O(1).
    local by_ws, by_sp = {}, {}
    for _, pair in ipairs(prefer_stockpile_getLinks()) do
        by_ws[pair.workshop_id] = by_ws[pair.workshop_id] or {}
        table.insert(by_ws[pair.workshop_id], pair.stockpile_id)
        by_sp[pair.stockpile_id] = by_sp[pair.stockpile_id] or {}
        table.insert(by_sp[pair.stockpile_id], pair.workshop_id)
    end
    return by_ws, by_sp
end

local function get_selected_workshop()
    local bld = dfhack.gui.getSelectedBuilding(true)
    if not bld then return nil end
    if df.building_workshopst:is_instance(bld)
       or df.building_furnacest:is_instance(bld) then
        return bld
    end
    return nil
end

local function get_selected_stockpile()
    return dfhack.gui.getSelectedStockpile(true)
end

local function list_candidate_workshops()
    local out = {}
    for _, bld in ipairs(df.global.world.buildings.all) do
        if df.building_workshopst:is_instance(bld)
           or df.building_furnacest:is_instance(bld) then
            table.insert(out, bld)
        end
    end
    return out
end

local function list_candidate_stockpiles()
    local out = {}
    for _, bld in ipairs(df.global.world.buildings.all) do
        if df.building_stockpilest:is_instance(bld) then
            table.insert(out, bld)
        end
    end
    return out
end

-- ---------------------------------------------------------------------------
-- Edit dialog: filtered checklist of candidate buildings

EditDialog = defclass(EditDialog, gui.ZScreenModal)
EditDialog.ATTRS{
    focus_path='prefer-stockpile/edit',
    -- target_bld: workshop or stockpile we're editing links for
    -- target_is_workshop: bool; controls which side is "candidates"
    target_bld=DEFAULT_NIL,
    target_is_workshop=DEFAULT_NIL,
}

function EditDialog:init()
    local title
    if self.target_is_workshop then
        title = ('Preferred stockpiles for %s'):format(building_name(self.target_bld))
    else
        title = ('Workshops preferring %s'):format(building_name(self.target_bld))
    end

    self:addviews{
        widgets.Window{
            frame={w=60, h=24},
            frame_title=title,
            resizable=true,
            resize_min={w=40, h=15},
            subviews={
                widgets.FilteredList{
                    view_id='list',
                    frame={t=0, b=2},
                    edit_below=false,
                    on_submit=self:callback('toggle_selected'),
                },
                widgets.HotkeyLabel{
                    frame={l=0, b=0},
                    key='SELECT',
                    label='Toggle',
                    on_activate=self:callback('toggle_selected'),
                },
                widgets.HotkeyLabel{
                    frame={l=14, b=0},
                    key='CUSTOM_M',
                    label='Pick from map',
                    on_activate=self:callback('enter_map_pick'),
                },
                widgets.HotkeyLabel{
                    frame={l=34, b=0},
                    key='LEAVESCREEN',
                    label='Close',
                    on_activate=self:callback('dismiss'),
                },
            },
        },
    }

    self:refresh()
end

function EditDialog:current_link_set()
    -- Returns a set keyed by candidate-building-id of currently-linked ones.
    local by_ws, by_sp = get_links_grouped()
    local set = {}
    if self.target_is_workshop then
        for _, sp_id in ipairs(by_ws[self.target_bld.id] or {}) do
            set[sp_id] = true
        end
    else
        for _, ws_id in ipairs(by_sp[self.target_bld.id] or {}) do
            set[ws_id] = true
        end
    end
    return set
end

function EditDialog:refresh()
    local candidates
    if self.target_is_workshop then
        candidates = list_candidate_stockpiles()
    else
        candidates = list_candidate_workshops()
    end
    local linked = self:current_link_set()

    local choices = {}
    for _, bld in ipairs(candidates) do
        local marker = linked[bld.id] and '[x]' or '[ ]'
        local name = building_name(bld)
        table.insert(choices, {
            text=('%s %s (#%d)'):format(marker, name, bld.id),
            search_key=name:lower(),
            bld_id=bld.id,
        })
    end
    table.sort(choices, function(a, b) return a.search_key < b.search_key end)

    local list = self.subviews.list
    local prev_filter = list:getFilter()
    list:setChoices(choices)
    if prev_filter and #prev_filter > 0 then list:setFilter(prev_filter) end
end

function EditDialog:enter_map_pick()
    -- Hand off to MapPickScreen; it will re-open this dialog on dismiss.
    local target, is_ws = self.target_bld, self.target_is_workshop
    self:dismiss()
    MapPickScreen{target_bld=target, target_is_workshop=is_ws}:show()
end

function EditDialog:toggle_selected()
    local _, choice = self.subviews.list:getSelected()
    if not choice then return end
    local linked = self:current_link_set()
    local ws_id, sp_id
    if self.target_is_workshop then
        ws_id, sp_id = self.target_bld.id, choice.bld_id
    else
        ws_id, sp_id = choice.bld_id, self.target_bld.id
    end
    if linked[choice.bld_id] then
        remove_link(ws_id, sp_id)
    else
        add_link(ws_id, sp_id)
    end
    self:refresh()
end

local open_edit_dialog -- forward; defined below MapPickScreen

-- ---------------------------------------------------------------------------
-- Map-pick mode: dismiss the dialog and let the user click a candidate on
-- the map.  Multi-toggle (stays active until dismissed); Esc / right-click
-- exits and reopens the dialog.

MapPickScreen = defclass(MapPickScreen, gui.ZScreen)
MapPickScreen.ATTRS{
    focus_path='prefer-stockpile/map-pick',
    -- ZScreen defaults pass_mouse_clicks=true, which routes clicks outside
    -- our (small) banner to the underlying DF viewscreen — so a click on a
    -- stockpile would open its native dialog instead of toggling our link.
    -- Consume all clicks; we route them ourselves in onInput.
    pass_mouse_clicks=false,
    target_bld=DEFAULT_NIL,
    target_is_workshop=DEFAULT_NIL,
}

function MapPickScreen:init()
    self.hover_text = ''
    local what = self.target_is_workshop and 'stockpile' or 'workshop or furnace'
    self:addviews{
        widgets.Panel{
            view_id='banner',
            frame={t=0, l=0, w=70, h=4},
            frame_style=gui.FRAME_THIN,
            frame_background=gui.CLEAR_PEN,
            subviews={
                widgets.Label{
                    frame={t=0, l=1},
                    text={
                        'Click a ', what,
                        ' on the map to toggle its link with ',
                        building_name(self.target_bld), '.',
                    },
                },
                widgets.Label{
                    view_id='hover',
                    frame={t=1, l=1, h=1}, auto_height=false,
                    text='',
                },
            },
        },
    }
end

function MapPickScreen:building_at_cursor()
    local pos = dfhack.gui.getMousePos()
    if not pos then return nil end
    return dfhack.buildings.findAtTile(pos)
end

function MapPickScreen:candidate_matches(bld)
    if not bld or bld.id == self.target_bld.id then return false end
    if self.target_is_workshop then
        return df.building_stockpilest:is_instance(bld)
    end
    return df.building_workshopst:is_instance(bld)
        or df.building_furnacest:is_instance(bld)
end

function MapPickScreen:render(dc)
    local bld = self:building_at_cursor()
    if self:candidate_matches(bld) then
        local linked
        if self.target_is_workshop then
            local by_ws = get_links_grouped()
            linked = false
            for _, id in ipairs(by_ws[self.target_bld.id] or {}) do
                if id == bld.id then linked = true; break end
            end
        else
            local _, by_sp = get_links_grouped()
            linked = false
            for _, id in ipairs(by_sp[self.target_bld.id] or {}) do
                if id == bld.id then linked = true; break end
            end
        end
        local marker = linked and '[x]' or '[ ]'
        self.subviews.hover:setText(('hover: %s %s (#%d)'):format(
            marker, building_name(bld), bld.id))
    else
        self.subviews.hover:setText('')
    end
    MapPickScreen.super.render(self, dc)
end

function MapPickScreen:onInput(keys)
    if keys.LEAVESCREEN or keys._MOUSE_R then
        self:dismiss()
        return true
    end
    if keys._MOUSE_L then
        -- Clicks landing on the banner go to its subviews via the normal
        -- dispatch.  Clicks anywhere else are map clicks for us to handle —
        -- check against the banner panel's own frame, not the screen's
        -- (the screen frame covers the whole map, so checking it would
        -- swallow every click).
        if self.subviews.banner:getMouseFramePos() then
            return MapPickScreen.super.onInput(self, keys)
        end
        local bld = self:building_at_cursor()
        if self:candidate_matches(bld) then
            local ws_id, sp_id
            if self.target_is_workshop then
                ws_id, sp_id = self.target_bld.id, bld.id
            else
                ws_id, sp_id = bld.id, self.target_bld.id
            end
            local linked = false
            local by_ws = get_links_grouped()
            for _, id in ipairs(by_ws[ws_id] or {}) do
                if id == sp_id then linked = true; break end
            end
            if linked then remove_link(ws_id, sp_id)
            else add_link(ws_id, sp_id) end
        end
        return true  -- always consume map clicks while in pick mode
    end
    return MapPickScreen.super.onInput(self, keys)
end

function MapPickScreen:onDismiss()
    open_edit_dialog(self.target_bld, self.target_is_workshop)
end

-- ---------------------------------------------------------------------------

open_edit_dialog = function(target_bld, is_workshop)
    return EditDialog{target_bld=target_bld, target_is_workshop=is_workshop}:show()
end

-- ---------------------------------------------------------------------------
-- Inline overlays
--
-- Follows the orders.lua/SkillRestrictionOverlay pattern: subviews are built
-- once in init() with stable view_ids; per-frame state is pushed in render()
-- by calling setText on the existing Labels.  Avoids tearing down and
-- rebuilding the widget tree every frame.

local function build_inline_panel(self, on_edit)
    -- Compact two-row layout to stack nicely with logistics/autohide
    -- panels on the stockpile view.  Labels use auto_height=false so the
    -- empty initial text doesn't collapse them out of view before the
    -- first render() pushes real values.
    self:addviews{
        widgets.Panel{
            frame_style=gui.FRAME_MEDIUM,
            frame_background=gui.CLEAR_PEN,
            subviews={
                widgets.Label{
                    view_id='header',
                    frame={l=1, t=0, h=1}, auto_height=false,
                    text='',
                },
                widgets.HotkeyLabel{
                    frame={l=1, t=1},
                    key='CUSTOM_P',
                    label='Edit preferred links',
                    on_activate=on_edit,
                },
            },
        },
    }
end

local function update_inline_panel(self, what, count)
    local enabled = isEnabled()
    local text = {}
    if not enabled then
        table.insert(text, {text='DISABLED ', pen=COLOR_YELLOW})
    end
    table.insert(text, ('%s: %d linked'):format(what, count))
    self.subviews.header:setText(text)
end

WorkshopOverlay = defclass(WorkshopOverlay, overlay.OverlayWidget)
WorkshopOverlay.ATTRS{
    desc='Lets you set preferred stockpiles for a workshop or furnace.',
    default_pos={x=-40, y=24},
    default_enabled=true,
    viewscreens={
        'dwarfmode/ViewSheets/BUILDING/Workshop',
        'dwarfmode/ViewSheets/BUILDING/Furnace',
    },
    frame={w=40, h=4},
}

function WorkshopOverlay:init()
    build_inline_panel(self, function()
        local bld = get_selected_workshop()
        if bld then open_edit_dialog(bld, true) end
    end)
end

function WorkshopOverlay:render(dc)
    local bld = get_selected_workshop()
    if not bld then return end
    local by_ws = get_links_grouped()
    update_inline_panel(self, 'Preferred stockpiles', #(by_ws[bld.id] or {}))
    WorkshopOverlay.super.render(self, dc)
end

StockpileOverlay = defclass(StockpileOverlay, overlay.OverlayWidget)
StockpileOverlay.ATTRS{
    desc='Lets you set workshops that prefer this stockpile.',
    -- Stack below the stockpiles plugin's auto-designation panel
    -- (default x=5,y=43,w=49,h=6).
    default_pos={x=5, y=49},
    default_enabled=true,
    viewscreens={
        'dwarfmode/Stockpile/Some/Default',
    },
    frame={w=49, h=4},
}

function StockpileOverlay:init()
    build_inline_panel(self, function()
        local bld = get_selected_stockpile()
        if bld then open_edit_dialog(bld, false) end
    end)
end

function StockpileOverlay:render(dc)
    local bld = get_selected_stockpile()
    if not bld then return end
    local _, by_sp = get_links_grouped()
    update_inline_panel(self, 'Preferred by workshops', #(by_sp[bld.id] or {}))
    StockpileOverlay.super.render(self, dc)
end

OVERLAY_WIDGETS = {
    workshop=WorkshopOverlay,
    stockpile=StockpileOverlay,
}

return _ENV
