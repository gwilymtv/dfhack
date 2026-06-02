local _ENV = mkmodule('plugins.ocean-waves')

local argparse = require('argparse')

local ONOFF = {on=true, off=false}

local function parse_onoff(s)
    if ONOFF[s] == nil then
        qerror(("expected 'on' or 'off', got %q"):format(s or ''))
    end
    return ONOFF[s]
end

local function parse_int(s, label)
    local v = tonumber(s)
    if not v then
        qerror(('expected an integer for %s, got %q'):format(label, s or ''))
    end
    return math.floor(v)
end

local actions = {}

function actions.enable() dfhack.run_command('enable', 'ocean-waves') end
function actions.disable() dfhack.run_command('disable', 'ocean-waves') end

actions['paint-test'] = function(args)
    set_paint_test(parse_onoff(args[1]))
end

actions['clobber-check'] = function(args)
    set_clobber_check(parse_onoff(args[1]))
end

actions['list-flows'] = function(args)
    local include_dead, max_print = false, 20
    local positionals = argparse.processArgsGetopt(args, {
        {nil, 'include-dead', handler=function() include_dead = true end},
        {'n', 'max', hasArg=true,
            handler=function(v) max_print = parse_int(v, 'max') end},
    })
    if #positionals > 0 then
        qerror('list-flows takes no positional args')
    end
    list_flows(include_dead, max_print)
end

actions['list-ocean-waves'] = function(args)
    local n = 20
    local positionals = argparse.processArgsGetopt(args, {
        {'n', 'max', hasArg=true,
            handler=function(v) n = parse_int(v, 'max') end},
    })
    if #positionals > 0 then
        qerror('list-ocean-waves takes no positional args')
    end
    list_ocean_waves(n)
end

actions['list-makers'] = function() list_makers() end

actions['edit-maker'] = function(args)
    if #args < 2 then
        qerror('edit-maker <index> key=value [key=value...]')
    end
    local index = parse_int(args[1], 'index')
    for i = 2, #args do
        local k, v = args[i]:match('^([^=]+)=(.+)$')
        if not k or not v then
            qerror(('expected key=value, got %q'):format(args[i]))
        end
        if k == 'interval' then
            set_maker_interval(index, parse_int(v, 'interval'))
        elseif k == 'dir' then
            set_maker_dir(index, v)
        else
            qerror(('unknown maker field %q (use interval, dir)'):format(k))
        end
    end
end

actions['surf-mode'] = function(args)
    if #args ~= 1 then
        qerror("surf-mode <maker|wave>")
    end
    set_surf_mode(args[1])
end

actions['dump-flow'] = function(args)
    local n = 5
    local positionals = argparse.processArgsGetopt(args, {
        {'n', 'max', hasArg=true,
            handler=function(v) n = parse_int(v, 'max') end},
    })
    if #positionals > 0 then
        qerror('dump-flow takes no positional args')
    end
    dump_flow(n)
end

actions['sample-flow'] = function(args)
    if #args == 0 then
        sample_flow(-1, 0, 0)
    elseif #args >= 2 then
        local wx = parse_int(args[1], 'wx')
        local wy = parse_int(args[2], 'wy')
        local wz = args[3] and parse_int(args[3], 'wz')
                or df.global.window_z
        sample_flow(wx, wy, wz)
    else
        qerror('sample-flow takes either no args (uses mouse) or <wx> <wy> [<wz>]')
    end
end

actions['tile-flow'] = function(args)
    if #args == 0 then
        tile_flow(-1, 0, 0)
    elseif #args >= 2 then
        local wx = parse_int(args[1], 'wx')
        local wy = parse_int(args[2], 'wy')
        local wz = args[3] and parse_int(args[3], 'wz')
                or df.global.window_z
        tile_flow(wx, wy, wz)
    else
        qerror('tile-flow takes either no args (uses mouse) or <wx> <wy> [<wz>]')
    end
end

actions['force-spawn'] = function(args)
    if #args < 3 then
        qerror('force-spawn <wx> <wy> <wz> [wave|foam]')
    end
    local wx = parse_int(args[1], 'wx')
    local wy = parse_int(args[2], 'wy')
    local wz = parse_int(args[3], 'wz')
    local kind = args[4] or 'wave'
    if kind ~= 'wave' and kind ~= 'foam' then
        qerror("kind must be 'wave' or 'foam'")
    end
    force_spawn(wx, wy, wz, kind)
end

actions['clear-synthetic'] = function() clear_synthetic() end

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
        dfhack.printerr(("unknown subcommand: %s\navailable: %s")
            :format(action, table.concat(names, ', ')))
        return false
    end
    local rest = {}
    for i = 2, #args do rest[i-1] = args[i] end
    actions[action](rest)
    return true
end

return _ENV
