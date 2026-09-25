/*
 * Fault injection plugin - a test fixture, not an example.
 *
 * Throws from whichever entry point its config names, so the loader's
 * error handling can be exercised:
 *
 *   { "faulty": { "mode": "action" } }      throw from action()
 *   { "faulty": { "mode": "exit" } }        throw from exit()
 *   { "faulty": { "mode": "config_get" } }  throw from config_get()
 *   { "faulty": { "mode": "config_set" } }  throw from config_set()
 *
 * Adding "_odd" to any of those throws something that is not derived
 * from std::exception, to reach the loader's catch(...) instead.
 *
 * Built only for TESTING builds and never installed, see CMakeLists.txt.
 */
#include <stdexcept>
#include <string>
#include "plugin.h"

class Faulty : public Plugin {
private:
	std::string mode;

	/* Throws for the named entry point: a std::exception when the mode
	   is "<name>", a bare int for "<name>_odd" so the loader's
	   catch(...) is exercised rather than its catch(std::exception). */
	void fail(const char* which) const
	{
		if (mode == which)
			throw std::runtime_error(std::string("faulty: ") + which);
		if (mode == std::string(which) + "_odd")
			throw 42;
	}

public:
	void info() override {}

	void init(ILogger*, IDevices*) override {}

	void exit() override { fail("exit"); }

	json config_get() const override
	{
		fail("config_get");

		return json{ { "mode", mode } };
	}

	void config_set(const json& j) override
	{
		// take the mode first so the very call that sets it can throw
		mode = j.value("mode", "");
		fail("config_set");
	}

	int action(const ActionEvent&) override
	{
		fail("action");

		return 0;
	}
};

extern "C" Plugin* create_plugin() {
	return new Faulty();
}

extern "C" void destroy_plugin(Plugin* p) {
	delete p;
}
