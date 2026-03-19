#include "platform/windows/bluetooth/BluetoothSocketUtils.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <set>
#include <vector>

#include <winrt/base.h>

namespace battery_monitor {

namespace {

std::optional<std::wstring> BuildBluetoothLookupContext(std::uint64_t bluetooth_address) {
    SOCKADDR_BTH context_address{};
    context_address.addressFamily = AF_BTH;
    context_address.btAddr = bluetooth_address;
    context_address.port = BT_PORT_ANY;
    context_address.serviceClassId = GUID{};

    std::array<wchar_t, 96> context_text{};
    DWORD context_length = static_cast<DWORD>(context_text.size());
    if (WSAAddressToStringW(reinterpret_cast<LPSOCKADDR>(&context_address), sizeof(context_address),
                           nullptr, context_text.data(), &context_length) == SOCKET_ERROR) {
        return std::nullopt;
    }

    return std::wstring(context_text.data());
}

}  // namespace

bool SendAll(SOCKET socket_handle, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int chunk = send(socket_handle, reinterpret_cast<const char*>(bytes.data() + sent),
                               static_cast<int>(bytes.size() - sent), 0);
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

bool ConnectWithTimeout(SOCKET socket_handle, const SOCKADDR_BTH& address, int timeout_ms) {
    u_long non_blocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        return false;
    }

    const int connect_result =
        connect(socket_handle, reinterpret_cast<const SOCKADDR*>(&address), sizeof(address));
    if (connect_result == SOCKET_ERROR) {
        const int connect_error = WSAGetLastError();
        if (connect_error != WSAEWOULDBLOCK && connect_error != WSAEINPROGRESS && connect_error != WSAEINVAL) {
            non_blocking = 0;
            ioctlsocket(socket_handle, FIONBIO, &non_blocking);
            WSASetLastError(connect_error);
            return false;
        }

        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_handle, &write_set);

        TIMEVAL timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        const int select_result = select(0, nullptr, &write_set, nullptr, &timeout);
        if (select_result <= 0) {
            non_blocking = 0;
            ioctlsocket(socket_handle, FIONBIO, &non_blocking);
            WSASetLastError(WSAETIMEDOUT);
            return false;
        }

        int socket_error = 0;
        int option_length = sizeof(socket_error);
        if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socket_error), &option_length) == SOCKET_ERROR ||
            socket_error != 0) {
            non_blocking = 0;
            ioctlsocket(socket_handle, FIONBIO, &non_blocking);
            WSASetLastError(socket_error == 0 ? WSAECONNABORTED : socket_error);
            return false;
        }
    }

    non_blocking = 0;
    ioctlsocket(socket_handle, FIONBIO, &non_blocking);
    return true;
}

std::vector<DiscoveredRfcommChannel> DiscoverRfcommChannelsFromSdp(std::uint64_t bluetooth_address,
                                                                   const GUID* service_filter,
                                                                   bool flush_cache) {
    std::vector<DiscoveredRfcommChannel> channels;
    const auto lookup_context = BuildBluetoothLookupContext(bluetooth_address);
    if (!lookup_context.has_value()) {
        return channels;
    }

    WSAQUERYSETW query{};
    query.dwSize = sizeof(query);
    query.dwNameSpace = NS_BTH;
    query.lpszContext = const_cast<LPWSTR>(lookup_context->c_str());
    if (service_filter != nullptr) {
        query.lpServiceClassId = const_cast<LPGUID>(service_filter);
    }

    HANDLE lookup_handle = nullptr;
    DWORD begin_flags = LUP_RETURN_ADDR | LUP_RETURN_NAME;
    if (flush_cache) {
        begin_flags |= LUP_FLUSHCACHE;
    }

    if (WSALookupServiceBeginW(&query, begin_flags, &lookup_handle) == SOCKET_ERROR) {
        return channels;
    }

    std::set<std::uint32_t> seen_ports;
    std::vector<std::uint8_t> buffer(4096);

    while (true) {
        DWORD buffer_length = static_cast<DWORD>(buffer.size());
        auto* result = reinterpret_cast<WSAQUERYSETW*>(buffer.data());
        std::memset(result, 0, buffer.size());
        result->dwSize = sizeof(WSAQUERYSETW);

        if (WSALookupServiceNextW(lookup_handle, LUP_RETURN_ADDR | LUP_RETURN_NAME,
                                  &buffer_length, result) == SOCKET_ERROR) {
            const int error_code = WSAGetLastError();
            if (error_code == WSAEFAULT && buffer_length > buffer.size()) {
                buffer.resize(buffer_length);
                continue;
            }
            break;
        }

        if (result->lpcsaBuffer == nullptr || result->dwNumberOfCsAddrs == 0U) {
            continue;
        }

        for (DWORD index = 0; index < result->dwNumberOfCsAddrs; ++index) {
            const auto& csaddr = result->lpcsaBuffer[index];
            if (csaddr.RemoteAddr.lpSockaddr == nullptr ||
                csaddr.RemoteAddr.iSockaddrLength < static_cast<int>(sizeof(SOCKADDR_BTH))) {
                continue;
            }

            const auto* remote = reinterpret_cast<const SOCKADDR_BTH*>(csaddr.RemoteAddr.lpSockaddr);
            if (remote->addressFamily != AF_BTH) {
                continue;
            }
            if (remote->port == BT_PORT_ANY || remote->port == 0U || remote->port > 60U) {
                continue;
            }
            if (!seen_ports.insert(remote->port).second) {
                continue;
            }

            DiscoveredRfcommChannel channel;
            channel.port = remote->port;
            channel.service_uuid = service_filter != nullptr ? *service_filter : remote->serviceClassId;
            if (result->lpszServiceInstanceName != nullptr) {
                channel.instance_name = winrt::to_string(winrt::hstring(result->lpszServiceInstanceName));
            }
            channels.push_back(std::move(channel));
        }
    }

    WSALookupServiceEnd(lookup_handle);
    return channels;
}

std::optional<std::vector<std::uint8_t>> ReceiveChunk(SOCKET socket_handle) {
    std::array<char, 4096> recv_buffer{};
    const int received = recv(socket_handle, recv_buffer.data(), static_cast<int>(recv_buffer.size()), 0);
    if (received <= 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(received));
    std::memcpy(bytes.data(), recv_buffer.data(), static_cast<std::size_t>(received));
    return bytes;
}

}  // namespace battery_monitor

