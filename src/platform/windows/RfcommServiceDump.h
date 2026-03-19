#pragma once

#include <cstdint>
#include <string>

namespace battery_monitor {

bool DumpBluetoothServicesForAddress(const std::string& device_name, std::uint64_t bluetooth_address);

}  // namespace battery_monitor
