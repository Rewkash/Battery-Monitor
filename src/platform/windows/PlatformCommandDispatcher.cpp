#include "app/PlatformCommandDispatcher.h"

#include <cstdint>

#include <winrt/base.h>

#include "platform/windows/WinRtBatteryProvider.h"

namespace battery_monitor {

namespace {

int ToExitCode(bool success) {
    return success ? 0 : 2;
}

}  // namespace

std::optional<int> TryRunPlatformCommand(const PlatformCommandOptions& options) {
    if (!options.HasAnyCommand()) {
        return std::nullopt;
    }

    WinRtBatteryProvider provider;

    if (options.set_xiaomi_submode) {
        return ToExitCode(provider.SetXiaomiNoiseSubmode(
            options.requested_submode_family, options.requested_submode, options.device_hint));
    }
    if (options.send_xiaomi_candidate) {
        return ToExitCode(provider.SendXiaomiControlCandidate(options.requested_candidate_id, options.device_hint));
    }
    if (options.set_xiaomi_noise_mode) {
        return ToExitCode(provider.SetXiaomiNoiseMode(options.requested_noise_mode, options.device_hint));
    }
    if (options.observe_xiaomi_control) {
        return ToExitCode(provider.ObserveXiaomiControlSession(options.device_hint, options.observe_seconds));
    }
    if (options.observe_zmi_serial) {
        return ToExitCode(provider.ObserveZmiSerialSession(options.device_hint, options.observe_seconds));
    }
    if (options.dump_bluetooth_services) {
        return ToExitCode(provider.DumpBluetoothServices(options.device_hint));
    }
    if (options.dump_ble_gatt) {
        return ToExitCode(provider.DumpBleGatt(options.device_hint));
    }
    if (options.probe_xiaomi_noise) {
        return ToExitCode(provider.ProbeXiaomiNoiseControl(options.device_hint));
    }

    return std::nullopt;
}

bool PrintPlatformException(const std::exception& exception, std::ostream& stream) {
    const auto* hresult_error = dynamic_cast<const winrt::hresult_error*>(&exception);
    if (hresult_error == nullptr) {
        return false;
    }

    const auto previous_flags = stream.flags();
    stream << "Error: WinRT HRESULT=0x" << std::hex << std::uppercase
           << static_cast<std::uint32_t>(hresult_error->code().value)
           << " message=" << winrt::to_string(hresult_error->message()) << '\n';
    stream.flags(previous_flags);
    return true;
}

}  // namespace battery_monitor
