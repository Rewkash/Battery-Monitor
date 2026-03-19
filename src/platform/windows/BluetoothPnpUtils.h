#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>
#include <cfgmgr32.h>
#include <devpropdef.h>

namespace battery_monitor {

std::vector<std::wstring> FindBthEnumInstanceIdsByAddress(std::uint64_t address);
std::vector<std::wstring> FindBthEnumServiceInstanceIdsByAddress(std::uint64_t address,
                                                                 const std::wstring& service_uuid_upper);
std::vector<std::wstring> FindBthLeInstanceIdsByAddress(std::uint64_t address);
std::optional<std::vector<std::uint8_t>> ReadDevNodePropertyRaw(DEVINST dev_inst,
                                                                const DEVPROPKEY& property_key,
                                                                DEVPROPTYPE* property_type);
std::optional<std::uint32_t> ReadDevNodeUInt32Property(DEVINST dev_inst, const DEVPROPKEY& property_key);
std::vector<std::string> ReadDevNodeStringListProperty(DEVINST dev_inst, const DEVPROPKEY& property_key);

}  // namespace battery_monitor
