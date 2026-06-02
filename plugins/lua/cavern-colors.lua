local _ENV = mkmodule('plugins.cavern-colors')

local argparse = require('argparse')

local MODES = {hybrid=true, mat_rgb=true, basic_color=true}
local ONOFF = {on=true, off=false}

local function parse_nonneg(s, label)
    local v = tonumber(s)
    if not v or v ~= v or v == math.huge or v < 0 then
        qerror(('expected a non-negative number for %s, got %q'):format(label, s))
    end
    return v
end

local function parse_onoff(s)
    if ONOFF[s] == nil then
        qerror(("expected 'on' or 'off', got %q"):format(s))
    end
    return ONOFF[s]
end

local function parse_coord(s, label)
    local v = tonumber(s)
    if not v then
        qerror(('expected an integer for %s, got %q'):format(label, s))
    end
    return math.floor(v)
end

local actions = {}

function actions.enable() dfhack.run_command('enable', 'cavern-colors') end
function actions.disable() dfhack.run_command('disable', 'cavern-colors') end

function actions.mode(args)
    local m = args[1]
    if not m or not MODES[m] then
        qerror("expected mode: hybrid, mat_rgb, or basic_color")
    end
    set_mode(m)
end

function actions.boost(args)
    set_boost(parse_nonneg(args[1] or '', 'boost'))
end

function actions.strength(args)
    set_strength(parse_nonneg(args[1] or '', 'strength'))
end

actions['rough-edges'] = function(args)
    set_rough_edges(parse_onoff(args[1] or ''))
end

actions['z-fog'] = function(args)
    set_z_fog(parse_onoff(args[1] or ''))
end

actions['sample-cell'] = function(args)
    if #args == 0 then
        sample_cell(-1, 0, 0)
    elseif #args >= 2 then
        local wx = parse_coord(args[1], 'wx')
        local wy = parse_coord(args[2], 'wy')
        local wz = args[3] and parse_coord(args[3], 'wz')
                or df.global.window_z
        sample_cell(wx, wy, wz)
    else
        qerror('sample-cell takes either no args or <wx> <wy> [<wz>]')
    end
end

actions['dump-texture'] = function(args)
    local texpos = tonumber(args[1])
    if not texpos then
        qerror('dump-texture <texpos>')
    end
    dump_texture(math.floor(texpos))
end

function parse_commandline(args)
    if #args == 0 then
        print_status()
        return true
    end
    local action = args[1]
    if not actions[action] then
        dfhack.printerr(("unknown subcommand: %s"):format(action))
        return false
    end
    local rest = {}
    for i = 2, #args do rest[i-1] = args[i] end
    actions[action](rest)
    return true
end

return _ENV
