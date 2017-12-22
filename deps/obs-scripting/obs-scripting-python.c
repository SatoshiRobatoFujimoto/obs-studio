/******************************************************************************
    Copyright (C) 2015 by Andrew Skinner <obs@theandyroid.com>
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

#include "obs-scripting-python.h"
#include "obs-scripting-config.h"
#include <util/base.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>

#include <obs.h>

/* ========================================================================= */

// #define DEBUG_PYTHON_STARTUP

static const char *startup_script = "\n\
import sys\n\
import os\n\
import obspython\n\
class stdout_logger(object):\n\
	def write(self, message):\n\
		obspython.script_log(obspython.LOG_INFO, message)\n\
	def flush(self):\n\
		pass\n\
class stderr_logger(object):\n\
	def write(self, message):\n\
		obspython.script_log(obspython.LOG_ERROR, message)\n\
	def flush(self):\n\
		pass\n\
os.environ['PYTHONUNBUFFERED'] = '1'\n\
sys.stdout = stdout_logger()\n\
sys.stderr = stderr_logger()\n";

#if RUNTIME_LINK
static wchar_t home_path[1024] = {0};
#endif

DARRAY(char*) python_paths;
static bool python_loaded = false;

static pthread_mutex_t tick_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct obs_python_script *first_tick_script = NULL;

static PyObject *py_obspython = NULL;
struct obs_python_script *cur_python_script = NULL;
struct python_obs_callback *cur_python_cb = NULL;

/* -------------------------------------------- */

bool py_to_libobs_(const char *type,
                   PyObject *  py_in,
                   void *      libobs_out,
                   const char *id,
                   const char *func,
                   int         line)
{
	swig_type_info *info = SWIG_TypeQuery(type);
	if (info == NULL) {
		warn("%s:%d: SWIG could not find type: %s%s%s",
		     func,
		     line,
		     id ? id : "",
		     id ? "::" : "",
		     type);
		return false;
	}

	int ret = SWIG_ConvertPtr(py_in, libobs_out, info, 0);
	if (!SWIG_IsOK(ret)) {
		warn("%s:%d: SWIG failed to convert python object to obs "
		     "object: %s%s%s",
		     func,
		     line,
		     id ? id : "",
		     id ? "::" : "",
		     type);
		return false;
	}

	return true;
}

bool libobs_to_py_(const char *type,
                   void *      libobs_in,
                   bool        ownership,
                   PyObject ** py_out,
                   const char *id,
                   const char *func,
                   int         line)
{
	swig_type_info *info = SWIG_TypeQuery(type);
	if (info == NULL) {
		warn("%s:%d: SWIG could not find type: %s%s%s",
		     func,
		     line,
		     id ? id : "",
		     id ? "::" : "",
		     type);
		return false;
	}

	*py_out = SWIG_NewPointerObj(libobs_in, info, (int)ownership);
	if (*py_out == Py_None) {
		warn("%s:%d: SWIG failed to convert obs object to python "
		     "object: %s%s%s",
		     func,
		     line,
		     id ? id : "",
		     id ? "::" : "",
		     type);
		return false;
	}

	return true;
}

#define libobs_to_py(type, obs_obj, ownership, py_obj) \
	libobs_to_py_(#type " *", obs_obj, ownership, py_obj, \
			NULL, __func__, __LINE__)
#define py_to_libobs(type, py_obj, libobs_out) \
	py_to_libobs_(#type " *", py_obj, libobs_out, \
			NULL, __func__, __LINE__)

/* ========================================================================= */

void add_functions_to_py_module(PyObject *module, PyMethodDef *method_list)
{
	PyObject *dict = PyModule_GetDict(module);
	PyObject *name = PyModule_GetNameObject(module);
	if (!dict || !name) {
		return;
	}
	for (PyMethodDef *ml = method_list; ml->ml_name != NULL; ml++) {
		PyObject *func = PyCFunction_NewEx(ml, module, name);
		if (!func) {
			continue;
		}
		PyDict_SetItemString(dict, ml->ml_name, func);
		Py_DECREF(func);
	}
	Py_DECREF(name);
}

