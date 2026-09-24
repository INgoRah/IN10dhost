#pragma once
#include <map>
#include <mutex>
#include "plugin.h"

using std::string;

class Plugins {
	private:
		std::filesystem::path exec_path;
		std::vector<Plugin*> plugins;
		/* Guards the plugins vector and every dlopen/dlclose below.
		   add/remove/reload mutate the vector while action() walks it
		   from the poll worker and the FUSE threads, and unloading a
		   library out from under a thread executing in it would
		   crash. Recursive because reload() calls remove()/add().
		   A plugin that starts threads of its own is still on its own
		   to stop them in exit(). */
		std::recursive_mutex mtx;
		/* content hash of the source .so each plugin was loaded from,
		   keyed by plugin name, so reload() only reloads what changed */
		std::map<string, uint64_t> hashes;
		Plugin* plugin_init(string name);
		int init();
		/* tears one plugin down: exit(), destroy, dlclose, drop the
		   shadow copy. Does not touch the plugins vector. */
		void unload(Plugin* plugin_ptr);
		/* Where the library for this plugin lives, empty if not found */
		std::filesystem::path find_lib(const string& name) const;
	public:
		Plugins() { init(); }
		~Plugins() { cleanup(); }
		int cleanup();
		// reload plugins whose library or script changed
		int reload();
		int add(string name);
		int remove(string name);
		int load(json j);
		json save();
		/* number of currently loaded plugins */
		size_t count();
		/* Builds an ActionEvent and hands it to every loaded plugin.
		   data, when given, must stay alive for the duration of the
		   call - plugins are not allowed to retain it. */
		int action(int code, int val = 0, const json* data = nullptr);
};
