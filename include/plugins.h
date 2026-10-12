#pragma once
#include <map>
#include <mutex>
#include <vector>
#include "plugin.h"

using std::string;

class Plugins : public IPluginFiles {
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
		Plugin* plugin_init(const string& name);
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
		/* the files the plugins added, by path; see IPluginFiles */
		struct PluginFile {
			Plugin* owner;
			int size;
		};
		std::map<string, PluginFile> files;
		/* Guards files. Taken alone or under mtx, never the other way
		   round: a plugin adds files from init(), under mtx. */
		std::mutex files_mtx;
		/* drops all files of a plugin that goes away */
		void files_drop(Plugin* owner);
		/* ACT_INITIALIZED and ACT_READY as far as they were sent, in
		   order, for a plugin loaded later to catch up on */
		std::vector<int> seen;
		/* one plugin's action(), guarded against it throwing */
		int call_action(Plugin* plugin_ptr, const ActionEvent& ev);
		/* hands a plugin loaded after the start the lifecycle events it
		   missed, see seen; to it alone, after its config is applied */
		void catch_up(Plugin* plugin_ptr);
	public:
		Plugins() { init(); }
		// a destructor must not throw (std::terminate()): whatever
		// cleanup() raises at shutdown is dropped, the logger may be
		// gone already
		~Plugins() {
			try {
				cleanup();
			} catch (...) {
			}
		}
		int cleanup();
		/* Reloads plugins whose library changed, and re-applies the
		   config of the rest so they can re-read external files such
		   as a script. With a name, only that plugin is considered.
		   Returns how many libraries were swapped. */
		int reload(const string& only = string());
		int add(const string& name);
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

		// IPluginFiles
		bool file_add(Plugin* owner, const string& path, int size) override;
		bool file_remove(Plugin* owner, const string& path) override;
		/* For the file system, path without leading or trailing '/':
		   > 0 a plugin file of that size, 0 a directory on the way to
		   one, -ENOENT neither */
		int file_attr(const string& path);
		/* the names of the plugin files and directories directly in dir,
		   "" for the root */
		std::vector<string> file_dir(const string& dir);
		/* hand a read or write to the plugin that added path; -ENOENT
		   when none did */
		int file_read(const string& path, char* buf, size_t size);
		int file_write(const string& path, const char* buf, size_t size);
};
