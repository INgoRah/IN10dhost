#pragma once
#include <map>
#include <mutex>
#include <vector>
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
		/* Loads lib<name>.so from a private copy - see the comment on
		   the definition for why the copy is not optional. */
		Plugin* plugin_init(string name);
		int init();
		/* tears one plugin down: exit(), destroy, dlclose, drop the
		   shadow copy. Does not touch the plugins vector. */
		void unload(Plugin* plugin_ptr);
		/* Where the library for this plugin lives, empty if not found */
		std::filesystem::path find_lib(const string& name) const;
		/* config_get()/config_set() run inside the plugin, so they are
		   guarded like action() is: a plugin handed a config it does
		   not like must fail alone, not take the daemon down or abort
		   whatever batch it was part of. read_config() reports an empty
		   object when the plugin throws. */
		json read_config(Plugin* plugin_ptr);
		void apply_config(Plugin* plugin_ptr, const json& cfg);
	public:
		Plugins() { init(); }
		~Plugins() { cleanup(); }
		int cleanup();
		/* Reloads plugins whose library changed, and re-applies the
		   config of the rest so they can re-read external files such
		   as a script. With a name, only that plugin is considered.
		   Returns how many libraries were swapped. */
		int reload(const string& only = string());
		int add(string name);
		int remove(string name);
		int load(json j);
		json save();
		/* number of currently loaded plugins */
		size_t count();
		/* names of the currently loaded plugins, for listing them */
		std::vector<string> names();
		/* config of one loaded plugin, null json if not loaded */
		json config_of(const string& name);
		/* Builds an ActionEvent and hands it to every loaded plugin.
		   data, when given, must stay alive for the duration of the
		   call - plugins are not allowed to retain it. */
		int action(int code, int val = 0, const json* data = nullptr);
};
