#include "platform/windows/XiaomiControlConnection.h"

#include "platform/windows/XiaomiControlSocket.h"
#include "platform/windows/XiaomiHandshake.h"

namespace battery_monitor {

XiaomiControlConnection::~XiaomiControlConnection() {
    Close();
}

XiaomiControlSocketStatus XiaomiControlConnection::OpenSocket(std::uint64_t bluetooth_address) {
    Close();

    if (!wsa_.started()) {
        return XiaomiControlSocketStatus::kWsaStartupFailed;
    }

    if (!ConnectXiaomiControlSocket(bluetooth_address, &socket_handle_, &connected_path_)) {
        return XiaomiControlSocketStatus::kSocketOpenFailed;
    }

    sequence_ = 0;
    return XiaomiControlSocketStatus::kOk;
}

bool XiaomiControlConnection::Authenticate(bool debug_enabled, XiaomiDebugLogFn debug_log) {
    if (socket_handle_ == INVALID_SOCKET) {
        return false;
    }
    return RunXiaomiAuthHandshake(socket_handle_, &sequence_, debug_enabled, debug_log);
}

SOCKET XiaomiControlConnection::socket_handle() const {
    return socket_handle_;
}

const std::string& XiaomiControlConnection::connected_path() const {
    return connected_path_;
}

std::uint8_t& XiaomiControlConnection::sequence() {
    return sequence_;
}

void XiaomiControlConnection::Close() {
    if (socket_handle_ != INVALID_SOCKET) {
        closesocket(socket_handle_);
        socket_handle_ = INVALID_SOCKET;
    }
    connected_path_.clear();
    sequence_ = 0;
}

}  // namespace battery_monitor
