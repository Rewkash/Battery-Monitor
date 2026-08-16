#include "platform/windows/devices/xiaomi/XiaomiRfcommSessionManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <variant>

#include <winsock2.h>

#include "platform/windows/bluetooth/BluetoothSocketUtils.h"
#include "platform/windows/devices/xiaomi/XiaomiControlConnection.h"
#include "platform/windows/devices/xiaomi/XiaomiControlSession.h"
#include "platform/windows/devices/xiaomi/XiaomiModeCache.h"
#include "platform/windows/devices/xiaomi/XiaomiNoiseModeCodec.h"
#include "platform/windows/devices/xiaomi/XiaomiProtocol.h"

namespace battery_monitor {

namespace {

using Clock = std::chrono::steady_clock;
using RequestResult = std::variant<std::vector<BatteryReading>, bool>;

enum class RequestKind { kBattery, kExperimentalMode, kNoiseMode, kNoiseSubmode };

struct SessionRequest {
    RequestKind kind = RequestKind::kBattery;
    ClassicBatteryService preferred_service = ClassicBatteryService::kXiaomiDeviceControl;
    NoiseControlMode noise_mode = NoiseControlMode::Off;
    std::uint8_t first_value = 0;
    std::uint8_t second_value = 0;
    std::promise<RequestResult> completion;
};

struct BatteryObservation {
    std::optional<XiaomiBatterySnapshot> device_info;
    std::optional<XiaomiBatterySnapshot> status;
    std::optional<Clock::time_point> device_info_received_at;
    std::optional<Clock::time_point> report_status_seen_at;
};

std::vector<BatteryReading> ResolveReadings(const BatteryObservation& observation) {
    if (!observation.status.has_value() && !observation.device_info.has_value()) {
        return {};
    }
    if (!observation.status.has_value()) {
        return BuildXiaomiBatteryReadings(*observation.device_info);
    }
    auto merged = *observation.status;
    if (observation.device_info.has_value()) {
        if (!merged.left.has_value()) merged.left = observation.device_info->left;
        if (!merged.right.has_value()) merged.right = observation.device_info->right;
        if (!merged.case_level.has_value()) merged.case_level = observation.device_info->case_level;
    }
    return BuildXiaomiBatteryReadings(merged);
}

std::uint8_t NoiseModeValue(NoiseControlMode mode) {
    if (mode == NoiseControlMode::Anc) return 0x01U;
    if (mode == NoiseControlMode::Transparency) return 0x02U;
    return 0x00U;
}

}  // namespace

class XiaomiRfcommSessionManager::Session final {
   public:
    Session(std::uint64_t address, bool debug_enabled, XiaomiDebugLogFn debug_log)
        : address_(address), debug_enabled_(debug_enabled), debug_log_(debug_log), worker_(&Session::WorkerMain, this) {}

    ~Session() { Stop(); }

    std::vector<BatteryReading> ReadBattery(ClassicBatteryService preferred_service,
                                            const ProviderOperationContext& operation) {
        if (operation.IsCancelled()) return {};
        auto request = MakeRequest(RequestKind::kBattery);
        request->preferred_service = preferred_service;
        auto future = request->completion.get_future();
        if (!Enqueue(request)) return {};
        while (!operation.IsCancelled()) {
            const auto wait = operation.Remaining(std::chrono::milliseconds(50));
            if (wait <= std::chrono::milliseconds::zero()) break;
            if (future.wait_for(wait) == std::future_status::ready) {
                return std::get<std::vector<BatteryReading>>(future.get());
            }
        }
        return {};
    }

    bool SetExperimentalNoiseMode(std::uint8_t mode_value, std::uint8_t detail_value) {
        auto request = MakeRequest(RequestKind::kExperimentalMode);
        request->first_value = mode_value;
        request->second_value = detail_value;
        auto future = request->completion.get_future();
        return Enqueue(request) && std::get<bool>(future.get());
    }

    bool SetNoiseControlMode(NoiseControlMode mode) {
        auto request = MakeRequest(RequestKind::kNoiseMode);
        request->noise_mode = mode;
        auto future = request->completion.get_future();
        return Enqueue(request) && std::get<bool>(future.get());
    }

    bool SetNoiseSubmode(std::uint8_t family, std::uint8_t submode) {
        auto request = MakeRequest(RequestKind::kNoiseSubmode);
        request->first_value = family;
        request->second_value = submode;
        auto future = request->completion.get_future();
        return Enqueue(request) && std::get<bool>(future.get());
    }

