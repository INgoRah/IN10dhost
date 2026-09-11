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

int Plugins::init()
{
	char buffer[PATH_MAX];
	ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (length == -1)
		return -1;
	buffer[length] = '\0';
	exec_path = std::filesystem::path(buffer).parent_path();

	logger.debug(std::format("Executable path: {}", exec_path.string()));
	return 0;
}

Plugin* Plugins::plugin_init(string name)
{
	std::filesystem::path f;
	Plugin* instance;
	void* handle;
	std::filesystem::path lib_path;

	f = exec_path / std::filesystem::path(std::string("lib") + name + ".so");
	if (!fs::exists(f)) {
		f = "/opt/lib/IN10dhost" / std::filesystem::path(std::string("lib") + name + ".so");
		if (!fs::exists(f)) {
			return nullptr;
		}
	}
	std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::high_resolution_clock::now().time_since_epoch()).count());
	std::string shadow_path = "/tmp/lib" + name + "." + timestamp + ".so";

	try {
		logger.info(std::format("trying to load plugin {} {} -> {} ...",
			name,
			f.string(),
			shadow_path));
		// Duplicate the newly compiled binary file to the shadow path
		fs::copy_file(f, shadow_path, fs::copy_options::overwrite_existing);

		// 4. Load the unique shadow file instead of the original path
		handle = dlopen(shadow_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
		if (!handle) {
			//std::cerr << "Cannot load library: " << dlerror() << std::endl;
			logger.error(std::format("Cannot load library {}: {}", name, dlerror()));
			return nullptr;
		}
		// Load the symbols (the factory functions)
		create_t create_plugin = (create_t) dlsym(handle, "create_plugin");
		destroy_t destroy_plugin = (destroy_t) dlsym(handle, "destroy_plugin");

		if (!create_plugin || !destroy_plugin) {
			logger.error(std::format("Cannot load symbols for plugin {}: {}", name, dlerror()));
			dlclose(handle);
			return nullptr;
		}
		instance = create_plugin();
		instance->name = name;
		instance->handle = handle;
		instance->lib_path = shadow_path;

		instance->init(&logger, static_cast<IDevices*>(&ow));
	}
	catch (const std::exception& e) {
		logger.error(std::format("Failed to load plugin {}: {}", name, e.what()));
		return nullptr;
	}
	logger.info(std::format("loaded plugin {}...", instance->name));
	plugins.push_back(instance);
	return instance;
}

int Plugins::cleanup()
{
	std::filesystem::path lib_path;

	for (auto plugin_ptr : plugins) {
		logger.info(std::format("Cleaning up plugin {}...", plugin_ptr->name));
		plugin_ptr->exit();
		// remember the lib path before destroying the plugin instance
		lib_path = plugin_ptr->lib_path;
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
	plugins.clear();

	return 0;
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
			p->config_set(pluginConfig);
	}
	logger.debug(std::format("loaded {} plugins", plugins.size()));
	return 0;
}

json Plugins::save()
{
	int i = 0;
	json j = json::object();
	for (auto plugin_ptr : plugins) {
		j[plugin_ptr->name.c_str()] = plugin_ptr->config_get();
		logger.debug(std::format("saving #{} plugin {}", ++i, plugin_ptr->name));
	}

	return j;
}

int Plugins::action(int action, int val)
{
	int ret;

	ret = 0;
	for (auto plugin_ptr : plugins) {
		logger.debug(std::format("plugin {} action: {}", plugin_ptr->name, action));
		// depending on the action, we might want to skip some plugins or
		// handle errors differently
		ret += plugin_ptr->action(action, val);
	}
	return ret;
}

int Plugins::reload()
{
	return 0;
}

int Plugins::add(string name)
{
	(void)name;
	return 0;
}

int Plugins::remove(string name)
{
	(void)name;
	return 0;
}
