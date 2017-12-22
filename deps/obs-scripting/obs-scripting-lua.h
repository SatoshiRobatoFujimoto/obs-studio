/******************************************************************************
    Copyright (C) 2017 by Hugh Bailey <jim@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define SWIG_TYPE_TABLE obslua
#include "swig/swigluarun.h"

#include <util/threading.h>
#include <util/base.h>
#include <util/bmem.h>

#include "obs-scripting-internal.h"
#include "obs-scripting-callback.h"

#define do_log(level, format, ...) \
	blog(level, "[Lua] " format, ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

/* ------------------------------------------------------------ */

struct lua_obs_callback;

struct obs_lua_script {
	obs_script_t base;

	struct dstr dir;
	struct dstr log_chunk;

	pthread_mutex_t mutex;
	lua_State *script;

	struct script_callback *first_callback;

	int tick;
	struct obs_lua_script *next_tick;
	struct obs_lua_script **p_prev_next_tick;

	bool defined_sources;
};

static inline struct obs_lua_script *get_obs_script(lua_State *script)
{
	if (!script)
		return NULL;

	void *ud = NULL;
	lua_getallocf(script, &ud);

	struct obs_lua_script *data = ud;
	return data;
}

static inline struct obs_lua_script *lock_script_(lua_State *script)
{
	struct obs_lua_script *data = get_obs_script(script);
	if (data)
		pthread_mutex_lock(&data->mutex);
	return data;
}

static inline void unlock_script_(struct obs_lua_script *data)
{
	if (data)
		pthread_mutex_unlock(&data->mutex);
}

#define lock_script(script) \
	struct obs_lua_script *ud__ = lock_script_(script)
#define unlock_script() \
	unlock_script_(ud__)

/* ------------------------------------------------ */

struct lua_obs_callback {
	struct script_callback base;

	lua_State *script;
	int reg_idx;
};

static inline struct lua_obs_callback *add_lua_obs_callback_extra(
		lua_State *script,
		int stack_idx,
		size_t extra_size)
{
	struct obs_lua_script *data = get_obs_script(script);
	struct lua_obs_callback *cb = add_script_callback(
			&data->first_callback,
			(obs_script_t *)data,
			sizeof(*cb) + extra_size);

	lua_pushvalue(script, stack_idx);
	cb->reg_idx = luaL_ref(script, LUA_REGISTRYINDEX);
	cb->script = script;
	return cb;
}

static inline struct lua_obs_callback *add_lua_obs_callback(
		lua_State *script, int stack_idx)
{
	return add_lua_obs_callback_extra(script, stack_idx, 0);
}

static inline void *lua_obs_callback_extra_data(struct lua_obs_callback *cb)
{
	return (void*)&cb[1];
}

static inline struct obs_lua_script *lua_obs_callback_script(
		struct lua_obs_callback *cb)
{
	return (struct obs_lua_script *)cb->base.script;
}

static inline struct lua_obs_callback *find_next_lua_obs_callback(
		lua_State *script, struct lua_obs_callback *cb, int stack_idx)
{
	void *ud = NULL;
	lua_getallocf(script, &ud);
	struct obs_lua_script *data = ud;

	cb = cb ? (struct lua_obs_callback *)cb->base.next
		: (struct lua_obs_callback *)data->first_callback;

	while (cb) {
		lua_rawgeti(script, LUA_REGISTRYINDEX, cb->reg_idx);
		bool match = lua_rawequal(script, -1, stack_idx);
		lua_pop(script, 1);

		if (match)
			break;

		cb = (struct lua_obs_callback *)cb->base.next;
	}

	return cb;
}

static inline struct lua_obs_callback *find_lua_obs_callback(
		lua_State *script, int stack_idx)
{
	return find_next_lua_obs_callback(script, NULL, stack_idx);
}

static inline void remove_lua_obs_callback(struct lua_obs_callback *cb)
{
	remove_script_callback(&cb->base);
	luaL_unref(cb->script, LUA_REGISTRYINDEX, cb->reg_idx);
}

static inline void just_free_lua_obs_callback(struct lua_obs_callback *cb)
{
	just_free_script_callback(&cb->base);
}

static inline void free_lua_obs_callback(struct lua_obs_callback *cb)
{
	free_script_callback(&cb->base);
}

/* ------------------------------------------------ */

static int is_ptr(lua_State *script, int idx)
{
	return lua_isuserdata(script, idx) || lua_isnil(script, idx);
}

static int is_table(lua_State *script, int idx)
{
	return lua_istable(script, idx);
}

static int is_function(lua_State *script, int idx)
{
	return lua_isfunction(script, idx);
}

typedef int (*param_cb)(lua_State *script, int idx);

static inline bool verify_args1_(lua_State *script,
		param_cb param1_check,
		const char *func)
{
	if (lua_gettop(script) != 1) {
		warn("Wrong number of parameters for %s", func);
		return false;
	}
	if (!param1_check(script, 1)) {
		warn("Wrong parameter type for parameter %d of %s", 1, func);
		return false;
	}

	return true;
}

#define verify_args1(script, param1_check) \
	verify_args1_(script, param1_check, __FUNCTION__)

static inline bool call_func_(lua_State *script,
		int reg_idx, int args, int rets,
		const char *func, const char *display_name)
{
	if (reg_idx == LUA_REFNIL)
		return false;

	struct obs_lua_script *data = get_obs_script(script);

	lua_rawgeti(script, LUA_REGISTRYINDEX, reg_idx);
	lua_insert(script, -1 - args);

	if (lua_pcall(script, args, rets, 0) != 0) {
		script_warn(&data->base, "Failed to call %s for %s: %s", func,
				display_name,
				lua_tostring(script, -1));
		lua_pop(script, 1);
		return false;
	}

	return true;
}

bool ls_get_libobs_obj_(lua_State * script,
                        const char *type,
                        int         lua_idx,
                        void *      libobs_out,
                        const char *id,
                        const char *func,
                        int         line);
bool ls_push_libobs_obj_(lua_State * script,
                         const char *type,
                         void *      libobs_in,
                         bool        ownership,
                         const char *id,
                         const char *func,
                         int         line);

extern void add_lua_source_functions(lua_State *script);