/* -------------------------------------------- */

static PyObject *py_get_current_script_path(PyObject *self, PyObject *args)
{
	UNUSED_PARAMETER(args);
	return PyDict_GetItemString(PyModule_GetDict(self),
			"__script_dir__");
}

static bool load_python_script(struct obs_python_script *data)
{
	PyObject *py_file     = NULL;
	PyObject *py_module   = NULL;
	PyObject *py_success  = NULL;
	PyObject *py_tick     = NULL;
	PyObject *py_load     = NULL;
	bool      success     = false;
	int       ret;

	cur_python_script = data;

	if (!data->module) {
		py_file   = PyUnicode_FromString(data->name.array);
		py_module = PyImport_Import(py_file);
	} else {
		py_module = PyImport_ReloadModule(data->module);
	}

	if (!py_module) {
		py_error();
		goto fail;
	}

	Py_XINCREF(py_obspython);
	ret = PyModule_AddObject(py_module, "obspython", py_obspython);
	if (py_error() || ret != 0)
		goto fail;

	ret = PyModule_AddStringConstant(py_module, "__script_dir__",
			data->dir.array);
	if (py_error() || ret != 0)
		goto fail;

	PyObject *py_data = PyCapsule_New(data, NULL, NULL);
	ret = PyModule_AddObject(py_module, "__script_data__", py_data);
	if (py_error() || ret != 0)
		goto fail;

	static PyMethodDef global_funcs[] = {
		{"script_path",
		 py_get_current_script_path,
		 METH_NOARGS,
		 "Gets the script path"},
		{0}
	};

	add_functions_to_py_module(py_module, global_funcs);

	py_tick = PyObject_GetAttrString(py_module, "script_tick");
	if (py_tick) {
		pthread_mutex_lock(&tick_mutex);

		struct obs_python_script *next = first_tick_script;
		data->next_tick = next;
		data->p_prev_next_tick = &first_tick_script;
		if (next) next->p_prev_next_tick = &data->next_tick;
		first_tick_script = data;

		data->tick = py_tick;
		py_tick = NULL;

		pthread_mutex_unlock(&tick_mutex);
	} else {
		PyErr_Clear();
	}

	py_load = PyObject_GetAttrString(py_module, "script_load");
	if (py_load) {
		PyObject *py_ret = PyObject_CallObject(py_load, NULL);
		py_error();
		Py_XDECREF(py_ret);
	} else {
		PyErr_Clear();
	}

	if (data->module)
		Py_XDECREF(data->module);
	data->module = py_module;
	py_module = NULL;

	success = true;

fail:
	Py_XDECREF(py_load);
	Py_XDECREF(py_tick);
	Py_XDECREF(py_success);
	Py_XDECREF(py_file);
	if (!success)
		Py_XDECREF(py_module);
	cur_python_script = NULL;
	return success;
}

static void unload_python_script(struct obs_python_script *data)
{
	PyObject *py_module   = data->module;
	PyObject *py_func     = NULL;
	PyObject *py_ret      = NULL;

	cur_python_script = data;

	py_func = PyObject_GetAttrString(py_module, "script_unload");
	if (PyErr_Occurred() || !py_func) {
		PyErr_Clear();
		goto fail;
	}

	py_ret = PyObject_CallObject(py_func, NULL);
	if (py_error())
		goto fail;

fail:
	Py_XDECREF(py_ret);
	Py_XDECREF(py_func);

	cur_python_script = NULL;
}

