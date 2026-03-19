#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>

namespace battery_monitor {

class ScopedWsa {
   public:
    ScopedWsa() {
        started_ = (WSAStartup(MAKEWORD(2, 2), &wsa_data_) == 0);
    }

    ~ScopedWsa() {
        if (started_) {
            WSACleanup();
        }
    }

    bool started() const {
        return started_;
    }

   private:
    WSADATA wsa_data_{};
    bool started_ = false;
};

struct DiscoveredRfcommChannel {
    std::uint32_t port = 0;
    GUID service_uuid{};
    std::string instance_name;
};

bool SendAll(SOCKET socket_handle, const std::vector<std::uint8_t>& bytes);
bool ConnectWithTimeout(SOCKET socket_handle, const SOCKADDR_BTH& address, int timeout_ms);
std::vector<DiscoveredRfcommChannel> DiscoverRfcommChannelsFromSdp(std::uint64_t bluetooth_address,
                                                                   const GUID* service_filter,
                                                                   bool flush_cache);
std::optional<std::vector<std::uint8_t>> ReceiveChunk(SOCKET socket_handle);

}  // namespace battery_monitor
