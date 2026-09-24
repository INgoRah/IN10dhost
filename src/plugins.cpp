#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <iostream>
#include <filesystem>
#include <unistd.h>
#include <limits.h>

// plugin loader
#include <dlfcn.h> // Unix dynamic loading
#include "main.h"
#include "fs.h"
#include "ow_devices.h"
#include "plugins.h"

namespace fs = std::filesystem;

extern OwDevices ow;

// Called from the Plugins constructor, so this must not touch any other
// global object: cross-translation-unit static initialization order is
// unspecified, and a global Plugins instance could be constructed before
// e.g. the global logger is (Coverity CID 651404, GLOBAL_INIT_ORDER).
// The resolved exec_path still gets logged safely, every time a plugin is
// actually loaded, via plugin_init()'s own logging below.
int Plugins::init()
{
	char buffer[PATH_MAX];
	ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (length == -1)
		return -1;
	buffer[length] = '\0';
	exec_path = std::filesystem::path(buffer).parent_path();

	return 0;
}

std::filesystem::path Plugins::find_lib(const string& name) const
{
	std::filesystem::path f;

	f = exec_path / std::filesystem::path(std::string("lib") + name + ".so");
	if (fs::exists(f))
		return f;
	f = "/opt/lib/in10dfs" / std::filesystem::path(std::string("lib") + name + ".so");
	if (fs::exists(f))
		return f;

	return std::filesystem::path();
}

size_t Plugins::count()
{
	std::lock_guard<std::recursive_mutex> lock(mtx);

	return plugins.size();
}

std::vector<string> Plugins::names()
{
	std::lock_guard<std::recursive_mutex> lock(mtx);
	std::vector<string> list;

	for (auto plugin_ptr : plugins)
		list.push_back(plugin_ptr->name);

	return list;
}

json Plugins::read_config(Plugin* plugin_ptr)
{
	try {
		return plugin_ptr->config_get();
	}
	catch (const std::exception& e) {
		logger.error(std::format("plugin {} config_get failed: {}",
			plugin_ptr->name, e.what()));
	}
	catch (...) {
		logger.error(std::format("plugin {} config_get failed", plugin_ptr->name));
	}

	return json::object();
}

void Plugins::apply_config(Plugin* plugin_ptr, const json& cfg)
{
	try {
		plugin_ptr->config_set(cfg);
	}
	catch (const std::exception& e) {
		logger.error(std::format("plugin {} config_set failed: {}",
			plugin_ptr->name, e.what()));
	}
	catch (...) {
		logger.error(std::format("plugin {} config_set failed", plugin_ptr->name));
	}
}

json Plugins::config_of(const string& name)
{
	std::lock_guard<std::recursive_mutex> lock(mtx);

	for (auto plugin_ptr : plugins)
		if (plugin_ptr->name == name)
			return read_config(plugin_ptr);

	return json();
}

/* Removes a temporary library copy, if there is one. Used on the load
   failure paths so a rejected plugin does not leave a file in /tmp. */
static void drop_tmp(const std::filesystem::path& p)
{
	std::error_code ec;

	if (!p.empty())
		fs::remove(p, ec);
}

/* Loads lib<name>.so and registers the plugin instance.
 *
 * The library is always dlopen()ed from a private copy, never in place.
 * That serves two purposes, and both are needed:
 *
 *  - reload: dlopen() keys its cache on the path, so re-opening the same
 *    path can hand back the code that is still mapped instead of what is
 *    now on disk. A unique path forces a fresh mapping.
 *  - a live mapping must not be rewritten underneath the process.
 *    Deploying (build.sh scp's the .so over the installed one) truncates
 *    and rewrites the file in place, and any page the loader has not
 *    faulted in yet then comes from the new file. Loading in place was
 *    tried and crashes inside dlsym() once the original is replaced.
 *
 * Downside: this needs /tmp to be writable and executable. */
