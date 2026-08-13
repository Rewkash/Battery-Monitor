#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

class Handle {
   public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    HANDLE release() { HANDLE value = value_; value_ = nullptr; return value; }
   private:
    HANDLE value_;
};

bool ParseUint64(const std::wstring& text, std::uint64_t* value) {
    if (value == nullptr || text.empty()) return false;
    std::uint64_t parsed = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

bool RenameDirectory(const std::filesystem::path& from, const std::filesystem::path& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'\"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            slashes = 0;
        } else {
            quoted.append(slashes, L'\\');
            slashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

bool IsSafeTransaction(const std::filesystem::path& current,
                       const std::filesystem::path& staging,
                       const std::filesystem::path& backup,
                       const std::wstring& executable,
                       const std::wstring& token) {
    if (!current.is_absolute() || !staging.is_absolute() || !backup.is_absolute() ||
        token.size() != 32 || token.find_first_not_of(L"0123456789abcdef") != std::wstring::npos ||
        (executable != L"battery-monitor.exe" && executable != L"battery-monitor-cli.exe")) {
        return false;
    }
    std::error_code error;
    const auto parent = std::filesystem::weakly_canonical(current.parent_path(), error);
    if (error || parent.empty() || std::filesystem::weakly_canonical(staging.parent_path(), error) != parent || error ||
        std::filesystem::weakly_canonical(backup.parent_path(), error) != parent || error) {
        return false;
    }
    return staging.filename() == L".battery-monitor-stage-" + token &&
           backup.filename() == L".battery-monitor-backup-" + token &&
           std::filesystem::is_directory(current, error) && !error &&
           std::filesystem::is_directory(staging, error) && !error &&
           !std::filesystem::exists(backup, error) && !error;
}

bool StartProcess(const std::filesystem::path& executable,
                   const std::filesystem::path& working_directory,
                   HANDLE health_event,
                   const std::wstring& expected_version,
                   PROCESS_INFORMATION* process) {
    std::wstring command = QuoteArgument(executable.wstring());
    BOOL inherit_handles = FALSE;
    if (health_event != nullptr) {
        command += L" --update-health-handle " +
                    std::to_wstring(reinterpret_cast<std::uintptr_t>(health_event)) +
                    L" --update-health-version " + QuoteArgument(expected_version);
        inherit_handles = TRUE;
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    std::vector<std::byte> attribute_storage;
    DWORD creation_flags = 0;
    if (health_event != nullptr) {
        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        attribute_storage.resize(attribute_size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size) ||
            !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       &health_event, sizeof(health_event), nullptr, nullptr)) {
            if (startup.lpAttributeList != nullptr) DeleteProcThreadAttributeList(startup.lpAttributeList);
            return false;
        }
        creation_flags = EXTENDED_STARTUPINFO_PRESENT;
    }
    const bool started = CreateProcessW(executable.c_str(), buffer.data(), nullptr, nullptr, inherit_handles,
                                        creation_flags, nullptr, working_directory.c_str(),
                                        &startup.StartupInfo, process) != FALSE;
    if (startup.lpAttributeList != nullptr) DeleteProcThreadAttributeList(startup.lpAttributeList);
    return started;
}

DWORD RunProcessAndWait(const std::filesystem::path& executable,
                        const std::wstring& arguments,
                        const std::filesystem::path& working_directory) {
    std::wstring command = QuoteArgument(executable.wstring()) + L" " + arguments;
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        working_directory.c_str(), &startup, &process)) {
        return ERROR_PROCESS_ABORTED;
    }
    Handle process_handle(process.hProcess);
    Handle thread_handle(process.hThread);
    if (WaitForSingleObject(process_handle.get(), INFINITE) != WAIT_OBJECT_0) return ERROR_PROCESS_ABORTED;
    DWORD exit_code = ERROR_PROCESS_ABORTED;
    return GetExitCodeProcess(process_handle.get(), &exit_code) ? exit_code : ERROR_PROCESS_ABORTED;
}

