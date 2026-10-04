#pragma once
#include <string>

class IDev {
public:
	virtual ~IDev() {}
	/* copies: the device may be renamed from another thread at any time,
	   a pointer into it would not stay valid */
	virtual std::string get_type() const = 0;
	virtual std::string get_name() const = 0;
};

class IDevices {
public:
	virtual ~IDevices() {};
    virtual IDev* get_dev(uint64_t targetCode) = 0;
};

