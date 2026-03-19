#include "platform/windows/XiaomiControlSocket.h"

#include "platform/windows/BluetoothSocketUtils.h"
#include "platform/windows/WindowsBluetoothConstants.h"

#include <string>

#include <ws2bth.h>

namespace battery_monitor {

bool ConnectXiaomiControlSocket(std::uint64_t bluetooth_address,
                                SOCKET* socket_handle,
                                std::string* connected_path) {
    if (socket_handle == nullptr || connected_path == nullptr) {
        return false;
    }

    *socket_handle = INVALID_SOCKET;
    connected_path->clear();

    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.btAddr = bluetooth_address;
    address.port = BT_PORT_ANY;

    auto try_connect = [&](const GUID& service_uuid, const char* service_name) -> bool {
        auto connect_once = [&](bool secure_mode, const char* mode_suffix) -> bool {
            const SOCKET candidate_socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
            if (candidate_socket == INVALID_SOCKET) {
                return false;
            }

            const ULONG secure_transport = secure_mode ? TRUE : FALSE;
            setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_AUTHENTICATE,
                       reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));
            setsockopt(candidate_socket, SOL_RFCOMM, SO_BTH_ENCRYPT,
                       reinterpret_cast<const char*>(&secure_transport), sizeof(secure_transport));

            const int timeout_ms = 320;
            setsockopt(candidate_socket, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            setsockopt(candidate_socket, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

            auto service_address = address;
            service_address.serviceClassId = service_uuid;
            if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
                closesocket(candidate_socket);
                return false;
            }

            *socket_handle = candidate_socket;
            *connected_path = std::string(service_name) + mode_suffix;
            return true;
        };

        return connect_once(true, "") || connect_once(false, "-insecure");
    };

    return try_connect(kXiaomiDeviceCtrlServiceUuid, "FD2D") ||
           try_connect(kBluetoothSerialPortServiceUuid, "SPP-1101") ||
           try_connect(kZmiPurPodsSerialServiceUuid, "ZMI-1101");
}

}  // namespace battery_monitor
