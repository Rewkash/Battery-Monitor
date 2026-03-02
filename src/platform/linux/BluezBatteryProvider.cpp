#include "platform/linux/BluezBatteryProvider.h"

#include <dbus/dbus.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace battery_monitor {

namespace {

constexpr const char* kBluezService = "org.bluez";
constexpr const char* kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr const char* kObjectManagerInterface = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kDeviceInterface = "org.bluez.Device1";
constexpr const char* kBatteryInterface = "org.bluez.Battery1";

using MessagePtr = std::unique_ptr<DBusMessage, decltype(&dbus_message_unref)>;
using ConnectionPtr = std::unique_ptr<DBusConnection, decltype(&dbus_connection_unref)>;

class ScopedError {
   public:
    ScopedError() {
        dbus_error_init(&error_);
    }

    ~ScopedError() {
        if (dbus_error_is_set(&error_)) {
            dbus_error_free(&error_);
        }
    }

    DBusError* get() {
        return &error_;
    }

    std::string message_or(const std::string& fallback) const {
        if (dbus_error_is_set(&error_) && error_.message != nullptr) {
            return std::string(error_.message);
        }
        return fallback;
    }

   private:
    DBusError error_{};
};

MessagePtr MakeMethodCall(const char* path, const char* interface, const char* method) {
    DBusMessage* raw = dbus_message_new_method_call(kBluezService, path, interface, method);
    return MessagePtr(raw, dbus_message_unref);
}

MessagePtr SendAndBlock(DBusConnection* connection, DBusMessage* message, ScopedError& error) {
    DBusMessage* raw_reply = dbus_connection_send_with_reply_and_block(connection, message, -1, error.get());
    return MessagePtr(raw_reply, dbus_message_unref);
}

std::vector<std::string> ListBatteryDevicePaths(DBusConnection* connection) {
    ScopedError error;
    auto request = MakeMethodCall("/", kObjectManagerInterface, "GetManagedObjects");
    if (!request) {
        throw std::runtime_error("Failed to create D-Bus request for GetManagedObjects.");
    }

    auto reply = SendAndBlock(connection, request.get(), error);
    if (!reply) {
        throw std::runtime_error("GetManagedObjects failed: " + error.message_or("no error details."));
    }

    DBusMessageIter top_iter;
    if (!dbus_message_iter_init(reply.get(), &top_iter) ||
        dbus_message_iter_get_arg_type(&top_iter) != DBUS_TYPE_ARRAY) {
        throw std::runtime_error("Unexpected GetManagedObjects reply shape.");
    }

    DBusMessageIter object_array_iter;
    dbus_message_iter_recurse(&top_iter, &object_array_iter);

    std::vector<std::string> paths;

    while (dbus_message_iter_get_arg_type(&object_array_iter) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&object_array_iter) != DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_next(&object_array_iter);
            continue;
        }

        DBusMessageIter object_entry_iter;
        dbus_message_iter_recurse(&object_array_iter, &object_entry_iter);

        if (dbus_message_iter_get_arg_type(&object_entry_iter) != DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_next(&object_array_iter);
            continue;
        }

        const char* object_path = nullptr;
        dbus_message_iter_get_basic(&object_entry_iter, &object_path);

        if (!dbus_message_iter_next(&object_entry_iter) ||
            dbus_message_iter_get_arg_type(&object_entry_iter) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&object_array_iter);
            continue;
        }

        DBusMessageIter interface_array_iter;
        dbus_message_iter_recurse(&object_entry_iter, &interface_array_iter);

        bool has_device_interface = false;
        bool has_battery_interface = false;

        while (dbus_message_iter_get_arg_type(&interface_array_iter) != DBUS_TYPE_INVALID) {
            if (dbus_message_iter_get_arg_type(&interface_array_iter) != DBUS_TYPE_DICT_ENTRY) {
                dbus_message_iter_next(&interface_array_iter);
                continue;
            }

            DBusMessageIter interface_entry_iter;
            dbus_message_iter_recurse(&interface_array_iter, &interface_entry_iter);
            if (dbus_message_iter_get_arg_type(&interface_entry_iter) == DBUS_TYPE_STRING) {
                const char* interface_name = nullptr;
                dbus_message_iter_get_basic(&interface_entry_iter, &interface_name);

                if (interface_name != nullptr) {
                    if (std::strcmp(interface_name, kDeviceInterface) == 0) {
                        has_device_interface = true;
                    } else if (std::strcmp(interface_name, kBatteryInterface) == 0) {
                        has_battery_interface = true;
                    }
                }
            }

            dbus_message_iter_next(&interface_array_iter);
        }

        if (has_device_interface && has_battery_interface && object_path != nullptr) {
            paths.emplace_back(object_path);
        }

        dbus_message_iter_next(&object_array_iter);
    }

    return paths;
}

