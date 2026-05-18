#include <format> // used for std::format
#include <iostream>
#include "plugin.h"

// These functions are the "doorway" into the library
extern "C" Plugin* create_plugin() {
	return nullptr;
}

