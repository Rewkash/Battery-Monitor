#pragma once

#include <cstdint>
#include <string>

#include <winsock2.h>

#include "platform/windows/bluetooth/BluetoothSocketUtils.h"

namespace battery_monitor {

using XiaomiDebugLogFn = void (*)(const std::string&);

enum class XiaomiControlSocketStatus {
    kOk,
    kWsaStartupFailed,
    kSocketOpenFailed,
};

class XiaomiControlConnection {
   public:
    XiaomiControlConnection() = default;
    ~XiaomiControlConnection();

    XiaomiControlSocketStatus OpenSocket(std::uint64_t bluetooth_address);
    bool Authenticate(bool debug_enabled = false, XiaomiDebugLogFn debug_log = nullptr);

    SOCKET socket_handle() const;
    const std::string& connected_path() const;
    std::uint8_t& sequence();

   private:
    void Close();

    ScopedWsa wsa_{};
    SOCKET socket_handle_ = INVALID_SOCKET;
    std::string connected_path_;
    std::uint8_t sequence_ = 0;
};

}  // namespace battery_monitor