static void add_to_python_path(const char *path)
{
	PyObject *py_path_str = NULL;
	PyObject *py_path     = NULL;
	int       ret;

	if (!path || !*path)
		return;

	for (size_t i = 0; i < python_paths.num; i++) {
		const char *python_path = python_paths.array[i];
		if (strcmp(path, python_path) == 0)
			return;
	}

	ret = PyRun_SimpleString("import sys");
	if (py_error() || ret != 0)
		goto fail;

	/* borrowed reference here */
	py_path = PySys_GetObject("path");
	if (py_error() || !py_path)
		goto fail;

	py_path_str = PyUnicode_FromString(path);
	ret = PyList_Append(py_path, py_path_str);
	if (py_error() || ret != 0)
		goto fail;

	char *new_path = bstrdup(path);
	da_push_back(python_paths, &new_path);

fail:
	Py_XDECREF(py_path_str);
}

/* -------------------------------------------- */

struct python_obs_timer {
	struct python_obs_timer *next;
	struct python_obs_timer **p_prev_next;

	uint64_t last_ts;
	uint64_t interval;
};

static pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct python_obs_timer *first_timer = NULL;

static inline void python_obs_timer_init(struct python_obs_timer *timer)
{
	pthread_mutex_lock(&timer_mutex);

	struct python_obs_timer *next = first_timer;
	timer->next = next;
	timer->p_prev_next = &first_timer;
	if (next) next->p_prev_next = &timer->next;
	first_timer = timer;

	pthread_mutex_unlock(&timer_mutex);
}

static inline void python_obs_timer_remove(struct python_obs_timer *timer)
{
	struct python_obs_timer *next = timer->next;
	if (next) next->p_prev_next = timer->p_prev_next;
	*timer->p_prev_next = timer->next;
}

static inline struct python_obs_callback *python_obs_timer_cb(
		struct python_obs_timer *timer)
{
	return &((struct python_obs_callback *)timer)[-1];
}

static PyObject *timer_remove(PyObject *self, PyObject *args)
{
	struct obs_python_script *script = cur_python_script;
	PyObject *py_cb;

	UNUSED_PARAMETER(self);

	if (!PyArg_ParseTuple(args, "O:" __FUNCTION__, &py_cb))
		return python_none();

	debug("timer_remove called");

	struct python_obs_callback *cb = find_python_obs_callback(script, py_cb);
	if (cb) remove_python_obs_callback(cb);
	return python_none();
}

static void timer_call(struct script_callback *p_cb)
{
	struct python_obs_callback *cb = (struct python_obs_callback *)p_cb;
	struct obs_python_script *script = python_obs_callback_script(cb);

	if (p_cb->removed)
		return;

	lock_python();
	cur_python_script = script;
	cur_python_cb = cb;

	PyObject *py_ret = PyObject_CallObject(cb->func, NULL);
	Py_XDECREF(py_ret);

	cur_python_cb = NULL;
	cur_python_script = NULL;
	unlock_python();
}

static void defer_timer_init(struct script_callback *script_cb)
{
	struct python_obs_callback *cb = (struct python_obs_callback *)script_cb;
	struct python_obs_timer *timer = python_obs_callback_extra_data(cb);
	python_obs_timer_init(timer);
}

static PyObject *timer_add(PyObject *self, PyObject *args)
{
	struct obs_python_script *script = cur_python_script;
	PyObject *py_cb;
	int ms;

	UNUSED_PARAMETER(self);

	if (!PyArg_ParseTuple(args, "Oi:" __FUNCTION__, &py_cb, &ms))
		return python_none();

	debug("timer_add called");

	struct python_obs_callback *cb = add_python_obs_callback_extra(
			script, py_cb, sizeof(struct python_obs_timer));
	struct python_obs_timer *timer = python_obs_callback_extra_data(cb);

	timer->interval = (uint64_t)ms * 1000000ULL;
	timer->last_ts = obs_get_video_frame_time();

	defer_call_post(defer_timer_init, cb);
	return python_none();
}

/* -------------------------------------------- */

