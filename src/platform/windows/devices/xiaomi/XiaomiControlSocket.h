#pragma once

#include <cstdint>
#include <string>

#include <winsock2.h>

#include "platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.h"

namespace battery_monitor {

bool ConnectXiaomiControlSocket(std::uint64_t bluetooth_address,
                                SOCKET* socket_handle,
                                std::string* connected_path);
bool ConnectXiaomiControlSocket(std::uint64_t bluetooth_address,
                                ClassicBatteryService preferred_service,
                                SOCKET* socket_handle,
                                std::string* connected_path);
bool ConnectXiaomiControlSocketForService(std::uint64_t bluetooth_address,
                                          ClassicBatteryService service,
                                          SOCKET* socket_handle,
                                          std::string* connected_path);

}  // namespace battery_monitor
