#include <format> // used for std::format
#include <iostream>
#include "plugin.h"
#include "interface/ds2408.h"

class Example : public Plugin {
private:
	ILogger* logger;
	IDevices *devices;
public:
	void info() override {
		std::cout << "Example Version 1.0" << std::endl;
	}
	void init(ILogger* logger, IDevices *devices) override {
		this->logger = logger;
		this->devices = devices;
		//LogLevel lvl = logger->get_level();
		logger->debug("Example plugin initialized");
	}

	void exit() {
		logger->debug("Example plugin exit");
		this->logger = nullptr;
		this->devices = nullptr;
	 }

	json config_get() const {
		json j;
		j["enabled"] = true;
		j["port"] = 8081;
		// or if using a struct: j = this->mySettingsStruct;
		return j;
	}

	void config_set(const json& j) {
		(void)j;
		/*
		bool en = j.value("enabled", true);
		int port = j.value("port", 8081);
		*/
		// or if using a struct: this->mySettingsStruct = j.get<MySettingsStruct>();
	}

	int action(int action, int val)
	{
		logger->info(std::format("Example plugin action={}, val={}", action, val));
		switch (action) {
			case 2: // initialized
			{
				IDev* dev = devices->get_dev(0x290200FDFF6677F8);
				if (dev) {
					IDS2408* d = dynamic_cast<IDS2408*>(dev);
					if (d) {
						d->pio_set(1);
					}
					d = nullptr;
				}
				dev = nullptr;
				return 0;
			}
			default:
				return 0;
		}
	}
};

// These functions are the "doorway" into the library
extern "C" Plugin* create_plugin() {
	return new Example();
}

extern "C" void destroy_plugin(Plugin* p) {
	delete p;
	std::cout << "Example plugin deleted" << std::endl;
}
