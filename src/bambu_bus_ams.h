#pragma once
#include <stdint.h>

enum class bambubus_package_type 
{
    error = -1,
    none = 0,
    filament_motion_short,
    filament_motion_long,
    online_detect,
    REQx6,
    NFC_detect,
    set_filament_info,
    MC_online,
    read_filament_info,
    set_filament_info_type2,
    version,
    serial_number,
    heartbeat,
    ETC,


    __BambuBus_package_packge_type_size
};


extern bambubus_package_type bambubus_run();
extern uint16_t bambubus_ams_address;