static void obs_python_tick_callback(void *priv, float seconds)
{
	struct python_obs_callback *cb = priv;

	if (cb->base.removed) {
		obs_remove_tick_callback(obs_python_tick_callback, cb);
		return;
	}

	lock_python();

	struct python_obs_callback *last_cb = cur_python_cb;
	cur_python_script = (struct obs_python_script *)cb->base.script;
	cur_python_cb = cb;

	PyObject *args = Py_BuildValue("(f)", seconds);

	PyObject *py_ret = PyObject_CallObject(cb->func, args);
	Py_XDECREF(py_ret);
	Py_XDECREF(args);

	cur_python_script = NULL;
	cur_python_cb = last_cb;

	unlock_python();
}

static PyObject *obs_python_remove_tick_callback(PyObject *self, PyObject *args)
{
	struct obs_python_script *script = cur_python_script;
	PyObject *py_cb = NULL;

	if (!script) {
		PyErr_SetString(PyExc_RuntimeError,
				"No active script, report this to Jim");
		return NULL;
	}

	UNUSED_PARAMETER(self);

	if (!PyArg_ParseTuple(args, "O:" __FUNCTION__, &py_cb))
		return python_none();
	if (!py_cb || !PyFunction_Check(py_cb))
		return python_none();

	struct python_obs_callback *cb = find_python_obs_callback(script, py_cb);
	if (cb) remove_python_obs_callback(cb);
	return python_none();
}

static PyObject *obs_python_add_tick_callback(PyObject *self, PyObject *args)
{
	struct obs_python_script *script = cur_python_script;
	PyObject *py_cb = NULL;

	if (!script) {
		PyErr_SetString(PyExc_RuntimeError,
				"No active script, report this to Jim");
		return NULL;
	}

	UNUSED_PARAMETER(self);

	if (!PyArg_ParseTuple(args, "O:" __FUNCTION__, &py_cb))
		return python_none();
	if (!py_cb || !PyFunction_Check(py_cb))
		return python_none();

	struct python_obs_callback *cb = add_python_obs_callback(script, py_cb);
	obs_add_tick_callback(obs_python_tick_callback, cb);
	return python_none();
}

/* -------------------------------------------- */

static void calldata_signal_callback(void *priv, calldata_t *cd)
{
	struct python_obs_callback *cb = priv;

	if (cb->base.removed) {
		signal_handler_remove_current();
		return;
	}

	lock_python();

	PyObject *py_cd;

	if (libobs_to_py(calldata_t, cd, false, &py_cd)) {
		PyObject *args = Py_BuildValue("(O)", py_cd);

		struct python_obs_callback *last_cb = cur_python_cb;
		cur_python_cb = cb;
		cur_python_script =
			(struct obs_python_script *)cb->base.script;

		PyObject *py_ret = PyObject_CallObject(cb->func, args);
		Py_XDECREF(py_ret);
		py_error();

		cur_python_script = NULL;
		cur_python_cb = last_cb;

		Py_XDECREF(args);
		Py_XDECREF(py_cd);
	}

	unlock_python();
}

static PyObject *obs_python_signal_handler_disconnect(
		PyObject *self, PyObject *args)
{
	struct obs_python_script *script = cur_python_script;
	PyObject *py_sh = NULL;
	PyObject *py_cb = NULL;
	const char *signal;

	if (!script) {
		PyErr_SetString(PyExc_RuntimeError,
				"No active script, report this to Jim");
		return NULL;
	}

	UNUSED_PARAMETER(self);

	signal_handler_t *handler;

	if (!PyArg_ParseTuple(args, "OsO:" __FUNCTION__, &py_sh, &signal,
				&py_cb))
		return python_none();

	if (!py_to_libobs(signal_handler_t, py_sh, &handler))
		return python_none();
	if (!py_cb || !PyFunction_Check(py_cb))
		return python_none();

	struct python_obs_callback *cb = find_python_obs_callback(script, py_cb);
	while (cb) {
		signal_handler_t *cb_handler =
			calldata_ptr(&cb->base.extra, "handler");
		const char *cb_signal =
			calldata_string(&cb->base.extra, "signal");

		if (cb_signal &&
		    strcmp(signal, cb_signal) != 0 &&
		    handler == cb_handler)
			break;

		cb = find_next_python_obs_callback(script, cb, py_cb);
	}

	if (cb) remove_python_obs_callback(cb);
	return python_none();
}