std::uint64_t LoadSequence(const std::filesystem::path& state) {
    std::ifstream input(state, std::ios::binary);
    if (!input) return 0;
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string key = "\"highestSequence\"";
    std::size_t position = content.find(key);
    if (position == std::string::npos) return 0;
    position = content.find(':', position + key.size());
    if (position == std::string::npos) return 0;
    position = content.find_first_of("0123456789", position + 1);
    if (position == std::string::npos) return 0;
    std::uint64_t parsed = 0;
    while (position < content.size() && content[position] >= '0' && content[position] <= '9') {
        const std::uint64_t digit = static_cast<std::uint64_t>(content[position++] - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return 0;
        parsed = parsed * 10 + digit;
    }
    return parsed;
}

bool CommitSequence(const std::filesystem::path& state, std::uint64_t sequence) {
    std::error_code error;
    std::filesystem::create_directories(state.parent_path(), error);
    if (error) return false;
    const std::filesystem::path temporary = state.wstring() + L".new";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << "{\"schemaVersion\":1,\"highestSequence\":\""
               << std::max(sequence, LoadSequence(state)) << "\"}";
        if (!output) return false;
    }
    if (!MoveFileExW(temporary.c_str(), state.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::wstring MutexName(const std::filesystem::path& current) {
    std::uint64_t hash = 1469598103934665603ULL;
    std::wstring normalized = current.lexically_normal().wstring();
    for (wchar_t character : normalized) {
        character = static_cast<wchar_t>(towlower(character));
        hash ^= static_cast<std::uint16_t>(character);
        hash *= 1099511628211ULL;
    }
    std::wostringstream name;
    name << L"Local\\BatteryMonitorUpdate-" << std::hex << hash;
    return name.str();
}

int ApplyMsi(int argc, wchar_t** argv) {
    if (argc != 10) return 2;
    const std::filesystem::path package = argv[2];
    const std::filesystem::path current = argv[3];
    const std::wstring executable = argv[4];
    const std::wstring token = argv[6];
    const std::filesystem::path state = argv[8];
    const std::wstring expected_version = argv[9];
    std::uint64_t old_pid = 0;
    std::uint64_t sequence = 0;
    std::error_code error;
    if (!ParseUint64(argv[5], &old_pid) || old_pid > MAXDWORD ||
        !ParseUint64(argv[7], &sequence) || sequence == 0 || !current.is_absolute() ||
        !package.is_absolute() || !state.is_absolute() ||
        (executable != L"battery-monitor.exe" && executable != L"battery-monitor-cli.exe") ||
        token.size() != 32 || token.find_first_not_of(L"0123456789abcdef") != std::wstring::npos ||
        package.filename() != token + L".msi" || package.extension() != L".msi" ||
        !std::filesystem::is_regular_file(package, error) || error) {
        return 3;
    }
    const auto package_parent = std::filesystem::weakly_canonical(package.parent_path(), error);
    if (error) return 3;
    const auto expected_parent = std::filesystem::weakly_canonical(state.parent_path() / L"downloads", error);
    if (error || package_parent != expected_parent) return 3;

    Handle mutex(CreateMutexW(nullptr, FALSE, MutexName(current).c_str()));
    if (mutex.get() == nullptr || WaitForSingleObject(mutex.get(), 0) != WAIT_OBJECT_0 ||
        sequence <= LoadSequence(state)) return 9;
    Handle old_process(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(old_pid)));
    if (old_process.get() != nullptr && WaitForSingleObject(old_process.get(), 60000) != WAIT_OBJECT_0) return 4;

    std::array<wchar_t, MAX_PATH> system_directory{};
    if (GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size())) == 0) return 5;
    const std::filesystem::path msiexec = std::filesystem::path(system_directory.data()) / L"msiexec.exe";
    const std::filesystem::path log = state.parent_path() / (L"msi-" + token + L".log");
    const std::wstring arguments = L"/i " + QuoteArgument(package.wstring()) +
                                   L" /quiet /norestart /L*v " + QuoteArgument(log.wstring());
    const DWORD install_result = RunProcessAndWait(msiexec, arguments, package.parent_path());
    if (install_result != ERROR_SUCCESS && install_result != ERROR_SUCCESS_REBOOT_REQUIRED) {
        PROCESS_INFORMATION restored{};
        if (std::filesystem::is_regular_file(current / executable, error) &&
            StartProcess(current / executable, current, nullptr, std::wstring(), &restored)) {
            CloseHandle(restored.hThread);
            CloseHandle(restored.hProcess);
        }
        return 6;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle health_event(CreateEventW(&security, TRUE, FALSE, nullptr));
    PROCESS_INFORMATION child{};
    bool healthy = health_event.get() != nullptr &&
                   StartProcess(current / executable, current, health_event.get(), expected_version, &child);
    Handle child_process(healthy ? child.hProcess : nullptr);
    Handle child_thread(healthy ? child.hThread : nullptr);
    if (healthy) {
        HANDLE waits[] = {health_event.get(), child_process.get()};
        healthy = WaitForMultipleObjects(2, waits, FALSE, 60000) == WAIT_OBJECT_0;
    }
    return healthy && CommitSequence(state, sequence) ? 0 : 8;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--apply-msi") return ApplyMsi(argc, argv);
    if (argc != 11 || std::wstring_view(argv[1]) != L"--apply") return 2;
    const std::filesystem::path current = argv[2];
    const std::filesystem::path staging = argv[3];
    const std::filesystem::path backup = argv[4];
    const std::wstring executable = argv[5];
    const std::wstring token = argv[7];
    const std::filesystem::path state = argv[9];
    const std::wstring expected_version = argv[10];
    std::uint64_t old_pid = 0;
    std::uint64_t sequence = 0;
    if (!ParseUint64(argv[6], &old_pid) || old_pid > MAXDWORD ||
        !ParseUint64(argv[8], &sequence) || sequence == 0 ||
        !IsSafeTransaction(current, staging, backup, executable, token)) return 3;

    Handle mutex(CreateMutexW(nullptr, FALSE, MutexName(current).c_str()));
    if (mutex.get() == nullptr || WaitForSingleObject(mutex.get(), 0) != WAIT_OBJECT_0 ||
        sequence <= LoadSequence(state)) return 9;

    Handle old_process(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(old_pid)));
    if (old_process.get() != nullptr && WaitForSingleObject(old_process.get(), 60000) != WAIT_OBJECT_0) return 4;
    if (!RenameDirectory(current, backup)) return 5;
    if (!RenameDirectory(staging, current)) {
        RenameDirectory(backup, current);
        return 6;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle health_event(CreateEventW(&security, TRUE, FALSE, nullptr));
    PROCESS_INFORMATION child{};
    const auto new_executable = current / executable;
    bool healthy = health_event.get() != nullptr &&
                    StartProcess(new_executable, current, health_event.get(), expected_version, &child);
    Handle child_process(healthy ? child.hProcess : nullptr);
    Handle child_thread(healthy ? child.hThread : nullptr);
    if (healthy) {
        HANDLE waits[] = {health_event.get(), child_process.get()};
        healthy = WaitForMultipleObjects(2, waits, FALSE, 60000) == WAIT_OBJECT_0;
    }
    if (healthy && CommitSequence(state, sequence)) return 0;

    if (child_process.get() != nullptr && WaitForSingleObject(child_process.get(), 0) == WAIT_TIMEOUT) {
        TerminateProcess(child_process.get(), 1);
        WaitForSingleObject(child_process.get(), 10000);
    }
    const auto failed = current.parent_path() / (L".battery-monitor-failed-" + token);
    if (!RenameDirectory(current, failed) || !RenameDirectory(backup, current)) return 7;
    PROCESS_INFORMATION restored{};
    if (StartProcess(current / executable, current, nullptr, std::wstring(), &restored)) {
        CloseHandle(restored.hThread);
        CloseHandle(restored.hProcess);
    }
    return 8;
}
