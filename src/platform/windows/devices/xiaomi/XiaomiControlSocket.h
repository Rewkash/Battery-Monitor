#pragma once

#include <cstdint>
#include <string>

#include <winsock2.h>

namespace battery_monitor {

bool ConnectXiaomiControlSocket(std::uint64_t bluetooth_address,
                                SOCKET* socket_handle,
                                std::string* connected_path);

}  // namespace battery_monitor