Plugin* Plugins::plugin_init(string name)
{
	std::lock_guard<std::recursive_mutex> lock(mtx);
	std::filesystem::path f;
	Plugin* instance;
	void* handle;
	/* the temporary copy, if one was made: that is the only file this
	   owns and the only one unload() may delete */
	std::filesystem::path tmp_path;

	f = find_lib(name);
	if (f.empty())
		return nullptr;
	/* remember what we loaded so reload() can tell whether the library
	   on disk has been replaced since */
	hashes[name] = file_hash(f);

	try {
		std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::high_resolution_clock::now().time_since_epoch()).count());

		tmp_path = "/tmp/lib" + name + "." + timestamp + ".so";
		logger.info(std::format("trying to load plugin {} {} -> {} ...",
			name,
			f.string(),
			tmp_path.string()));
		// Duplicate the newly compiled binary file to the shadow path
		fs::copy_file(f, tmp_path, fs::copy_options::overwrite_existing);

		handle = dlopen(tmp_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
		if (!handle) {
			//std::cerr << "Cannot load library: " << dlerror() << std::endl;
			logger.error(std::format("Cannot load library {}: {}", name, dlerror()));
			drop_tmp(tmp_path);
			return nullptr;
		}
		// Load the symbols (the factory functions)
		create_t create_plugin = (create_t) dlsym(handle, "create_plugin");
		destroy_t destroy_plugin = (destroy_t) dlsym(handle, "destroy_plugin");

		if (!create_plugin || !destroy_plugin) {
			logger.error(std::format("Cannot load symbols for plugin {}: {}", name, dlerror()));
			dlclose(handle);
			drop_tmp(tmp_path);
			return nullptr;
		}
		instance = create_plugin();
		instance->name = name;
		instance->handle = handle;
		instance->lib_path = tmp_path;

		instance->init(&logger, static_cast<IDevices*>(&ow));
	}
	catch (const std::exception& e) {
		logger.error(std::format("Failed to load plugin {}: {}", name, e.what()));
		drop_tmp(tmp_path);
		return nullptr;
	}
	logger.info(std::format("loaded plugin {}...", instance->name));
	plugins.push_back(instance);
	return instance;
}

/* Tears one plugin down. The caller owns removing it from the plugins
   vector; this only releases the instance, the library and the shadow
   copy. exit() runs inside the plugin, so it is guarded like action(). */
void Plugins::unload(Plugin* plugin_ptr)
{
	// remember the lib path before destroying the plugin instance
	std::filesystem::path lib_path = plugin_ptr->lib_path;

	logger.info(std::format("Cleaning up plugin {}...", plugin_ptr->name));
	try {
		plugin_ptr->exit();
	}
	catch (const std::exception& e) {
		logger.error(std::format("plugin {} exit failed: {}", plugin_ptr->name, e.what()));
	}
	catch (...) {
		logger.error(std::format("plugin {} exit failed", plugin_ptr->name));
	}
	if (plugin_ptr->handle) {
		void* handle = plugin_ptr->handle;
		destroy_t destroy_plugin = (destroy_t) dlsym(handle, "destroy_plugin");
		destroy_plugin(plugin_ptr);
		logger.info(std::format("destroying plugin..."));
		dlclose(handle);
	}
	logger.info("destroyed plugin");
	try {
		if (fs::exists(lib_path))
			fs::remove(lib_path);
		logger.info("deleted tmp file");
	} catch (const fs::filesystem_error& e) {
		std::cerr << "Failed to clean up old temp files: " << e.what() << std::endl;
	}
}

int Plugins::cleanup()
{
	std::lock_guard<std::recursive_mutex> lock(mtx);

	for (auto plugin_ptr : plugins)
		unload(plugin_ptr);
	plugins.clear();
	hashes.clear();

	return 0;
}

/* Loads a plugin that is not loaded yet. Returns 0 on success, -1 when
   the library is missing or failed to load, 1 when it was already
   there (nothing to do). */
int Plugins::add(string name)
{
	std::lock_guard<std::recursive_mutex> lock(mtx);

	for (auto plugin_ptr : plugins)
		if (plugin_ptr->name == name)
			return 1;

	return plugin_init(name) ? 0 : -1;
}

