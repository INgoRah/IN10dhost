#pragma once

#include "ow_dev.h"
#include "interface/ds2450.h"

class ds2450 : public OwDev, public IDS2450 {
};
