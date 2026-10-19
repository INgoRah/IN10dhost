#include <cstring>
#include <format> // used for std::format
#include <iostream>
#include "plugin.h"
#include "interface/ds2408.h"

class Example : public Plugin {
private:
	ILogger* logger;
	IDevices *devices;
	// what was last written to example/echo
	std::string echo;
public:
	Example() : logger(nullptr), devices(nullptr) {}

	void info() override {
		std::cout << "Example Version 1.0" << std::endl;
	}
	void init(ILogger* logger, IDevices *devices) override {
		this->logger = logger;
		this->devices = devices;
		//LogLevel lvl = logger->get_level();
		logger->debug("Example plugin initialized");
		// files of its own: /status/example next to the daemon's status
		// files, and /example/echo in a directory of its own
		files->file_add(this, "status/example", 32);
		files->file_add(this, "example/echo", 64);
	}

	void exit() {
		logger->debug("Example plugin exit");
		// not needed, the host drops them anyway; how a file goes away
		files->file_remove(this, "status/example");
		this->logger = nullptr;
		this->devices = nullptr;
	 }

	json config_get() const {
		json j;
		j["enabled"] = true;
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

	int fs_read(const string& path, char* buf, size_t size) override
	{
		std::string s = path == "status/example" ? std::string("example plugin running\n") : echo;

		if (s.size() > size)
			return -EINVAL;
		std::memcpy(buf, s.data(), s.size());
		return (int)s.size();
	}

	// status/example is read only, see Plugin::fs_write()
	int fs_write(const string& path, const char* buf, size_t size) override
	{
		if (path != "example/echo")
			return Plugin::fs_write(path, buf, size);
		if (size > 64)
			return -EFBIG;
		echo.assign(buf, size);
		return (int)size;
	}

	int action(const ActionEvent& ev) override
	{
		switch (ev.code) {
			case ACT_READY:
			{
				IDev* dev = devices->get_dev(0x290200FDFF6677F8);
				if (dev) {
					IDS2408* d = dynamic_cast<IDS2408*>(dev);
					if (d) {
						//d->pio_set(1);
						logger->info(std::format("Example plugin action={}, val={}", ev.code, ev.val));
					}
					d = nullptr;
				}
				dev = nullptr;
				// 1 = handled, see the action() contract in plugin.h
				return 1;
			}
			case ACT_ALARM_AFTER:
			 {
				// ev.val is the bus, the alarming device is in ev.data
				if (ev.data)
					logger->info(std::format("Example plugin alarm on bus {} dev {}",
						ev.val, ev.data->value("rom", "?")));
				return 1;
			 }
			case ACT_DEV_CHANGE:
			 {
				// the changed device is in ev.data
				if (ev.data) {
					// dump() renders the whole object as a string;
					// the json itself has no std::format support
					logger->info(std::format("Example plugin change {}", ev.data->dump()));
					// the default handed to value() also picks the type
					// it is read back as - "pio" is a number, so asking
					// for it with a string default throws type_error
					logger->info(std::format("Example plugin dev {} PIO={}",
						ev.data->value("rom", "?"), ev.data->value("pio", 0xff)));
					}
				return 1;
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
}