/* Unloads a single plugin by name. Returns 0 when it was unloaded, -1
   when no such plugin is loaded. */
int Plugins::remove(string name)
{
	std::lock_guard<std::recursive_mutex> lock(mtx);

	for (auto it = plugins.begin(); it != plugins.end(); ++it) {
		if ((*it)->name != name)
			continue;
		unload(*it);
		plugins.erase(it);
		hashes.erase(name);
		return 0;
	}

	return -1;
}

/* Reloads plugins whose library on disk differs from the one that was
   loaded, carrying their config across. Plugins that did not change
   just get their config re-applied, which is what makes the scripting
   plugin pick up an edited script. Returns the number reloaded. */
int Plugins::reload(const string& only)
{
	std::lock_guard<std::recursive_mutex> lock(mtx);
	std::vector<std::pair<string, json>> changed;
	int cnt = 0;

	for (auto plugin_ptr : plugins) {
		const string& name = plugin_ptr->name;

		if (!only.empty() && name != only)
			continue;
		uint64_t now = file_hash(find_lib(name));

		if (now != 0 && now != hashes[name]) {
			logger.info(std::format("plugin {} changed on disk, reloading", name));
			// keep its config so the reloaded instance starts up the same
			changed.push_back({ name, read_config(plugin_ptr) });
			continue;
		}
		/* unchanged library: hand the config back so a plugin that
		   reads external files (a script, a table) can re-read them */
		apply_config(plugin_ptr, read_config(plugin_ptr));
	}

	for (const auto& [name, cfg] : changed) {
		// cannot fail: the name came from the plugins vector above and
		// the lock has been held ever since, so it is still in there
		remove(name);
		Plugin* p = plugin_init(name);
		if (p == nullptr) {
			logger.error(std::format("plugin {} failed to reload", name));
			continue;
		}
		apply_config(p, cfg);
		cnt++;
	}

	return cnt;
}

int Plugins::load(json j)
{
	int i = 0;
	bool skip;
	for (auto& [pluginName, pluginConfig] : j.items()) {
		skip = false;
		for (auto plugin_ptr : plugins) {
			if (plugin_ptr->name == pluginName) {
				logger.debug(std::format("plugin {} already loaded", pluginName));
				skip = true;
				break;
			}
		}
		if (skip)
			continue;
		logger.debug(std::format("loading #{} plugin {}",i++, pluginName));
		auto p = plugin_init(pluginName);
		if (p)
			apply_config(p, pluginConfig);
	}
	logger.debug(std::format("loaded {} plugins", plugins.size()));
	return 0;
}

json Plugins::save()
{
	int i = 0;
	json j = json::object();
	for (auto plugin_ptr : plugins) {
		j[plugin_ptr->name.c_str()] = read_config(plugin_ptr);
		logger.debug(std::format("saving #{} plugin {}", ++i, plugin_ptr->name));
	}

	return j;
}

int Plugins::action(int code, int val, const json* data)
{
	int ret;
	ActionEvent ev{ code, val, data };

	ret = 0;
	for (auto plugin_ptr : plugins) {
		// depending on the action, we might want to skip some plugins or
		// handle errors differently
		//
		// A plugin must never be able to take the daemon down. Nothing
		// up the call chain (alarm handling, the poll loop, FUSE
		// read/write) catches anything, so an exception escaping here
		// would reach no handler at all and terminate the process -
		// e.g. a json type_error from a mistyped value() default. Log
		// it and carry on with the remaining plugins.
		try {
			ret += plugin_ptr->action(ev);
		}
		catch (const std::exception& e) {
			logger.error(std::format("plugin {} action {} failed: {}",
				plugin_ptr->name, code, e.what()));
		}
		catch (...) {
			logger.error(std::format("plugin {} action {} failed with an unknown exception",
				plugin_ptr->name, code));
		}
	}
	return ret;
}