    void Stop() {
        RequestStop();
        if (worker_.joinable()) worker_.join();
    }

    void RequestStop() {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
            if (published_socket_ != INVALID_SOCKET) shutdown(published_socket_, SD_BOTH);
        }
        condition_.notify_all();
    }

   private:
    enum class ReceiveStatus { kData, kTimeout, kClosed, kFailed };

    static std::shared_ptr<SessionRequest> MakeRequest(RequestKind kind) {
        auto request = std::make_shared<SessionRequest>();
        request->kind = kind;
        return request;
    }

    bool Enqueue(const std::shared_ptr<SessionRequest>& request) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            requested_connection_.store(true);
            requests_.push_back(request);
        }
        condition_.notify_one();
        return true;
    }

    bool IsStopping() const {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    void Log(const std::string& message) const {
        if (debug_enabled_ && debug_log_ != nullptr) debug_log_(message);
    }

    void PublishSocket(SOCKET value) {
        std::lock_guard lock(mutex_);
        published_socket_ = value;
        if (stopping_ && published_socket_ != INVALID_SOCKET) shutdown(published_socket_, SD_BOTH);
    }

    void CloseConnection() {
        PublishSocket(INVALID_SOCKET);
        connection_.Close();
        rx_buffer_.clear();
        background_battery_ = {};
        connected_ = false;
    }

    bool Connect(ClassicBatteryService preferred_service) {
        CloseConnection();
        std::vector<ClassicBatteryService> services;
        const auto add_service = [&](ClassicBatteryService service) {
            if (std::find(services.begin(), services.end(), service) == services.end()) {
                services.push_back(service);
            }
        };
        if (const auto known = TryGetSuccessfulClassicBatteryService(address_); known.has_value()) {
            add_service(*known);
        }
        add_service(preferred_service);
        add_service(ClassicBatteryService::kXiaomiDeviceControl);
        add_service(ClassicBatteryService::kBluetoothSerialPort);
        add_service(ClassicBatteryService::kZmiPurPodsSerial);

        for (std::size_t index = 0; index < services.size() && !IsStopping(); ++index) {
            if (connection_.OpenSocketForService(address_, services[index]) !=
                XiaomiControlSocketStatus::kOk) {
                continue;
            }
            PublishSocket(connection_.socket_handle());
            if (!connection_.Authenticate(debug_enabled_, debug_log_)) {
                CloseConnection();
                continue;
            }
            RememberSuccessfulClassicBatteryService(address_, services[index]);
            connected_ = true;
            reconnect_service_ = services[index];
            reconnect_delay_ = std::chrono::milliseconds(250);
            Log("Xiaomi persistent RFCOMM: connected address=" + std::to_string(address_) +
                " path=" + connection_.connected_path());
            return true;
        }
        Log("Xiaomi persistent RFCOMM: connection/authentication failed address=" +
            std::to_string(address_));
        CloseConnection();
        return false;
    }

    ReceiveStatus Receive(std::vector<std::uint8_t>* chunk) {
        std::array<std::uint8_t, 4096> buffer{};
        const int received = recv(connection_.socket_handle(), reinterpret_cast<char*>(buffer.data()),
                                  static_cast<int>(buffer.size()), 0);
        if (received > 0) {
            chunk->assign(buffer.begin(), buffer.begin() + received);
            return ReceiveStatus::kData;
        }
        if (received == 0) return ReceiveStatus::kClosed;
        const int error = WSAGetLastError();
        return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK
                   ? ReceiveStatus::kTimeout
                   : ReceiveStatus::kFailed;
    }

    void ObserveMessage(const XiaomiMessage& message, BatteryObservation* battery) {
        Log(FormatXiaomiMessageLine(message, true, "Xiaomi persistent rx "));
        if (IsXiaomiReportStatusNotification(message) &&
            !SendXiaomiReportStatusAck(connection_.socket_handle(), message)) {
            connected_ = false;
            return;
        }
        const auto mode = ParseXiaomiNoiseModeCode(static_cast<std::uint8_t>(message.opcode), message.payload);
        if (mode.has_value()) {
            const auto submode = static_cast<std::uint8_t>(message.opcode) == 0xF4U
                                     ? ParseXiaomiNoiseSubmodeCodeFromF4Payload(message.payload)
                                     : std::optional<std::uint8_t>{};
            PutXiaomiModeCacheEntry(address_, *mode, submode);
            last_mode_ = *mode;
        }
        if (battery == nullptr || message.payload.empty()) return;
        if (message.opcode == XiaomiOpcode::kGetDeviceInfo) {
            const auto snapshot = ExtractBatterySnapshotFromXiaomiPayload(
                message.payload, std::optional<std::uint8_t>{std::uint8_t{0x07}}, debug_enabled_, debug_log_);
            if (snapshot.has_value()) {
                battery->device_info = snapshot;
                battery->device_info_received_at = Clock::now();
            }
        } else if (message.opcode == XiaomiOpcode::kReportStatus) {
            battery->report_status_seen_at = Clock::now();
            const auto snapshot = ExtractBatterySnapshotFromXiaomiPayload(
                message.payload, std::optional<std::uint8_t>{std::uint8_t{0x00}}, debug_enabled_, debug_log_);
            if (snapshot.has_value()) battery->status = snapshot;
        }
    }

    ReceiveStatus ReceiveAndDispatch(BatteryObservation* battery) {
        std::vector<std::uint8_t> chunk;
        const auto status = Receive(&chunk);
        if (status != ReceiveStatus::kData) return status;
        for (const auto& message : AppendAndDecodeXiaomiMessages(&rx_buffer_, chunk)) {
            ObserveMessage(message, battery);
        }
        return connected_ ? ReceiveStatus::kData : ReceiveStatus::kFailed;
    }

    bool SendMessage(XiaomiMessageType type, std::uint8_t opcode,
                     std::initializer_list<std::uint8_t> payload) {
        XiaomiMessage message;
        message.type = type;
        message.opcode = static_cast<XiaomiOpcode>(opcode);
        message.sequence = connection_.sequence()++;
        message.payload.assign(payload.begin(), payload.end());
        if (SendAll(connection_.socket_handle(), EncodeXiaomiMessage(message))) return true;
        connected_ = false;
        return false;
    }

    bool SendInfoRequest(XiaomiOpcode opcode) {
        XiaomiMessage message;
        message.type = XiaomiMessageType::kPhoneRequest;
        message.opcode = opcode;
        message.sequence = connection_.sequence()++;
        if (SendAll(connection_.socket_handle(), EncodeXiaomiMessage(message))) return true;
        connected_ = false;
        return false;
    }

    std::vector<BatteryReading> HandleBatteryRequest() {
        BatteryObservation observation = std::move(background_battery_);
        background_battery_ = {};
        observation.device_info_received_at.reset();
        observation.report_status_seen_at.reset();
        if (!SendInfoRequest(XiaomiOpcode::kGetDeviceInfo) ||
            !SendInfoRequest(XiaomiOpcode::kGetDeviceRunInfo)) {
            return {};
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(1600);
        while (connected_ && !IsStopping() && Clock::now() < deadline) {
            const auto now = Clock::now();
            if (observation.status.has_value()) break;
            if (observation.device_info_received_at.has_value() &&
                now - *observation.device_info_received_at > std::chrono::milliseconds(500)) break;
            if (observation.report_status_seen_at.has_value() &&
                now - *observation.report_status_seen_at > std::chrono::milliseconds(900)) break;
            const auto status = ReceiveAndDispatch(&observation);
            if (status == ReceiveStatus::kClosed || status == ReceiveStatus::kFailed) connected_ = false;
        }
        return ResolveReadings(observation);
    }

    bool WaitForMode(std::uint8_t expected_mode, std::chrono::milliseconds timeout) {
        last_mode_.reset();
        const auto deadline = Clock::now() + timeout;
        while (connected_ && !IsStopping() && Clock::now() < deadline) {
            const auto status = ReceiveAndDispatch(&background_battery_);
            if (last_mode_ == expected_mode) return true;
            if (status == ReceiveStatus::kClosed || status == ReceiveStatus::kFailed) connected_ = false;
        }
        return false;
    }

    bool HandleExperimentalMode(const SessionRequest& request) {
        if (!SendMessage(static_cast<XiaomiMessageType>(0x01U), 0x0EU,
                         {0x02, 0x04, request.first_value})) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (!SendMessage(static_cast<XiaomiMessageType>(0x01U), 0xF4U,
                         {0x04, 0x00, 0x0B, request.first_value, request.second_value})) return false;
        WaitForMode(request.first_value, std::chrono::milliseconds(2500));
        return true;
    }

    bool HandleNoiseMode(NoiseControlMode mode) {
        const auto mode_value = NoiseModeValue(mode);
        if (connection_.connected_path().starts_with("ZMI-1101")) {
            const std::uint8_t detail = mode == NoiseControlMode::Anc ? 0x02U
                                        : mode == NoiseControlMode::Transparency ? 0x01U : 0x00U;
            if (!SendMessage(XiaomiMessageType::kPhoneRequest, 0x0EU, {0x02, 0x04, mode_value})) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (!SendMessage(XiaomiMessageType::kPhoneRequest, 0xF4U,
                             {0x04, 0x00, 0x0B, mode_value, detail})) return false;
        } else if (!SendMessage(static_cast<XiaomiMessageType>(0xC1U), 0x08U,
                                {0x02, 0x04, mode_value})) {
            return false;
        }
        return WaitForMode(mode_value, std::chrono::milliseconds(1800));
    }

    bool HandleNoiseSubmode(const SessionRequest& request) {
        const bool sent = SendMessage(static_cast<XiaomiMessageType>(0xC1U), 0xF2U,
                                      {0x04, 0x00, 0x0B, request.first_value, request.second_value});
        if (sent) PutXiaomiModeCacheEntry(address_, request.first_value, request.second_value);
        return sent;
    }

    void CompleteFailure(const std::shared_ptr<SessionRequest>& request) noexcept {
        try {
            if (request->kind == RequestKind::kBattery) request->completion.set_value(std::vector<BatteryReading>{});
            else request->completion.set_value(false);
        } catch (const std::future_error&) {
        }
    }

    void HandleRequest(const std::shared_ptr<SessionRequest>& request) {
        if (!connected_ && !Connect(request->preferred_service)) {
            CompleteFailure(request);
            return;
        }
        switch (request->kind) {
            case RequestKind::kBattery: request->completion.set_value(HandleBatteryRequest()); break;
            case RequestKind::kExperimentalMode: request->completion.set_value(HandleExperimentalMode(*request)); break;
            case RequestKind::kNoiseMode: request->completion.set_value(HandleNoiseMode(request->noise_mode)); break;
            case RequestKind::kNoiseSubmode: request->completion.set_value(HandleNoiseSubmode(*request)); break;
        }
        if (!connected_) CloseConnection();
    }

    void WorkerMain() {
        auto next_reconnect = Clock::now();
        while (!IsStopping()) {
            std::shared_ptr<SessionRequest> request;
            {
                std::unique_lock lock(mutex_);
                if (requests_.empty() && (!requested_connection_.load() || connected_)) {
                    condition_.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping_ || !requests_.empty(); });
                } else if (requests_.empty() && Clock::now() < next_reconnect) {
                    condition_.wait_until(lock, next_reconnect, [&] { return stopping_ || !requests_.empty(); });
                }
                if (stopping_) break;
                if (!requests_.empty()) {
                    request = requests_.front();
                    requests_.pop_front();
                }
            }
            if (request != nullptr) {
                try {
                    HandleRequest(request);
                } catch (const std::exception& error) {
                    Log("Xiaomi persistent RFCOMM: request failed address=" + std::to_string(address_) +
                        " error=" + error.what());
                    CompleteFailure(request);
                    CloseConnection();
                } catch (...) {
                    Log("Xiaomi persistent RFCOMM: request failed address=" + std::to_string(address_) +
                        " error=unknown exception");
                    CompleteFailure(request);
                    CloseConnection();
                }
                if (!connected_) next_reconnect = Clock::now() + reconnect_delay_;
                continue;
            }
            if (!connected_ && requested_connection_.load() && Clock::now() >= next_reconnect) {
                if (!Connect(reconnect_service_)) {
                    next_reconnect = Clock::now() + reconnect_delay_;
                    reconnect_delay_ = std::min(reconnect_delay_ * 2, std::chrono::milliseconds(5000));
                }
                continue;
            }
            if (connected_) {
                const auto status = ReceiveAndDispatch(&background_battery_);
                if (status == ReceiveStatus::kClosed || status == ReceiveStatus::kFailed) {
                    Log("Xiaomi persistent RFCOMM: remote socket closed address=" + std::to_string(address_));
                    CloseConnection();
                    next_reconnect = Clock::now() + reconnect_delay_;
                }
            }
        }
        CloseConnection();
        std::deque<std::shared_ptr<SessionRequest>> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(requests_);
        }
        for (const auto& request : pending) CompleteFailure(request);
    }

    std::uint64_t address_;
    bool debug_enabled_;
    XiaomiDebugLogFn debug_log_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<SessionRequest>> requests_;
    bool stopping_ = false;
    std::atomic_bool requested_connection_{false};
    SOCKET published_socket_ = INVALID_SOCKET;
    XiaomiControlConnection connection_;
    bool connected_ = false;
    std::chrono::milliseconds reconnect_delay_{250};
    ClassicBatteryService reconnect_service_ = ClassicBatteryService::kXiaomiDeviceControl;
    std::vector<std::uint8_t> rx_buffer_;
    BatteryObservation background_battery_;
    std::optional<std::uint8_t> last_mode_;
    std::thread worker_;
};

