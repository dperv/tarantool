test_run = require('test_run').new()
net_box = require('net.box')
urilib = require('uri')
msgpack = require('msgpack')

IPROTO_REQUEST_TYPE   = 0x00
IPROTO_INSERT         = 0x02
IPROTO_SYNC           = 0x01
IPROTO_SPACE_ID       = 0x10
IPROTO_TUPLE          = 0x21
IPROTO_ERROR          = 0x31
IPROTO_ERROR_STACK    = 0x51
IPROTO_ERROR_CODE     = 0x01
IPROTO_ERROR_MESSAGE  = 0x02
IPROTO_OK             = 0x00
IPROTO_SCHEMA_VERSION = 0x05
IPROTO_STATUS_KEY     = 0x00

-- gh-1148: test capabilities of stacked diagnostics bypassing net.box.
--
test_run:cmd("setopt delimiter ';'")
lua_func = [[function(tuple) local json = require('json') return json.encode(tuple) end]]
test_run:cmd("setopt delimiter ''");

box.schema.func.create('f1', {body = lua_func, is_deterministic = true, is_sandboxed = true})
s = box.schema.space.create('s')
pk = s:create_index('pk')
idx = s:create_index('idx', {func = box.func.f1.id, parts = {{1, 'string'}}})

box.schema.user.grant('guest', 'read, write, execute', 'universe')

next_request_id = 16
header = msgpack.encode({ \
    [IPROTO_REQUEST_TYPE] = IPROTO_INSERT, \
    [IPROTO_SYNC]         = next_request_id, \
})

body = msgpack.encode({ \
    [IPROTO_SPACE_ID] = s.id, \
    [IPROTO_TUPLE]    = box.tuple.new({1}) \
})

uri = urilib.parse(box.cfg.listen)
sock = net_box.establish_connection(uri.host, uri.service)

-- Send request.
--
size = msgpack.encode(header:len() + body:len())
sock:write(size .. header .. body)
-- Read responce.
--
size = msgpack.decode(sock:read(5))
header_body = sock:read(size)
header, header_len = msgpack.decode(header_body)
body = msgpack.decode(header_body:sub(header_len))
sock:close()

-- Both keys (obsolete and stack ones) are present in response.
--
assert(body[IPROTO_ERROR_STACK] ~= nil)
assert(body[IPROTO_ERROR] ~= nil)

err = body[IPROTO_ERROR_STACK][1]
assert(err[IPROTO_ERROR_MESSAGE] == body[IPROTO_ERROR])
err = body[IPROTO_ERROR_STACK][2]
assert(err[IPROTO_ERROR_CODE] ~= nil)
assert(type(err[IPROTO_ERROR_CODE]) == 'number')
assert(err[IPROTO_ERROR_MESSAGE] ~= nil)
assert(type(err[IPROTO_ERROR_MESSAGE]) == 'string')

box.schema.user.revoke('guest', 'read,write,execute', 'universe')
s:drop()
box.func.f1:drop()
