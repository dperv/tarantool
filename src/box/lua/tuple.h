#ifndef INCLUDES_TARANTOOL_MOD_BOX_LUA_TUPLE_H
#define INCLUDES_TARANTOOL_MOD_BOX_LUA_TUPLE_H
/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <box/tuple.h>

struct lua_State;
struct txn;
struct tuple;

/**
 * Push tuple on lua stack
 */
void
lbox_pushtuple(struct lua_State *L, struct tuple *tuple);

static inline int
lbox_pushtupleornil(lua_State *L, box_tuple_t *tuple)
{
	if (tuple == NULL)
		return 0;
	lbox_pushtuple(L, tuple);
	return 1;
}

struct tuple *lua_istuple(struct lua_State *L, int narg);

void
luamp_encode_tuple(struct lua_State *L, struct luaL_serializer *cfg,
		  struct mpstream *stream, int index);

char *
lbox_encode_tuple_on_gc(lua_State *L, int idx, size_t *p_len);

void
box_lua_tuple_init(struct lua_State *L);

#if defined(__cplusplus)
extern "C" {
#endif

struct tuple *
boxffi_tuple_update(struct tuple *tuple, const char *expr, const char *expr_end);

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* INCLUDES_TARANTOOL_MOD_BOX_LUA_TUPLE_H */
