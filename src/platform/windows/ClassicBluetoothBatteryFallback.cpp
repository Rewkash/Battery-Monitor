#include "platform/windows/ClassicBluetoothBatteryFallback.h"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2bth.h>

#include "platform/windows/BluetoothSocketUtils.h"
#include "platform/windows/HfpBatterySession.h"
#include "platform/windows/WindowsBluetoothConstants.h"
#include "platform/windows/XiaomiBatteryReadings.h"
#include "platform/windows/ZmiSerialBatterySession.h"

namespace battery_monitor {

namespace {

void LogDebug(bool debug_enabled, XiaomiDebugLogFn debug_log, const std::string& message) {
    if (debug_enabled && debug_log != nullptr) {
        debug_log(message);
    }
}

std::vector<BatteryReading> TryReadZmiSerialBatteryFromSocketWithConfig(SOCKET socket_handle,
                                                                        bool debug_enabled,
                                                                        XiaomiDebugLogFn debug_log,
                                                                        int observe_ms) {
    return TryReadZmiSerialBatteryFromSocket(
        socket_handle,
        &ReplyToHfpAgCommand,
        &ParseAtBatteryPercentFromLine,
        debug_enabled,
        debug_log,
        observe_ms);
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

bool TryConnectPortSocket(const SOCKADDR_BTH& base_address,
                          std::uint32_t port,
                          const char* name,
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
                     "Classic RFCOMM: socket() failed for " + std::string(name) +
                         " error=" + std::to_string(WSAGetLastError()));
            return false;
        }

        ConfigureRfcommSocket(candidate_socket, secure_mode, timeout_ms);

        auto service_address = base_address;
        service_address.serviceClassId = GUID{};
        service_address.port = port;
        if (!ConnectWithTimeout(candidate_socket, service_address, timeout_ms)) {
            LogDebug(debug_enabled, debug_log,
                     "Classic RFCOMM: connect(" + std::string(name) + mode_suffix +
                         ") failed error=" + std::to_string(WSAGetLastError()));
            closesocket(candidate_socket);
            return false;
        }

        *socket_handle = candidate_socket;
        *connected_path = std::string(name) + mode_suffix;
        LogDebug(debug_enabled, debug_log, "Classic RFCOMM: connected via " + *connected_path);
        return true;
    };

    return connect_once(true, "") || connect_once(false, "-insecure");
}

}  // namespace

