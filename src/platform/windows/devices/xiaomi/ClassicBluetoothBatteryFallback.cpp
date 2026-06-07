#include "platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.h"

#include <cstdint>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2bth.h>

#include "platform/windows/bluetooth/BluetoothSocketUtils.h"
#include "platform/windows/shared/WindowsBluetoothConstants.h"

namespace battery_monitor {

namespace {

void LogDebug(bool debug_enabled, XiaomiDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

void ConfigureRfcommSocket(SOCKET socket_handle, bool secure_mode, int timeout_ms) {
    const ULONG secure_transport = secure_mode ? TRUE : FALSE;
    setsockopt(socket_handle, SOL_RFCOMM, SO_BTH_AUTHENTICATE,
               reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));
    setsockopt(socket_handle, SOL_RFCOMM, SO_BTH_ENCRYPT,
               reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
}

bool TryConnectServiceSocket(const SOCKADDR_BTH& base_address,
                             const GUID& service_uuid,
                             const char* service_name,
                             int timeout_ms,
                             bool debug_enabled,
                             XiaomiDebugLogFn debug_log,
                             SOCKET* socket_handle,
                             std::string* connected_path) {
    if (socket_handle == nullptr || connected_path == nullptr) {
        return false;
    }

    auto connect_once = [&](bool secure_mode, const char* mode_suffix) -> bool {
        const SOCKET candidate_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (candidate_socket == INVALID_SOCKET) {
            LogDebug(debug_enabled, debug_log,
                     "Classic RFCOMM: socket() failed for " + std::string(service_name) +
                         " error=" + std::to_string(WSAGetLastError()));
            return false;
        }

        ConfigureRfcommSocket(candidate_socket, secure_mode, timeout_ms);

        auto service_address = base_address;
        service_address.serviceClassId = service_uuid;
        if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
            LogDebug(debug_enabled, debug_log,
                     "Classic RFCOMM: connect(" + std::string(service_name) + mode_suffix +
                         ") failed error=" + std::to_string(WSAGetLastError()));
            closesocket(candidate_socket);
            return false;
        }

        *socket_handle = candidate_socket;
        *connected_path = std::string(service_name) + mode_suffix;
        LogDebug(debug_enabled, debug_log, "Classic RFCOMM: connected via " + *connected_path);
        return true;
    };

    return connect_once(true, "") || connect_once(false, "-insecure");
}

}  // namespace

std::vector<BatteryReading> TryReadXiaomiClassicBattery(std::uint64_t bluetooth_address,
                                                        XiaomiModeCacheUpdateFn mode_cache_update,
                                                        bool debug_enabled,
                                                        XiaomiDebugLogFn debug_log) {
    LogDebug(debug_enabled, debug_log,
             "Xiaomi classic fallback: attempting RFCOMM connection to address=" + std::to_string(bluetooth_address));

    ScopedWsa wsa;
    if (!wsa.started()) {
        LogDebug(debug_enabled, debug_log, "Xiaomi classic fallback: WSAStartup failed");
        return {};
    }

    SOCKET socket_handle = INVALID_SOCKET;
    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.btAddr = bluetooth_address;
    address.port = BT_PORT_ANY;
    std::string connected_path;

    const bool connected =
        TryConnectServiceSocket(address, kXiaomiDeviceCtrlServiceUuid, "FD2D", 280,
                                debug_enabled, debug_log, &socket_handle, &connected_path) ||
        TryConnectServiceSocket(address, kBluetoothSerialPortServiceUuid, "SPP-1101", 280,
                                debug_enabled, debug_log, &socket_handle, &connected_path) ||
        TryConnectServiceSocket(address, kZmiPurPodsSerialServiceUuid, "ZMI-1101", 280,
                                debug_enabled, debug_log, &socket_handle, &connected_path);
    if (!connected) {
        return {};
    }

    const int io_timeout_ms = 260;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&io_timeout_ms), sizeof(io_timeout_ms));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&io_timeout_ms), sizeof(io_timeout_ms));

    const auto classic_session =
        RunXiaomiClassicBatterySession(socket_handle, bluetooth_address, mode_cache_update, debug_enabled, debug_log);

    closesocket(socket_handle);
    return classic_session.readings;
}

}  // namespace battery_monitor