struct XiaomiRfcommSessionManager::State {
    std::mutex mutex;
    std::condition_variable condition;
    bool stopping = false;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions;
    std::deque<std::shared_ptr<Session>> retired_sessions;
    std::thread cleanup_worker;
};

XiaomiRfcommSessionManager::XiaomiRfcommSessionManager(bool debug_enabled, XiaomiDebugLogFn debug_log)
    : debug_enabled_(debug_enabled), debug_log_(debug_log), state_(std::make_unique<State>()) {
    State* const state = state_.get();
    state->cleanup_worker = std::thread([state]() {
        while (true) {
            std::shared_ptr<Session> session;
            {
                std::unique_lock lock(state->mutex);
                state->condition.wait(lock, [state] {
                    return state->stopping || !state->retired_sessions.empty();
                });
                if (state->retired_sessions.empty()) {
                    if (state->stopping) break;
                    continue;
                }
                session = std::move(state->retired_sessions.front());
                state->retired_sessions.pop_front();
            }
            session->Stop();
        }
    });
}

XiaomiRfcommSessionManager::~XiaomiRfcommSessionManager() { Shutdown(); }

std::shared_ptr<XiaomiRfcommSessionManager::Session> XiaomiRfcommSessionManager::GetOrCreateSession(std::uint64_t address) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) return {};
    auto& session = state_->sessions[address];
    if (session == nullptr) session = std::make_shared<Session>(address, debug_enabled_, debug_log_);
    return session;
}

