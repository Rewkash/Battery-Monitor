#pragma once

#include <cstdint>

#include <QString>

namespace battery_monitor {

[[nodiscard]] QString ResolveUpdateDataRoot();
[[nodiscard]] std::uint64_t LoadHighestAcceptedUpdateSequence();
[[nodiscard]] bool SaveHighestAcceptedUpdateSequence(std::uint64_t sequence, QString* error);

}  // namespace battery_monitor