static PyObject *obs_python_signal_handler_connect(
		PyObject *self, PyObject *args)
{
	struct obs_python_script *script = cur_python_script;
	PyObject *py_sh = NULL;
	PyObject *py_cb = NULL;
	const char *signal;

	if (!script) {
		PyErr_SetString(PyExc_RuntimeError,
				"No active script, report this to Jim");
		return NULL;
	}

	UNUSED_PARAMETER(self);

	signal_handler_t *handler;

	if (!PyArg_ParseTuple(args, "OsO:" __FUNCTION__, &py_sh, &signal,
				&py_cb))
		return python_none();

	if (!py_to_libobs(signal_handler_t, py_sh, &handler))
		return python_none();
	if (!py_cb || !PyFunction_Check(py_cb))
		return python_none();

	struct python_obs_callback *cb = add_python_obs_callback(script, py_cb);
	calldata_set_ptr(&cb->base.extra, "handler", handler);
	calldata_set_string(&cb->base.extra, "signal", signal);
	signal_handler_connect(handler, signal, calldata_signal_callback, cb);
	return python_none();
}

/* -------------------------------------------- */

static void calldata_signal_callback_global(void *priv, const char *signal,
		calldata_t *cd)
{
	struct python_obs_callback *cb = priv;

	if (cb->base.removed) {
		signal_handler_remove_current();
		return;
	}

	lock_python();

	PyObject *py_cd;

	if (libobs_to_py(calldata_t, cd, false, &py_cd)) {
		PyObject *args = Py_BuildValue("(sO)", signal, py_cd);

		struct python_obs_callback *last_cb = cur_python_cb;
		cur_python_script =
			(struct obs_python_script *)cb->base.script;
		cur_python_cb = cb;

		PyObject *py_ret = PyObject_CallObject(cb->func, args);
		Py_XDECREF(py_ret);
		py_error();

		cur_python_script = NULL;
		cur_python_cb = last_cb;

		Py_XDECREF(args);
		Py_XDECREF(py_cd);
	}

	unlock_python();
}

static PyObject *obs_python_signal_handler_disconnect_global(
		PyObject *self, PyObject *args)
{
	struct obs_python_script *script = cur_python_script;
	PyObject *py_sh = NULL;
	PyObject *py_cb = NULL;

	if (!script) {
		PyErr_SetString(PyExc_RuntimeError,
				"No active script, report this to Jim");
		return NULL;
	}

	UNUSED_PARAMETER(self);

	signal_handler_t *handler;

	if (!PyArg_ParseTuple(args, "OO:" __FUNCTION__, &py_sh, &py_cb))
		return python_none();

	if (!py_to_libobs(signal_handler_t, py_sh, &handler))
		return python_none();
	if (!py_cb || !PyFunction_Check(py_cb))
		return python_none();

	struct python_obs_callback *cb = find_python_obs_callback(script, py_cb);
	while (cb) {
		signal_handler_t *cb_handler =
			calldata_ptr(&cb->base.extra, "handler");

		if (handler == cb_handler)
			break;

		cb = find_next_python_obs_callback(script, cb, py_cb);
	}

	if (cb) remove_python_obs_callback(cb);
	return python_none();
}

static PyObject *obs_python_signal_handler_connect_global(
		PyObject *self, PyObject *args)
{
	struct obs_python_script *script = cur_python_script;
	PyObject *py_sh = NULL;
	PyObject *py_cb = NULL;

	if (!script) {
		PyErr_SetString(PyExc_RuntimeError,
				"No active script, report this to Jim");
		return NULL;
	}

	UNUSED_PARAMETER(self);

	signal_handler_t *handler;

	if (!PyArg_ParseTuple(args, "OO:" __FUNCTION__, &py_sh, &py_cb))
		return python_none();

	if (!py_to_libobs(signal_handler_t, py_sh, &handler))
		return python_none();
	if (!py_cb || !PyFunction_Check(py_cb))
		return python_none();

	struct python_obs_callback *cb = add_python_obs_callback(script, py_cb);
	calldata_set_ptr(&cb->base.extra, "handler", handler);
	signal_handler_connect_global(handler,
			calldata_signal_callback_global, cb);
	return python_none();
}

