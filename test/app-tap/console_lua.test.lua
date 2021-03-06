#!/usr/bin/env tarantool
--
-- vim: ts=4 sw=4 et

local console = require('console')
local socket = require('socket')
local tap = require('tap')
local fio = require('fio')

local EOL = ';'
local CONSOLE_SOCKET = 'console-lua.sock'

--
-- Set Lua output mode.
local function set_lua_output(client, opts)
    local opts = opts or {}
    local mode = opts.block and 'lua,block' or 'lua'
    client:write(('\\set output %s\n'):format(mode))
    assert(client:read(EOL), 'true' .. EOL, 'set lua output mode')
end

--
-- Start console and setup a client.
local function start_console()
    -- Make sure not stale sockets are
    -- left from previous runs.
    fio.unlink(CONSOLE_SOCKET)

    local server = console.listen('unix/:./' .. CONSOLE_SOCKET)
    assert(server ~= nil, 'console.listen started')

    local client = socket.tcp_connect('unix/', CONSOLE_SOCKET)
    assert(client ~= nil, 'connect to console')
    local handshake = client:read({chunk = 128})
    assert(string.match(handshake, '^Tarantool .*console') ~= nil, 'handshake')

    -- Switch to Lua output mode.
    set_lua_output(client, {block = false})
    return server, client
end

--
-- Disconnect from console and stop it.
local function stop_console(server, client)
    client:close()
    server:close()

    local new_client = socket.tcp_connect('unix/', CONSOLE_SOCKET)
    assert(new_client == nil, 'console.listen stopped')
end

--
-- Give `{x}` for a scalar `x`, just `x` otherwise.
local function totable(x)
    return type(x) == 'table' and x or {x}
end

--
-- Execute a list of statements, show requests and responses.
local function execute_statements(test, client, statements, name)
    test:test(name, function(test)
        test:plan(2 * #statements)

        for _, stmt in ipairs(statements) do
            local request = stmt .. '\n'
            local res = client:write(request)
            test:ok(res ~= nil, ('-> [[%s]]'):format(request:gsub('\n', '\\n')))

            local response = client:read(EOL)
            test:ok(response ~= nil and response:endswith(EOL),
                    ('<- [[%s]]'):format(response))
        end
    end)
end

--
-- Execute a statement and verify its response.
local function execute_and_verify(test, client, input, exp_output, name)
    test:test(name, function(test)
        test:plan(2)

        local res = client:write(input .. '\n')
        test:ok(res ~= nil, ('-> [[%s]]'):format(input))

        local exp = exp_output .. EOL
        local res = client:read(EOL)
        test:is(res, exp, ('<- [[%s]]'):format(exp:gsub('\n', '\\n')))
    end)
end

--
-- Test cases table:
-- @name:       Name of the test, mandatory
-- @prepare:    Preparation script, optional
-- @opts:       Console options, optional
-- @input:      Input statement to execute, mandatory
-- @expected:   Expected results to compare with, mandatory
-- @cleanup:    Cleanup script to run after the test, optional
local cases = {
    {
        name        = 'multireturn line mode',
        prepare     = 'a = {1, 2, 3}',
        opts        = {block = false},
        input       = '1, 2, nil, a',
        expected    = '1, 2, box.NULL, {1, 2, 3}',
        cleanup     = 'a = nil',
    }, {
        name        = 'multireturn block mode',
        prepare     = 'a = {1, 2, 3}',
        opts        = {block = true},
        input       = '1, 2, nil, a',
        expected    = '1, 2, box.NULL, {\n  1,\n  2,\n  3\n}',
        cleanup     = 'a = nil',
    }, {
        name        = 'trailing nils, line mode',
        opts        = {block = false},
        input       = '1, nil, nil, nil',
        expected    = '1, box.NULL, box.NULL, box.NULL',
    }, {
        name        = 'trailing nils, block mode',
        opts        = {block = true},
        input       = '1, nil, nil, nil',
        expected    = '1, box.NULL, box.NULL, box.NULL',
    }, {
        name        = 'empty output',
        input       = '\\set output',
        expected    = '"Specify output format: lua or yaml."',
    }
}

local test = tap.test('console-lua')
test:plan(#cases)

local server, client = start_console()

for _, case in ipairs(cases) do
    test:test(case.name, function(test)
        test:plan(3)

        execute_statements(test, client, totable(case.prepare), 'prepare')

        set_lua_output(client, case.opts)
        execute_and_verify(test, client, case.input, case.expected, 'run')

        execute_statements(test, client, totable(case.cleanup), 'cleanup')
    end)
end

stop_console(server, client)

os.exit(test:check() and 0 or 1)
