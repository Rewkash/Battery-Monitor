#pragma once

// Shared, lifetime-safe WinRT async wait helper.
//
// Several Windows readers need to block on a winrt IAsyncOperation with a
// deadline while honouring ProviderOperationContext cancellation. That exact
// loop was duplicated across the readers under src/platform/windows/; new or
// refactored readers should use this single copy instead of adding another.

#include <chrono>
#include <optional>
#include <thread>

#include <winrt/Windows.Foundation.h>

#include "core/BatteryTypes.h"

namespace battery_monitor {

// Waits for `operation` for at most `timeout` (further clamped by the
// operation's remaining budget). Cancels and returns nullopt on timeout,
// cancellation, or WinRT error. Never blocks past the caller's deadline.
template <typename TResult>
std::optional<TResult> WaitForAsyncResult(winrt::Windows::Foundation::IAsyncOperation<TResult> operation,
                                          std::chrono::milliseconds timeout,
                                          const ProviderOperationContext& operation_context = {}) {
    try {
        const auto deadline = std::chrono::steady_clock::now() + operation_context.Remaining(timeout);

        while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started &&
               !operation_context.IsCancelled() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
            operation.Cancel();
            return std::nullopt;
        }

        if (operation.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
            return std::nullopt;
        }

        return operation.GetResults();
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

}  // namespace battery_monitor
