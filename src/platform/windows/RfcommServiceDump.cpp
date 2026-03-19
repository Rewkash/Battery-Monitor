#include "platform/windows/RfcommServiceDump.h"

#include "platform/windows/BluetoothSocketUtils.h"
#include "platform/windows/WindowsBluetoothConstants.h"

#include <iostream>
#include <string>

#include <winrt/base.h>

namespace battery_monitor {

namespace {

std::string GuidToString(const winrt::guid& value) {
    return winrt::to_string(winrt::to_hstring(value));
}

}  // namespace

bool DumpBluetoothServicesForAddress(const std::string& device_name, std::uint64_t bluetooth_address) {
    std::cout << "Bluetooth service dump for: " << device_name << " address=" << bluetooth_address << "\n";

    auto dump_channels = [&](const GUID* service_filter, const char* label, bool flush_cache) {
        const auto channels = DiscoverRfcommChannelsFromSdp(bluetooth_address, service_filter, flush_cache);
        std::cout << label << ": " << channels.size() << " channel(s)\n";
        for (const auto& channel : channels) {
            std::cout << "  port=" << channel.port
                      << " uuid=" << GuidToString(winrt::guid(channel.service_uuid));
            if (!channel.instance_name.empty()) {
                std::cout << " name='" << channel.instance_name << "'";
            }
            std::cout << "\n";
        }
    };

    dump_channels(&kXiaomiDeviceCtrlServiceUuid, "FD2D", true);
    dump_channels(&kBluetoothSerialPortServiceUuid, "SPP-1101", false);
    dump_channels(&kZmiPurPodsSerialServiceUuid, "ZMI-1101", false);
    dump_channels(&kHandsfreeAudioGatewayServiceUuid, "HFP-111E", false);
    dump_channels(nullptr, "SDP-ANY", false);
    return true;
}

}  // namespace battery_monitor
