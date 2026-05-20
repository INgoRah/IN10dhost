#pragma once
#include "plugin.h"

using std::string;

class Plugins {
	private:
		std::filesystem::path exec_path;
		std::vector<Plugin*> plugins;
		Plugin* plugin_init(string name);
		int init();
	public:
		Plugins() { init(); }
		~Plugins() { cleanup(); }
		int cleanup();
		// reload changed plugins
		int reload();
		int add(string name);
		int remove(string name);
		int load(json j);
		json save();
		int action(int action, int val);
};