std::optional<std::uint8_t> TryReadGenericClassicHfpBattery(std::uint64_t bluetooth_address,
                                                            bool debug_enabled,
                                                            XiaomiDebugLogFn debug_log) {
    if (bluetooth_address <= 0xFFFFULL) {
        return std::nullopt;
    }

    LogDebug(debug_enabled, debug_log,
             "Generic HFP fallback: attempting RFCOMM connection to address=" + std::to_string(bluetooth_address));

    ScopedWsa wsa;
    if (!wsa.started()) {
        LogDebug(debug_enabled, debug_log, "Generic HFP fallback: WSAStartup failed");
        return std::nullopt;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.btAddr = bluetooth_address;
    address.port = BT_PORT_ANY;
    std::string connected_path;

    bool connected = TryConnectServiceSocket(address, kHandsfreeAudioGatewayServiceUuid, "HFP-111E", 320,
                                             debug_enabled, debug_log, &socket_handle, &connected_path);
    if (!connected) {
        const auto channels =
            DiscoverRfcommChannelsFromSdp(bluetooth_address, &kHandsfreeAudioGatewayServiceUuid, false);
        for (const auto& channel : channels) {
            const std::string port_label = "port-" + std::to_string(channel.port);
            if (TryConnectPortSocket(address, channel.port, port_label.c_str(), 220,
                                     debug_enabled, debug_log, &socket_handle, &connected_path)) {
                connected = true;
                break;
            }
        }
    }
    if (!connected) {
        connected = TryConnectPortSocket(address, 2U, "port-2", 220, debug_enabled, debug_log,
                                         &socket_handle, &connected_path);
    }
    if (!connected) {
        return std::nullopt;
    }

    const auto battery = TryReadHfpBatteryFromSocket(socket_handle, debug_enabled, debug_log);
    closesocket(socket_handle);
    if (battery.has_value()) {
        LogDebug(debug_enabled, debug_log, "Generic HFP fallback: battery main=" + std::to_string(*battery));
    } else {
        LogDebug(debug_enabled, debug_log, "Generic HFP fallback: battery was not reported");
    }
    return battery;
}

std::vector<BatteryReading> TryReadXiaomiClassicBattery(std::uint64_t bluetooth_address,
                                                        bool enable_dynamic_port_scan,
                                                        XiaomiModeCacheUpdateFn mode_cache_update,
                                                        int zmi_observe_ms,
                                                        bool debug_enabled,
                                                        XiaomiDebugLogFn debug_log) {
    std::vector<BatteryReading> readings;
    LogDebug(debug_enabled, debug_log,
             "Xiaomi classic fallback: attempting RFCOMM connection to address=" + std::to_string(bluetooth_address));

    ScopedWsa wsa;
    if (!wsa.started()) {
        LogDebug(debug_enabled, debug_log, "Xiaomi classic fallback: WSAStartup failed");
        return readings;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.btAddr = bluetooth_address;
    address.port = BT_PORT_ANY;
    std::string connected_path;

    bool connected = TryConnectServiceSocket(address, kXiaomiDeviceCtrlServiceUuid, "FD2D", 280,
                                             debug_enabled, debug_log, &socket_handle, &connected_path) ||
                     TryConnectServiceSocket(address, kBluetoothSerialPortServiceUuid, "SPP-1101", 280,
                                             debug_enabled, debug_log, &socket_handle, &connected_path) ||
                     TryConnectServiceSocket(address, kZmiPurPodsSerialServiceUuid, "ZMI-1101", 280,
                                             debug_enabled, debug_log, &socket_handle, &connected_path) ||
                     TryConnectServiceSocket(address, kHandsfreeAudioGatewayServiceUuid, "HFP-111E", 280,
                                             debug_enabled, debug_log, &socket_handle, &connected_path) ||
                     TryConnectPortSocket(address, 15U, "RFCOMM-port-15", 280,
                                          debug_enabled, debug_log, &socket_handle, &connected_path);

    if (!connected && enable_dynamic_port_scan) {
        std::set<std::uint32_t> sdp_ports_attempted;
        auto try_sdp = [&](const GUID* service_filter, const char* source_name, bool flush_cache) {
            const auto channels = DiscoverRfcommChannelsFromSdp(bluetooth_address, service_filter, flush_cache);
            if (!channels.empty()) {
                LogDebug(debug_enabled, debug_log,
                         "Xiaomi classic fallback: SDP discovered channels=" + std::to_string(channels.size()) +
                             " source=" + std::string(source_name));
            }

            for (const auto& channel : channels) {
                if (!sdp_ports_attempted.insert(channel.port).second) {
                    continue;
                }

                std::string label = std::string(source_name) + "-port-" + std::to_string(channel.port);
                if (!channel.instance_name.empty()) {
                    label += " '" + channel.instance_name + "'";
                }

                if (TryConnectPortSocket(address, channel.port, label.c_str(), 180,
                                         debug_enabled, debug_log, &socket_handle, &connected_path)) {
                    LogDebug(debug_enabled, debug_log,
                             "Xiaomi classic fallback: SDP matched port=" + std::to_string(channel.port) +
                                 " source=" + std::string(source_name));
                    connected = true;
                    return;
                }
            }
        };

        try_sdp(&kZmiPurPodsSerialServiceUuid, "SDP-ZMI-1101", true);
        if (!connected) {
            try_sdp(&kBluetoothSerialPortServiceUuid, "SDP-SPP-1101", false);
        }
        if (!connected) {
            try_sdp(&kHandsfreeAudioGatewayServiceUuid, "SDP-HFP-111E", false);
        }
        if (!connected) {
            try_sdp(nullptr, "SDP-ANY", false);
        }

        if (!connected) {
            constexpr std::array<std::uint32_t, 24> kZmiCandidatePorts = {
                1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                13, 14, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25};
            for (const auto port : kZmiCandidatePorts) {
                if (sdp_ports_attempted.contains(port)) {
                    continue;
                }
                std::string port_name = "RFCOMM-port-" + std::to_string(port);
                if (TryConnectPortSocket(address, port, port_name.c_str(), 110,
                                         debug_enabled, debug_log, &socket_handle, &connected_path)) {
                    LogDebug(debug_enabled, debug_log,
                             "Xiaomi classic fallback: dynamic port scan matched port=" + std::to_string(port));
                    connected = true;
                    break;
                }
            }
        }
    }

    if (!connected) {
        return readings;
    }

    const bool zmi_serial_path =
        connected_path.find("ZMI-1101") != std::string::npos ||
        connected_path.find("SDP-ZMI-1101") != std::string::npos;
    const bool try_zmi_pre_auth_probe = zmi_serial_path || enable_dynamic_port_scan;
    std::vector<BatteryReading> partial_pre_auth_readings;
    if (try_zmi_pre_auth_probe) {
        auto pre_auth_readings =
            TryReadZmiSerialBatteryFromSocketWithConfig(socket_handle, debug_enabled, debug_log, zmi_observe_ms);
        if (!pre_auth_readings.empty()) {
            if (HasUsefulXiaomiTwsReadings(pre_auth_readings)) {
                readings = std::move(pre_auth_readings);
                LogDebug(debug_enabled, debug_log,
                         "Xiaomi classic fallback: ZMI pre-auth serial probe succeeded, entries=" +
                             std::to_string(readings.size()));
                closesocket(socket_handle);
                return readings;
            }

            partial_pre_auth_readings = std::move(pre_auth_readings);
            LogDebug(debug_enabled, debug_log,
                     "Xiaomi classic fallback: ZMI pre-auth serial probe yielded partial entries=" +
                         std::to_string(partial_pre_auth_readings.size()) + ", continue auth flow");
        }
    }

    const int io_timeout_ms = 260;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&io_timeout_ms), sizeof(io_timeout_ms));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&io_timeout_ms), sizeof(io_timeout_ms));

    const bool likely_hfp_transport =
        connected_path.find("HFP") != std::string::npos ||
        connected_path.find("SDP-HFP") != std::string::npos;
    if (enable_dynamic_port_scan && likely_hfp_transport) {
        auto hfp_triplet = TryReadHfpAgBatteryFromSocket(socket_handle, debug_enabled, debug_log);
        if (HasUsefulXiaomiTwsReadings(hfp_triplet)) {
            readings = std::move(hfp_triplet);
            closesocket(socket_handle);
            return readings;
        }

        const auto hfp_main = TryReadHfpBatteryFromSocket(socket_handle, debug_enabled, debug_log);
        if (hfp_main.has_value()) {
            readings.push_back(BatteryReading{"main", *hfp_main});
        }
        if (readings.empty() && !partial_pre_auth_readings.empty()) {
            readings = partial_pre_auth_readings;
        }
        closesocket(socket_handle);
        return readings;
    }

    const auto classic_session =
        RunXiaomiClassicBatterySession(socket_handle, bluetooth_address, mode_cache_update, debug_enabled, debug_log);
    readings = classic_session.readings;
    if (!classic_session.auth_start_sent) {
        if (!partial_pre_auth_readings.empty()) {
            readings = partial_pre_auth_readings;
        }
        if (enable_dynamic_port_scan) {
            auto hfp_triplet = TryReadHfpAgBatteryFromSocket(socket_handle, debug_enabled, debug_log);
            if (HasUsefulXiaomiTwsReadings(hfp_triplet)) {
                readings = std::move(hfp_triplet);
            }
            if (readings.empty()) {
                const auto hfp_main = TryReadHfpBatteryFromSocket(socket_handle, debug_enabled, debug_log);
                if (hfp_main.has_value()) {
                    readings.push_back(BatteryReading{"main", *hfp_main});
                }
            }
        }
        closesocket(socket_handle);
        return readings;
    }

    if (readings.empty()) {
        if (zmi_serial_path || enable_dynamic_port_scan) {
            auto zmi_serial_readings =
                TryReadZmiSerialBatteryFromSocketWithConfig(socket_handle, debug_enabled, debug_log, zmi_observe_ms);
            if (!zmi_serial_readings.empty()) {
                readings = std::move(zmi_serial_readings);
                LogDebug(debug_enabled, debug_log,
                         "Xiaomi classic fallback: ZMI serial pattern fallback succeeded, entries=" +
                             std::to_string(readings.size()));
            }
        }

        if (readings.empty()) {
            LogDebug(debug_enabled, debug_log, "Xiaomi classic fallback: timeout without battery payload");

            const bool likely_hfp_path = connected_path.find("HFP") != std::string::npos;
            if (enable_dynamic_port_scan && likely_hfp_path) {
                auto hfp_triplet = TryReadHfpAgBatteryFromSocket(socket_handle, debug_enabled, debug_log);
                if (!hfp_triplet.empty()) {
                    readings = std::move(hfp_triplet);
                }
            }

            std::optional<std::uint8_t> hfp_battery;
            if (readings.empty() && likely_hfp_path) {
                hfp_battery = TryReadHfpBatteryFromSocket(socket_handle, debug_enabled, debug_log);
            } else if (readings.empty()) {
                SOCKET hfp_socket = INVALID_SOCKET;
                std::string hfp_path;
                if (TryConnectServiceSocket(address, kHandsfreeAudioGatewayServiceUuid, "HFP-111E", 240,
                                            debug_enabled, debug_log, &hfp_socket, &hfp_path)) {
                    if (enable_dynamic_port_scan) {
                        auto hfp_triplet = TryReadHfpAgBatteryFromSocket(hfp_socket, debug_enabled, debug_log);
                        if (!hfp_triplet.empty()) {
                            readings = std::move(hfp_triplet);
                        }
                    }
                    if (readings.empty()) {
                        hfp_battery = TryReadHfpBatteryFromSocket(hfp_socket, debug_enabled, debug_log);
                    }
                    closesocket(hfp_socket);
                }
            }

            if (readings.empty() && hfp_battery.has_value()) {
                readings.push_back(BatteryReading{"main", *hfp_battery});
                LogDebug(debug_enabled, debug_log,
                         "Xiaomi classic fallback: HFP battery main=" + std::to_string(*hfp_battery));
            }
        }
    }

    if (!HasUsefulXiaomiTwsReadings(readings) && !partial_pre_auth_readings.empty()) {
        if (XiaomiReadingsRichnessScore(partial_pre_auth_readings) > XiaomiReadingsRichnessScore(readings)) {
            readings = partial_pre_auth_readings;
        }
    }

    closesocket(socket_handle);
    return readings;
}

}  // namespace battery_monitor
