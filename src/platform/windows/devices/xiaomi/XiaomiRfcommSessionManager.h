#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "core/INoiseControlProvider.h"
#include "core/BatteryTypes.h"
#include "platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.h"
#include "platform/windows/devices/xiaomi/XiaomiBatteryCodec.h"
#include "platform/windows/devices/xiaomi/XiaomiHandshake.h"

namespace battery_monitor {

class XiaomiRfcommSessionManager final {
   public:
    XiaomiRfcommSessionManager(bool debug_enabled = false, XiaomiDebugLogFn debug_log = nullptr);
    ~XiaomiRfcommSessionManager();

    XiaomiRfcommSessionManager(const XiaomiRfcommSessionManager&) = delete;
    XiaomiRfcommSessionManager& operator=(const XiaomiRfcommSessionManager&) = delete;

    std::vector<BatteryReading> ReadBattery(std::uint64_t address,
                                             ClassicBatteryService preferred_service,
                                             const ProviderOperationContext& operation = {});
    bool SetExperimentalNoiseMode(std::uint64_t address,
                                  std::uint8_t mode_value,
                                  std::uint8_t detail_value);
    bool SetNoiseControlMode(std::uint64_t address, NoiseControlMode mode);
    bool SetNoiseSubmode(std::uint64_t address, std::uint8_t family, std::uint8_t submode);

    void NotifyConnectionChanged(std::uint64_t address, bool connected);
    void Shutdown();

    // Called from session worker threads (rate-limited) when a push message
    // changed battery levels, charging flags or the noise mode. Install it
    // right after construction: existing sessions capture the handler at
    // creation time.
    void SetDataChangedHandler(std::function<void(std::uint64_t)> handler);

   private:
    class Session;
    struct State;

    std::shared_ptr<Session> GetOrCreateSession(std::uint64_t address);

    bool debug_enabled_ = false;
    XiaomiDebugLogFn debug_log_ = nullptr;
    std::unique_ptr<State> state_;
};

}  // namespace battery_monitor
