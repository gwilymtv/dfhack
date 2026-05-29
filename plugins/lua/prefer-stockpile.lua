local _ENV = mkmodule('plugins.prefer-stockpile')

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

return _ENV
