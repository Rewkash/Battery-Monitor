#include "platform/windows/BluetoothPnpUtils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <winrt/base.h>

namespace battery_monitor {

namespace {

std::string ToUtf8(const std::wstring& value) {
    return winrt::to_string(winrt::hstring(value));
}

}  // namespace

std::vector<std::wstring> FindBthEnumInstanceIdsByAddress(std::uint64_t address) {
    std::wstringstream prefix_builder;
    prefix_builder << L"BTHENUM\\DEV_" << std::uppercase << std::hex
                   << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring expected_prefix = prefix_builder.str();

    std::wstringstream filter_builder;
    filter_builder << expected_prefix << L"\\*";
    const std::wstring filter = filter_builder.str();

    ULONG size = 0;
    const auto size_result = CM_Get_Device_ID_List_SizeW(&size, filter.c_str(), CM_GETIDLIST_FILTER_NONE);
    if (size_result != CR_SUCCESS || size <= 1U) {
        return {};
    }

    std::vector<wchar_t> buffer(size);
    const auto list_result = CM_Get_Device_ID_ListW(filter.c_str(), buffer.data(), size, CM_GETIDLIST_FILTER_NONE);
    if (list_result != CR_SUCCESS || buffer.empty() || buffer.front() == L'\0') {
        return {};
    }

    std::vector<std::wstring> instance_ids;
    std::size_t cursor = 0;
    while (cursor < buffer.size() && buffer[cursor] != L'\0') {
        const wchar_t* current = buffer.data() + static_cast<std::ptrdiff_t>(cursor);
        const std::size_t length = std::wcslen(current);
        if (length == 0U) {
            break;
        }

        std::wstring instance_id(current, length);
        std::wstring normalized = instance_id;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towupper(value));
        });
        if (normalized.rfind(expected_prefix, 0) == 0) {
            instance_ids.push_back(std::move(instance_id));
        }
        cursor += length + 1U;
    }

    return instance_ids;
}

std::vector<std::wstring> FindBthEnumServiceInstanceIdsByAddress(std::uint64_t address,
                                                                 const std::wstring& service_uuid_upper) {
    if (address <= 0xFFFFULL || service_uuid_upper.empty()) {
        return {};
    }

    std::wstringstream address_builder;
    address_builder << std::uppercase << std::hex << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring address_token = address_builder.str();

    std::wstringstream filter_builder;
    filter_builder << L"BTHENUM\\*" << address_token << L"*";
    const std::wstring filter = filter_builder.str();

    ULONG size = 0;
    const auto size_result = CM_Get_Device_ID_List_SizeW(&size, filter.c_str(), CM_GETIDLIST_FILTER_NONE);
    if (size_result != CR_SUCCESS || size <= 1U) {
        return {};
    }

    std::vector<wchar_t> buffer(size);
    const auto list_result = CM_Get_Device_ID_ListW(filter.c_str(), buffer.data(), size, CM_GETIDLIST_FILTER_NONE);
    if (list_result != CR_SUCCESS || buffer.empty() || buffer.front() == L'\0') {
        return {};
    }

    const std::wstring expected_service_prefix = L"BTHENUM\\{" + service_uuid_upper + L"}";
    std::vector<std::wstring> instance_ids;
    std::size_t cursor = 0;
    while (cursor < buffer.size() && buffer[cursor] != L'\0') {
        const wchar_t* current = buffer.data() + static_cast<std::ptrdiff_t>(cursor);
        const std::size_t length = std::wcslen(current);
        if (length == 0U) {
            break;
        }

        std::wstring instance_id(current, length);
        std::wstring normalized = instance_id;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towupper(value));
        });

        if (normalized.rfind(expected_service_prefix, 0) == 0 &&
            normalized.find(address_token) != std::wstring::npos) {
            instance_ids.push_back(std::move(instance_id));
        }
        cursor += length + 1U;
    }

    return instance_ids;
}