bool GetBooleanProperty(DBusConnection* connection, const std::string& path, const char* interface_name,
                        const char* property_name, bool* value) {
    ScopedError error;
    auto request = MakeMethodCall(path.c_str(), kPropertiesInterface, "Get");
    if (!request) {
        return false;
    }

    const char* interface_arg = interface_name;
    const char* property_arg = property_name;
    if (!dbus_message_append_args(request.get(), DBUS_TYPE_STRING, &interface_arg, DBUS_TYPE_STRING, &property_arg,
                                  DBUS_TYPE_INVALID)) {
        return false;
    }

    auto reply = SendAndBlock(connection, request.get(), error);
    if (!reply) {
        return false;
    }

    DBusMessageIter root_iter;
    if (!dbus_message_iter_init(reply.get(), &root_iter) || dbus_message_iter_get_arg_type(&root_iter) != DBUS_TYPE_VARIANT) {
        return false;
    }

    DBusMessageIter variant_iter;
    dbus_message_iter_recurse(&root_iter, &variant_iter);
    if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_BOOLEAN) {
        return false;
    }

    dbus_bool_t dbus_bool = false;
    dbus_message_iter_get_basic(&variant_iter, &dbus_bool);
    *value = dbus_bool == TRUE;
    return true;
}

bool GetByteProperty(DBusConnection* connection, const std::string& path, const char* interface_name,
                     const char* property_name, std::uint8_t* value) {
    ScopedError error;
    auto request = MakeMethodCall(path.c_str(), kPropertiesInterface, "Get");
    if (!request) {
        return false;
    }

    const char* interface_arg = interface_name;
    const char* property_arg = property_name;
    if (!dbus_message_append_args(request.get(), DBUS_TYPE_STRING, &interface_arg, DBUS_TYPE_STRING, &property_arg,
                                  DBUS_TYPE_INVALID)) {
        return false;
    }

    auto reply = SendAndBlock(connection, request.get(), error);
    if (!reply) {
        return false;
    }

    DBusMessageIter root_iter;
    if (!dbus_message_iter_init(reply.get(), &root_iter) || dbus_message_iter_get_arg_type(&root_iter) != DBUS_TYPE_VARIANT) {
        return false;
    }

    DBusMessageIter variant_iter;
    dbus_message_iter_recurse(&root_iter, &variant_iter);
    if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_BYTE) {
        return false;
    }

    unsigned char raw_value = 0;
    dbus_message_iter_get_basic(&variant_iter, &raw_value);
    *value = static_cast<std::uint8_t>(raw_value);
    return true;
}

bool GetStringProperty(DBusConnection* connection, const std::string& path, const char* interface_name,
                       const char* property_name, std::string* value) {
    ScopedError error;
    auto request = MakeMethodCall(path.c_str(), kPropertiesInterface, "Get");
    if (!request) {
        return false;
    }

    const char* interface_arg = interface_name;
    const char* property_arg = property_name;
    if (!dbus_message_append_args(request.get(), DBUS_TYPE_STRING, &interface_arg, DBUS_TYPE_STRING, &property_arg,
                                  DBUS_TYPE_INVALID)) {
        return false;
    }

    auto reply = SendAndBlock(connection, request.get(), error);
    if (!reply) {
        return false;
    }

    DBusMessageIter root_iter;
    if (!dbus_message_iter_init(reply.get(), &root_iter) || dbus_message_iter_get_arg_type(&root_iter) != DBUS_TYPE_VARIANT) {
        return false;
    }

    DBusMessageIter variant_iter;
    dbus_message_iter_recurse(&root_iter, &variant_iter);
    if (dbus_message_iter_get_arg_type(&variant_iter) != DBUS_TYPE_STRING) {
        return false;
    }

    const char* raw_value = nullptr;
    dbus_message_iter_get_basic(&variant_iter, &raw_value);
    if (raw_value == nullptr) {
        return false;
    }

    *value = raw_value;
    return true;
}

}  // namespace

std::vector<DeviceBatteryInfo> BluezBatteryProvider::GetConnectedDevicesBattery() {
    ScopedError error;
    DBusConnection* raw_connection = dbus_bus_get(DBUS_BUS_SYSTEM, error.get());
    if (raw_connection == nullptr) {
        throw std::runtime_error("Failed to connect to system D-Bus: " + error.message_or("no error details."));
    }

    ConnectionPtr connection(raw_connection, dbus_connection_unref);
    const auto paths = ListBatteryDevicePaths(connection.get());

    std::vector<DeviceBatteryInfo> devices;
    devices.reserve(paths.size());

    for (const auto& path : paths) {
        bool is_connected = false;
        if (!GetBooleanProperty(connection.get(), path, kDeviceInterface, "Connected", &is_connected) || !is_connected) {
            continue;
        }

        std::uint8_t battery_percent = 0;
        if (!GetByteProperty(connection.get(), path, kBatteryInterface, "Percentage", &battery_percent)) {
            continue;
        }

        std::string alias = "Unknown";
        GetStringProperty(connection.get(), path, kDeviceInterface, "Alias", &alias);

        DeviceBatteryInfo entry;
        entry.device_id = path;
        entry.device_name = alias;
        entry.battery_component = "main";
        entry.battery_level_percent = battery_percent;
        devices.push_back(std::move(entry));
    }

    return devices;
}

}  // namespace battery_monitor