/* -------------------------------------------- */

static PyObject *remove_current_callback(PyObject *self, PyObject *args)
{
	UNUSED_PARAMETER(self);
	UNUSED_PARAMETER(args);

	if (cur_python_cb)
		remove_python_obs_callback(cur_python_cb);
	return python_none();
}

/* -------------------------------------------- */

static PyObject *calldata_source(PyObject *self, PyObject *args)
{
	PyObject *py_ret = NULL;
	PyObject *py_cd  = NULL;

	calldata_t *cd;
	const char *name;

	UNUSED_PARAMETER(self);

	if (!PyArg_ParseTuple(args, "Os:calldata_source", &py_cd, &name))
		goto fail;
	if (!py_to_libobs(calldata_t, py_cd, &cd))
		goto fail;

	obs_source_t *source = calldata_ptr(cd, name);
	libobs_to_py(obs_source_t, source, false, &py_ret);

fail:
	return py_ret;
}

/* -------------------------------------------- */

struct dstr cur_py_log_chunk = {0};

static PyObject *py_script_log(PyObject *self, PyObject *args)
{
	static bool calling_self = false;
	int log_level;
	const char *msg;

	UNUSED_PARAMETER(self);

	if (calling_self)
		return python_none();
	calling_self = true;

	/* ------------------- */

	if (!PyArg_ParseTuple(args, "is:calldata_source", &log_level, &msg))
		goto fail;
	if (!msg || !*msg)
		goto fail;

	dstr_cat(&cur_py_log_chunk, msg);

	const char *start = cur_py_log_chunk.array;
	char *endl = strchr(start, '\n');

	while (endl) {
		*endl = 0;
		script_log(&cur_python_script->base, log_level, "%s", start);
		*endl = '\n';

		start = endl + 1;
		endl = strchr(start, '\n');
	}

	if (start) {
		size_t len = strlen(start);
		if (len) memmove(cur_py_log_chunk.array, start, len);
		dstr_resize(&cur_py_log_chunk, len);
	}

	/* ------------------- */

fail:
	calling_self = false;
	return python_none();
}

/* -------------------------------------------- */

static void add_hook_functions(PyObject *module)
{
	static PyMethodDef funcs[] = {
#define DEF_FUNC(n, c) {n, c, METH_VARARGS, NULL}

		DEF_FUNC("script_log", py_script_log),
		DEF_FUNC("timer_remove", timer_remove),
		DEF_FUNC("timer_add", timer_add),
		DEF_FUNC("obs_remove_tick_callback",
		         obs_python_remove_tick_callback),
		DEF_FUNC("obs_add_tick_callback",
		         obs_python_add_tick_callback),
		DEF_FUNC("signal_handler_disconnect",
		         obs_python_signal_handler_disconnect),
		DEF_FUNC("signal_handler_connect",
		         obs_python_signal_handler_connect),
		DEF_FUNC("signal_handler_disconnect_global",
		         obs_python_signal_handler_disconnect_global),
		DEF_FUNC("signal_handler_connect_global",
		         obs_python_signal_handler_connect_global),
		DEF_FUNC("remove_current_callback",
		         remove_current_callback),
		DEF_FUNC("calldata_source", calldata_source),

#undef DEF_FUNC
		{0}
	};

	add_functions_to_py_module(module, funcs);
}

/* -------------------------------------------- */

bool obs_python_script_load(obs_script_t *s)
{
	struct obs_python_script *data = (struct obs_python_script *)s;
	if (!data->base.loaded) {
		lock_python();
		data->base.loaded = load_python_script(data);
		unlock_python();
	}

	return data->base.loaded;
}

