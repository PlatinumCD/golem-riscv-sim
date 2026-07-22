#ifndef HW_MISC_MITTENS_NIC_H
#define HW_MISC_MITTENS_NIC_H

#include "hw/sysbus.h"

#define TYPE_MITTENS_NIC "mittens-nic"

DeviceState *mittens_nic_create(hwaddr address);

#endif
