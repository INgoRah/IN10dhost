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
		/* Builds an ActionEvent and hands it to every loaded plugin.
		   data, when given, must stay alive for the duration of the
		   call - plugins are not allowed to retain it. */
		int action(int code, int val = 0, const json* data = nullptr);
};