obs_script_t *obs_python_script_create(const char *path)
{
	struct obs_python_script *data = bzalloc(sizeof(*data));

	data->base.type = OBS_SCRIPT_LANG_PYTHON;

	dstr_copy(&data->base.path, path);
	dstr_replace(&data->base.path, "\\", "/");
	path = data->base.path.array;

	const char *slash = path && *path ? strrchr(path, '/') : NULL;
	if (slash) {
		slash++;
		dstr_copy(&data->base.file, slash);
		dstr_left(&data->dir, &data->base.path, slash - path);
	} else {
		dstr_copy(&data->base.file, path);
	}

	path = data->base.file.array;
	dstr_copy_dstr(&data->name, &data->base.file);

	const char *ext = strstr(path, ".py");
	if (ext)
		dstr_resize(&data->name, ext - path);

	lock_python();
	add_to_python_path(data->dir.array);
	data->base.loaded = load_python_script(data);
	unlock_python();

	return (obs_script_t *)data;
}

void obs_python_script_unload(obs_script_t *s)
{
	struct obs_python_script *data = (struct obs_python_script *)s;

	if (!s->loaded)
		return;

	/* ---------------------------- */
	/* unhook tick function         */

	if (data->p_prev_next_tick) {
		pthread_mutex_lock(&tick_mutex);

		struct obs_python_script *next = data->next_tick;
		if (next) next->p_prev_next_tick = data->p_prev_next_tick;
		*data->p_prev_next_tick = next;

		pthread_mutex_unlock(&tick_mutex);

		data->p_prev_next_tick = NULL;
		data->next_tick = NULL;
	}

	lock_python();

	Py_XDECREF(data->tick);
	data->tick = NULL;

	/* ---------------------------- */
	/* remove all callbacks         */

	struct script_callback *cb = data->first_callback;
	while (cb) {
		struct script_callback *next = cb->next;
		remove_script_callback(cb);
		cb = next;
	}

	/* ---------------------------- */
	/* unload                       */

	unload_python_script(data);
	unlock_python();

	s->loaded = false;
}

void obs_python_script_destroy(obs_script_t *s)
{
	struct obs_python_script *data = (struct obs_python_script *)s;

	if (data) {
		lock_python();
		Py_XDECREF(data->module);
		unlock_python();

		dstr_free(&data->base.path);
		dstr_free(&data->base.file);
		dstr_free(&data->dir);
		dstr_free(&data->name);
		bfree(data);
	}
}

/* -------------------------------------------- */

static void python_tick(void *param, float seconds)
{
	struct obs_python_script *data;
	bool valid;
	uint64_t ts = obs_get_video_frame_time();

	pthread_mutex_lock(&tick_mutex);
	valid = !!first_tick_script;
	pthread_mutex_unlock(&tick_mutex);

	/* --------------------------------- */
	/* process script_tick calls         */

	if (valid) {
		lock_python();

		PyObject *args = Py_BuildValue("(f)", seconds);

		pthread_mutex_lock(&tick_mutex);
		data = first_tick_script;
		while (data) {
			cur_python_script = data;

			PyObject *py_ret = PyObject_CallObject(data->tick, args);
			Py_XDECREF(py_ret);
			py_error();

			data = data->next_tick;
		}

		cur_python_script = NULL;

		pthread_mutex_unlock(&tick_mutex);

		Py_XDECREF(args);

		unlock_python();
	}

	/* --------------------------------- */
	/* process timers                    */

	pthread_mutex_lock(&timer_mutex);
	struct python_obs_timer *timer = first_timer;
	while (timer) {
		struct python_obs_timer *next = timer->next;
		struct python_obs_callback *cb = python_obs_timer_cb(timer);

		if (cb->base.removed) {
			python_obs_timer_remove(timer);
		} else {
			uint64_t elapsed = ts - timer->last_ts;

			if (elapsed >= timer->interval) {
				lock_python();
				timer_call(&cb->base);
				unlock_python();

				timer->last_ts += timer->interval;
			}
		}

		timer = next;
	}
	pthread_mutex_unlock(&timer_mutex);

	UNUSED_PARAMETER(param);
}