std::vector<BatteryReading> XiaomiRfcommSessionManager::ReadBattery(std::uint64_t address,
                                                                     ClassicBatteryService preferred_service,
                                                                     const ProviderOperationContext& operation) {
    const auto session = GetOrCreateSession(address);
    return session != nullptr ? session->ReadBattery(preferred_service, operation)
                              : std::vector<BatteryReading>{};
}

bool XiaomiRfcommSessionManager::SetExperimentalNoiseMode(std::uint64_t address, std::uint8_t mode_value,
                                                           std::uint8_t detail_value) {
    const auto session = GetOrCreateSession(address);
    return session != nullptr && session->SetExperimentalNoiseMode(mode_value, detail_value);
}

bool XiaomiRfcommSessionManager::SetNoiseControlMode(std::uint64_t address, NoiseControlMode mode) {
    const auto session = GetOrCreateSession(address);
    return session != nullptr && session->SetNoiseControlMode(mode);
}

bool XiaomiRfcommSessionManager::SetNoiseSubmode(std::uint64_t address, std::uint8_t family, std::uint8_t submode) {
    const auto session = GetOrCreateSession(address);
    return session != nullptr && session->SetNoiseSubmode(family, submode);
}

void XiaomiRfcommSessionManager::NotifyConnectionChanged(std::uint64_t address, bool connected) {
    if (connected) return;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping) return;
        const auto found = state_->sessions.find(address);
        if (found == state_->sessions.end()) return;
        found->second->RequestStop();
        state_->retired_sessions.push_back(std::move(found->second));
        state_->sessions.erase(found);
    }
    state_->condition.notify_one();
}

void XiaomiRfcommSessionManager::Shutdown() {
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->stopping) {
            state_->stopping = true;
            for (auto& [address, session] : state_->sessions) {
                (void)address;
                session->RequestStop();
                state_->retired_sessions.push_back(std::move(session));
            }
            state_->sessions.clear();
        }
    }
    state_->condition.notify_all();
    if (state_->cleanup_worker.joinable()) state_->cleanup_worker.join();
}

}  // namespace battery_monitor
