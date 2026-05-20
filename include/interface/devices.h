#pragma once

class IDev {
public:
	virtual ~IDev() {}
	virtual const char* get_type() const = 0;
	virtual const char* get_name() const = 0;
};

class IDevices {
public:
	virtual ~IDevices() {};
    virtual IDev* get_dev(uint64_t targetCode) = 0;
};