/* -------------------------------------------- */

void obs_python_unload(void);

bool obs_scripting_python_runtime_linked(void)
{
	return (bool)RUNTIME_LINK;
}

bool obs_scripting_python_loaded(void)
{
	return python_loaded;
}

void obs_python_load(void)
{
	da_init(python_paths);

	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

	pthread_mutex_init(&tick_mutex, NULL);
	pthread_mutex_init(&timer_mutex, &attr);

}

extern void add_python_frontend_funcs(PyObject *module);

bool obs_scripting_load_python(const char *python_path)
{
	if (python_loaded)
		return true;

	/* Use external python on windows and mac */
#if RUNTIME_LINK
# if 0
	struct dstr old_path  = {0};
	struct dstr new_path  = {0};
# endif

	if (!import_python(python_path))
		return false;

	if (python_path && *python_path) {
		os_utf8_to_wcs(python_path, 0, home_path, 1024);
		Py_SetPythonHome(home_path);
# if 0
		dstr_copy(&old_path, getenv("PATH"));
		_putenv("PYTHONPATH=");
		_putenv("PATH=");
# endif
	}
#else
	UNUSED_VARIABLE(python_path);
#endif

	Py_Initialize();

#if 0
# ifdef _DEBUG
	if (pythondir && *pythondir) {
		dstr_printf(&new_path, "PATH=%s", old_path.array);
		_putenv(new_path.array);
	}
# endif

	bfree(pythondir);
	dstr_free(&new_path);
	dstr_free(&old_path);
#endif

	PyEval_InitThreads();

	/* ---------------------------------------------- */
	/* Must set arguments for guis to work            */

	wchar_t *argv[] = {L"", NULL};
	int      argc   = sizeof(argv) / sizeof(wchar_t*) - 1;

	PySys_SetArgv(argc, argv);

#ifdef DEBUG_PYTHON_STARTUP
	/* ---------------------------------------------- */
	/* Debug logging to file if startup is failing    */

	PyRun_SimpleString("import os");
	PyRun_SimpleString("import sys");
	PyRun_SimpleString("os.environ['PYTHONUNBUFFERED'] = '1'");
	PyRun_SimpleString("sys.stdout = open('./stdOut.txt','w',1)");
	PyRun_SimpleString("sys.stderr = open('./stdErr.txt','w',1)");
	PyRun_SimpleString("print(sys.version)");
#endif

	/* ---------------------------------------------- */
	/* Load main interface module                     */

	py_obspython = PyImport_ImportModule("obspython");
	bool success = !py_error();
	if (!success) {
		warn("Error importing obspython.py', unloading obs-python");
		goto out;
	}

	python_loaded = PyRun_SimpleString(startup_script) == 0;
	py_error();

	add_hook_functions(py_obspython);
	py_error();

	add_python_frontend_funcs(py_obspython);
	py_error();

out:
	/* ---------------------------------------------- */
	/* Free data                                      */

	PyEval_ReleaseThread(PyGILState_GetThisThreadState());

	if (!success) {
		warn("Failed to load python plugin");
		obs_python_unload();
	}

	if (python_loaded)
		obs_add_tick_callback(python_tick, NULL);

	return python_loaded;
}

void obs_python_unload(void)
{
	if (Py_IsInitialized()) {
		PyGILState_Ensure();

		Py_XDECREF(py_obspython);
		Py_Finalize();
	}

	/* ---------------------- */

	obs_remove_tick_callback(python_tick, NULL);

	for (size_t i = 0; i < python_paths.num; i++)
		bfree(python_paths.array[i]);
	da_free(python_paths);

	pthread_mutex_destroy(&tick_mutex);
	pthread_mutex_destroy(&timer_mutex);
	dstr_free(&cur_py_log_chunk);
}