std::vector<std::wstring> FindBthLeInstanceIdsByAddress(std::uint64_t address) {
    std::wstringstream prefix_builder;
    prefix_builder << L"BTHLE\\DEV_" << std::uppercase << std::hex
                   << std::setw(12) << std::setfill(L'0') << address;
    const std::wstring expected_prefix = prefix_builder.str();

    std::wstringstream filter_builder;
    filter_builder << expected_prefix << L"\\*";
    const std::wstring filter = filter_builder.str();

    ULONG size = 0;
    const auto size_result = CM_Get_Device_ID_List_SizeW(&size, filter.c_str(), CM_GETIDLIST_FILTER_NONE);
    if (size_result != CR_SUCCESS || size <= 1U) {
        return {};
    }

    std::vector<wchar_t> buffer(size);
    const auto list_result = CM_Get_Device_ID_ListW(filter.c_str(), buffer.data(), size, CM_GETIDLIST_FILTER_NONE);
    if (list_result != CR_SUCCESS || buffer.empty() || buffer.front() == L'\0') {
        return {};
    }

    std::vector<std::wstring> instance_ids;
    std::size_t cursor = 0;
    while (cursor < buffer.size() && buffer[cursor] != L'\0') {
        const wchar_t* current = buffer.data() + static_cast<std::ptrdiff_t>(cursor);
        const std::size_t length = std::wcslen(current);
        if (length == 0U) {
            break;
        }

        std::wstring instance_id(current, length);
        std::wstring normalized = instance_id;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towupper(value));
        });
        if (normalized.rfind(expected_prefix, 0) == 0) {
            instance_ids.push_back(std::move(instance_id));
        }
        cursor += length + 1U;
    }

    return instance_ids;
}

std::optional<std::vector<std::uint8_t>> ReadDevNodePropertyRaw(DEVINST dev_inst,
                                                                const DEVPROPKEY& property_key,
                                                                DEVPROPTYPE* property_type) {
    if (property_type == nullptr) {
        return std::nullopt;
    }

    *property_type = DEVPROP_TYPE_EMPTY;
    ULONG size = 0;
    const auto query_result =
        CM_Get_DevNode_PropertyW(dev_inst, &property_key, property_type, nullptr, &size, 0);
    if (query_result != CR_SUCCESS && query_result != CR_BUFFER_SMALL) {
        return std::nullopt;
    }
    if (size == 0U) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> raw_data(size);
    const auto property_result = CM_Get_DevNode_PropertyW(
        dev_inst, &property_key, property_type, reinterpret_cast<PBYTE>(raw_data.data()), &size, 0);
    if (property_result != CR_SUCCESS) {
        return std::nullopt;
    }
    raw_data.resize(size);
    return raw_data;
}

std::optional<std::uint32_t> ReadDevNodeUInt32Property(DEVINST dev_inst, const DEVPROPKEY& property_key) {
    DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
    const auto raw_data = ReadDevNodePropertyRaw(dev_inst, property_key, &property_type);
    if (!raw_data.has_value()) {
        return std::nullopt;
    }

    if (property_type == DEVPROP_TYPE_UINT32 && raw_data->size() >= sizeof(std::uint32_t)) {
        std::uint32_t value = 0;
        std::memcpy(&value, raw_data->data(), sizeof(value));
        return value;
    }
    if (property_type == DEVPROP_TYPE_INT32 && raw_data->size() >= sizeof(std::int32_t)) {
        std::int32_t value = 0;
        std::memcpy(&value, raw_data->data(), sizeof(value));
        if (value >= 0) {
            return static_cast<std::uint32_t>(value);
        }
    }
    return std::nullopt;
}

std::vector<std::string> ReadDevNodeStringListProperty(DEVINST dev_inst, const DEVPROPKEY& property_key) {
    DEVPROPTYPE property_type = DEVPROP_TYPE_EMPTY;
    const auto raw_data = ReadDevNodePropertyRaw(dev_inst, property_key, &property_type);
    if (!raw_data.has_value()) {
        return {};
    }

    std::vector<std::string> values;
    if ((property_type & DEVPROP_MASK_TYPE) == DEVPROP_TYPE_STRING && raw_data->size() >= sizeof(wchar_t)) {
        const wchar_t* cursor = reinterpret_cast<const wchar_t*>(raw_data->data());
        const wchar_t* end = reinterpret_cast<const wchar_t*>(raw_data->data() + raw_data->size());
        while (cursor < end && *cursor != L'\0') {
            const std::size_t length = std::wcslen(cursor);
            if (length == 0U) {
                break;
            }
            values.push_back(ToUtf8(std::wstring(cursor, length)));
            cursor += length + 1U;
        }
    }
    return values;
}

}  // namespace battery_monitor
