#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include "wire.hpp"
#include "session.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE v = nullptr) : value(v) {}
    Handle(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
std::runtime_error error(const char* text) { return std::runtime_error(std::string(text) + " (Windows " + std::to_string(GetLastError()) + ")"); }
std::vector<DWORD> dota_processes() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) throw error("Process enumeration failed");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry); std::vector<DWORD> result;
    if (Process32FirstW(snapshot.value, &entry)) do {
        if (!_wcsicmp(entry.szExeFile, L"dota2.exe")) result.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot.value, &entry));
    return result;
}
std::uintptr_t remote_module(DWORD pid, const std::wstring& name) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) throw error("Module enumeration failed");
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot.value, &entry)) do {
        if (!_wcsicmp(entry.szModule, name.c_str())) return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snapshot.value, &entry));
    return 0;
}
std::uintptr_t remote_load_library(DWORD pid) {
    auto address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE actual = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &actual)) throw error("LoadLibraryW unavailable");
    wchar_t path[32768]; auto size = GetModuleFileNameW(actual, path, 32768);
    if (!size || size == 32768) throw error("System module path failed");
    auto base = remote_module(pid, fs::path(std::wstring(path, size)).filename().wstring());
    if (!base) throw std::runtime_error("Required system module missing in Dota");
    return base + reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(actual);
}
DWORD call(HANDLE process, std::uintptr_t function, void* argument, bool* completed = nullptr, DWORD timeout = 45000) {
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(function), argument, 0, nullptr));
    if (!thread) throw error("Dota rejected module initialization");
    auto result = WaitForSingleObject(thread.value, timeout);
    if (result != WAIT_OBJECT_0) throw std::runtime_error("Initialization timed out; inspect status.json. Do not retry in the same game session.");
    if (completed) *completed = true;
    DWORD exit = 0;
    if (!GetExitCodeThread(thread.value, &exit)) throw error("Cannot read initialization result");
    return exit;
}
void verify_source(const fs::path& directory, const fs::path& game) {
    std::ifstream source(directory / "catalog.source", std::ios::binary);
    wire::Bytes signature(24);
    if (!source.read(reinterpret_cast<char*>(signature.data()), signature.size()) ||
        std::string(signature.begin(), signature.begin() + 8) != "OVISRC01")
        throw std::runtime_error("Catalogue fingerprint missing. Run start.cmd.");
    WIN32_FILE_ATTRIBUTE_DATA info{};
    auto pak = game / "dota/pak01_dir.vpk";
    if (!GetFileAttributesExW(pak.c_str(), GetFileExInfoStandard, &info)) throw error("Dota VPK missing");
    auto size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    auto time = (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    if (size != wire::little(wire::View(signature).subspan(8, 8)) || time != wire::little(wire::View(signature).subspan(16, 8)))
        throw std::runtime_error("Dota changed after catalogue generation. Run start.cmd to rebuild.");
}
int wmain(int argc, wchar_t** argv) {
    try {
        wchar_t own_path[32768]; auto size = GetModuleFileNameW(nullptr, own_path, 32768);
        if (!size || size == 32768) throw error("Executable path unavailable");
        auto directory = fs::path(std::wstring(own_path, size)).parent_path();
        if (argc == 2 && std::wstring(argv[1]) == L"--status") {
            auto state = fs::exists(directory / "installed.marker") ? directory.parent_path() / "state" : directory;
            std::ifstream in(state / "status.json");
            if (in) std::cout << in.rdbuf(); else std::cout << "No session status.\n";
            return 0;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--help") {
            std::cout << "InventoryChanger.exe [--pid PID | --status | --check]\n"
                "Adds a local virtual inventory through an experimental GC client adapter.\n"
                "The current Dota client and in-match rendering have NOT been verified.\n"
                "Close Dota to unload. This does not grant Steam ownership.\n"; return 0;
        }
        const bool guarding=argc>=3&&std::wstring(argv[1])==L"--guard";
        const bool check_only = argc == 2 && std::wstring(argv[1]) == L"--check";
        auto pids = dota_processes(); DWORD pid = 0;
        if ((argc == 3 && std::wstring(argv[1]) == L"--pid") || (guarding&&(argc==3||argc==4))) {
            std::wstring value(argv[2]); std::size_t end; auto raw = std::stoull(value, &end);
            if (end != value.size() || !raw || raw > UINT32_MAX) throw std::runtime_error("Invalid PID");
            pid = static_cast<DWORD>(raw);
            if (std::find(pids.begin(), pids.end(), pid) == pids.end()) throw std::runtime_error("PID is not Dota 2");
        } else if (argc != 1 && !check_only) throw std::runtime_error("Unknown arguments. Use --help.");
        else {
            if (pids.empty()) throw std::runtime_error("Start Dota 2 first, then run start.cmd.");
            if (pids.size() != 1) throw std::runtime_error("Multiple Dota clients. Select one with --pid PID.");
            pid = pids.front();
        }
        auto access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE;
        if (!check_only&&!guarding) access |= PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD;
        Handle process(OpenProcess(access, FALSE, pid));
        if (!process) throw error("Cannot open Dota process");
        wchar_t game_path[32768]; DWORD length = 32768;
        if (!QueryFullProcessImageNameW(process.value, 0, game_path, &length)) throw error("Dota path unavailable");
        fs::path executable(std::wstring(game_path, length));
        auto game = executable.parent_path().parent_path().parent_path();
        if (_wcsicmp(executable.filename().c_str(), L"dota2.exe") ||
            _wcsicmp(executable.parent_path().filename().c_str(), L"win64") ||
            !fs::exists(game / "dota/gameinfo.gi")) throw std::runtime_error("Only an installed Windows x64 Dota 2 client is supported");
        BOOL wow = FALSE;
        if (!IsWow64Process(process.value, &wow) || wow) throw std::runtime_error("Dota architecture is not x64");
        if(guarding){
            auto cache=game/L"dota"/(L"cache_"+std::to_wstring(session::active_account())+L"_1.soc");
            session::CacheGuard guard(cache,L"C:\\Dotaunlock\\state\\backups");
            if(argc==4){Handle ready(OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[3]));if(!ready||!SetEvent(ready.value))throw error("Guard handshake failed");}
            std::cout<<"Inventory cache protected until Dota exits.\n";WaitForSingleObject(process.value,INFINITE);return 0;
        }
        verify_source(directory, game);
        auto dll = directory / "InventoryClient.dll";
        if (!fs::is_regular_file(dll) || !fs::is_regular_file(directory / "catalog.bin")) throw std::runtime_error("Client DLL or catalogue missing");
        if (remote_module(pid, L"InventoryClient.dll")) throw std::runtime_error("Module already loaded. Inspect status.json or restart Dota before retrying.");
        auto inspect = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!inspect) throw error("Cannot inspect InventoryClient.dll");
        auto entry = GetProcAddress(inspect, "InventoryStart");
        auto entry_offset = reinterpret_cast<std::uintptr_t>(entry) - reinterpret_cast<std::uintptr_t>(inspect);
        FreeLibrary(inspect);
        if (!entry) throw std::runtime_error("InventoryStart export missing");
        if (check_only) {
            std::cout << "PASS: Dota x64 process, current catalogue fingerprint, and native DLL export verified. No game process memory modified.\n";
            return 0;
        }
        auto event_name=L"Local\\DotaunlockCacheReady-"+std::to_wstring(pid)+L"-"+std::to_wstring(GetCurrentProcessId());
        Handle ready(CreateEventW(nullptr,TRUE,FALSE,event_name.c_str()));if(!ready)throw error("Guard event creation failed");
        std::wstring command=L"\""+std::wstring(own_path)+L"\" --guard "+std::to_wstring(pid)+L" "+event_name;
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
        if(!CreateProcessW(own_path,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&child))throw error("Cache guard start failed");
        Handle guard_process(child.hProcess),guard_thread(child.hThread);HANDLE waits[]={ready.value,guard_process.value};
        if(WaitForMultipleObjects(2,waits,FALSE,15000)!=WAIT_OBJECT_0)throw std::runtime_error("Cache guard unavailable; inventory was not attached");
        const auto path_text = dll.wstring(); SIZE_T bytes = (path_text.size() + 1) * sizeof(wchar_t);
        auto remote = VirtualAllocEx(process.value, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!remote) throw error("Cannot allocate module path");
        SIZE_T written = 0; bool completed = false;
        try {
            if (!WriteProcessMemory(process.value, remote, path_text.c_str(), bytes, &written) || written != bytes) {
                completed = true; throw error("Cannot write module path");
            }
            call(process.value, remote_load_library(pid), remote, &completed);
        } catch (...) { if (completed) VirtualFreeEx(process.value, remote, 0, MEM_RELEASE); throw; }
        VirtualFreeEx(process.value, remote, 0, MEM_RELEASE);
        auto remote_base = remote_module(pid, L"InventoryClient.dll");
        if (!remote_base) throw std::runtime_error("Dota did not load the module");
        auto result = call(process.value, remote_base + entry_offset, nullptr, nullptr, 330000);
        if (result) throw std::runtime_error("Client adapter failed. Read inventory.log.");
        std::cout << "Client adapter installed. Cache delivery is recorded in status.json.\n"
            "This is not confirmation of in-game rendering. Close Dota to unload.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "ERROR: " << e.what() << '\n'; return 1; }
}
