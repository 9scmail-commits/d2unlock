#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "inventory.hpp"
#include "tiers.hpp"
#include "callbacks.hpp"
#include "emotes.hpp"
#include "roster.hpp"
#include "security.hpp"
#include "skins.hpp"
#include "livemount.hpp"
#include "gsi.hpp"
#include "hud.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace {
using namespace wire;
namespace fs = std::filesystem;
using Send = int (*)(void*, std::uint32_t, const void*, std::uint32_t);
using Available = bool (*)(void*, std::uint32_t*);
using Retrieve = int (*)(void*, std::uint32_t*, void*, std::uint32_t, std::uint32_t*);
HMODULE module;
void* coordinator = nullptr;
void** original_table = nullptr;
void* replacement_table[3]{};
Send original_send = nullptr;
Available original_available = nullptr;
Retrieve original_retrieve = nullptr;
std::unique_ptr<inventory::Inventory> local;
std::unique_ptr<tiers::Ranks> ranks;
std::unique_ptr<matchdata::Roster> roster;
Bytes emoticon_mask; unsigned local_account = 0;
std::recursive_mutex lock;
fs::path directory, state_directory;
std::mutex io_lock;
std::condition_variable io_ready;
std::map<std::wstring, std::string> io_files;
std::deque<std::string> io_logs;
struct Pending { std::uint32_t type; Bytes bytes; bool virtual_cache = false; ULONGLONG queued_at = 0; bool local_operation = false; bool tier_update = false; };
std::deque<Pending> pending;
std::size_t pending_bytes = 0;
bool stopping = false;
callbacks::Hooks callback_hooks;
int callback_pipe = 0, callback_user = 0;
std::uint64_t queue_generation = 0;
thread_local std::uint64_t notified_generation = 0;
thread_local ULONGLONG last_notification = 0;
thread_local bool synthetic_callback = false;
thread_local bool announced_this_cycle = false;
thread_local std::uint32_t callback_size = 0;
bool adapter_installed = false;
std::atomic<bool> skins_dirty{false};
bool mount_pending = false, mount_deferred = false; std::string mount_next, mount_current;
livemount::FileSystem filesystem; bool filesystem_checked = false;
gsi::Listener game_state;

void log(const std::string& text) noexcept {
    try { std::lock_guard guard(io_lock); if (io_logs.size() < 256) io_logs.push_back(text); io_ready.notify_one(); } catch (...) {}
}
void persist(const wchar_t* name, std::string text) {
    std::lock_guard guard(io_lock); io_files[name] = std::move(text); io_ready.notify_one();
}
void writer() {
    for (;;) {
        std::map<std::wstring, std::string> files; std::deque<std::string> logs;
        { std::unique_lock guard(io_lock); io_ready.wait(guard, [] { return !io_files.empty() || !io_logs.empty(); });
          files.swap(io_files); logs.swap(io_logs); }
        for (auto& [name, content] : files) try {
            security::write(state_directory / name, std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(content.data()), content.size()));
        } catch (...) {}
        if (!logs.empty()) try {
            security::plain_path(state_directory / "inventory.log");
            std::ofstream out(state_directory / "inventory.log", std::ios::app);
            for (const auto& line : logs) {
                SYSTEMTIME now; GetLocalTime(&now); char stamp[32];
                std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
                out << stamp << line << '\n';
            }
        } catch (...) {}
    }
}
void status(const char* stage) noexcept {
    try { persist(L"status.json", std::string("{\"stage\":\"") + stage + "\",\"pid\":" + std::to_string(GetCurrentProcessId())
        + ",\"definitions\":" + std::to_string(local ? local->count() : 0) + "}\n"); } catch (...) {}
}
Bytes read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("missing file: " + path.filename().string());
    auto size = in.tellg();
    if (size < 0 || size > 16 * 1024 * 1024) throw std::runtime_error("file size limit");
    Bytes bytes(static_cast<std::size_t>(size)); in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw std::runtime_error("file read failed");
    return bytes;
}
void enqueue(Pending value) {
    if (pending.size() >= 128 || pending_bytes + value.bytes.size() > 64 * 1024 * 1024)
        throw std::runtime_error("local message queue full");
    value.queued_at = GetTickCount64();
    pending_bytes += value.bytes.size(); pending.push_back(std::move(value)); ++queue_generation;
}

bool hook_callback(int pipe, callbacks::Message* message) noexcept {
    if (pipe != callback_pipe || !message) return callback_hooks.get(pipe, message);
    if (synthetic_callback) return false;
    if (callback_hooks.get(pipe, message)) return true;
    try {
        std::lock_guard guard(lock);
        if (announced_this_cycle) { announced_this_cycle = false; return false; }
        if (pending.empty()) return false;
        const auto now = GetTickCount64();
        if (queue_generation == notified_generation && now - last_notification < 16) return false;
        callback_size = static_cast<std::uint32_t>(pending.front().bytes.size());
        *message = {callback_user, 1701, reinterpret_cast<unsigned char*>(&callback_size), sizeof(callback_size)};
        notified_generation = queue_generation; last_notification = now;
        synthetic_callback = true; announced_this_cycle = true; return true;
    } catch (...) { return false; }
}
void hook_free_callback(int pipe) noexcept {
    if (pipe == callback_pipe && synthetic_callback) { synthetic_callback = false; return; }
    callback_hooks.release(pipe);
}

std::uint64_t strategy_lobby = 0; ULONGLONG strategy_since = 0; int logged_phase = -2;
std::optional<Packet> match_view() {
    if (!local || !roster || stopping) return std::nullopt;
    unsigned hide = 0;
    if (roster->in_match()) {
        const auto now = GetTickCount64();
        const bool live = game_state.fresh(now);
        const int phase = live ? game_state.phase() : static_cast<int>(roster->game_phase());
        if (phase != logged_phase) { logged_phase = phase; log("Game phase " + std::to_string(phase) + (live ? " (game state integration)" : " (lobby)")); }
        if (!live && phase == 3 && strategy_lobby != roster->lobby_id()) { strategy_lobby = roster->lobby_id(); strategy_since = now; }
        const bool picking = phase == 2 || phase == 12 || (phase == 3 && (live || now - strategy_since < 25000));
        const unsigned hero = live && game_state.hero() ? game_state.hero() : roster->own_hero();
        if (!picking) hide = hero;
    }
    if (local->hidden_hero() == hide) return std::nullopt;
    log(hide ? "Hero " + std::to_string(hide) + ": real loadout shown for the match, chosen skins drawn from the mounted VPK"
             : std::string("Hero skins: every choice shown"));
    return local->hide_hero(hide);
}

bool pump() {
    if (!pending.empty()) return true;
    for (unsigned consumed = 0; consumed < 128; ++consumed) {
        std::uint32_t size = 0;
        if (!original_available(coordinator, &size)) return false;
        if (!size || size > max_message) return false;
        Pending incoming{};
        std::optional<Packet> view_update;
        bool discarded = false;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            incoming.bytes.resize(size); std::uint32_t actual = 0;
            auto result = original_retrieve(coordinator, &incoming.type, incoming.bytes.data(), size, &actual);
            if (result == 2 && actual > size && actual <= max_message) { size = actual; continue; }
            if (result != 0 || actual > size) return false;
            incoming.bytes.resize(actual);
            try {
                auto type = incoming.type & 0x7FFFFFFFu;
                if (type == 7504) { auto msg = Packet::parse(incoming.type, incoming.bytes); if (emotes::patch(msg, local_account, emoticon_mask)) incoming.bytes = msg.encode(); }
                if (roster && (type == 21 || type == 22 || type == 23 || type == 24 || type == 26 || type == 4004)) {
                    auto msg = Packet::parse(incoming.type, incoming.bytes);
                    if (roster->incoming(msg)) view_update = match_view();
                }
                if (!stopping && ranks && (type == 7388 || type == 15001 || type == 15002)) {
                    auto message = Packet::parse(incoming.type, incoming.bytes);
                    const bool own = ranks->private_response(message);
                    const bool changed = ranks->incoming(message);
                    if (own && !changed) { log("Hero XP request rejected: response=" + std::to_string(type) + " result=" + std::to_string(message.header.number(13, 1))); discarded = true; break; }
                    if (changed) { incoming.type = message.type; incoming.bytes = message.encode(); incoming.tier_update = own; }
                }
                if (!stopping && (type == 21 || type == 22 || type == 23 || type == 24 || type == 25 || type == 26 || type == 4004)) {
                    auto message = Packet::parse(incoming.type, incoming.bytes);
                    auto candidate = *local;
                    auto owned_before = local->owned_text();
                    if (candidate.incoming(message)) {
                        incoming.bytes = message.encode(); incoming.virtual_cache = candidate.active() && (type == 24 || type == 4004);
                    }
                    *local = std::move(candidate);
                    if (local->active() && local->owned_text() != owned_before) skins_dirty = true;
                }
            } catch (const std::exception& e) {
                log(std::string("Incoming packet unchanged: ") + e.what());
            }
            enqueue(std::move(incoming));
            if (view_update) try { enqueue({view_update->type, view_update->encode()}); } catch (const std::exception& e) { log(std::string("Loadout view not queued: ") + e.what()); }
            return true;
        }
        if (!discarded) return false;
    }
    return false;
}

void apply_mount() noexcept {
    if (!mount_pending) return;
    if (livemount::connected_to_game(game_state.fresh(GetTickCount64()), game_state.phase(), roster && roster->in_match())) {
        if (!mount_deferred) { mount_deferred = true; log("Match skins: new choice kept for later; it is mounted once you are back in the menu (remounting while connected to a game server drops the connection)"); }
        return;
    }
    if (mount_deferred) log("Match skins: back in the menu, applying the postponed choice");
    mount_pending = mount_deferred = false;
    try {
        if (!filesystem_checked) {
            filesystem_checked = true; auto why = filesystem.open();
            log(why.empty() ? "Match skins: Dota file system interface verified" : "Match skins disabled: " + why);
        }
        if (!filesystem.ready()) return;
        if (!mount_next.empty()) filesystem.add(mount_next);
        if (!mount_current.empty()) filesystem.remove(mount_current);
        log(mount_next.empty() ? std::string("Match skins: unmounted") : "Match skins: mounted " + mount_next);
        mount_current = mount_next; filesystem.print();
    } catch (...) { log("Match skins: mount failed"); }
}

bool hook_available(void* self, std::uint32_t* size) noexcept {
    try {
        std::lock_guard guard(lock);
        apply_mount();
        if (self != coordinator) return original_available(self, size);
        if (pending.empty()) if (auto update = match_view()) enqueue({update->type, update->encode()});
        if (pump()) { if (size) *size = static_cast<std::uint32_t>(pending.front().bytes.size()); return true; }
        return original_available(self, size);
    } catch (...) { return original_available(self, size); }
}
int hook_retrieve(void* self, std::uint32_t* type, void* buffer, std::uint32_t capacity, std::uint32_t* required) noexcept {
    try {
        std::lock_guard guard(lock);
        if (self != coordinator) return original_retrieve(self, type, buffer, capacity, required);
        if (!pump()) return original_retrieve(self, type, buffer, capacity, required);
        const auto& value = pending.front();
        if (!required) return 4;
        *required = static_cast<std::uint32_t>(value.bytes.size());
        if (type) *type = value.type;
        if (capacity < value.bytes.size()) return 2;
        if (!buffer || !type) return 4;
        std::memcpy(buffer, value.bytes.data(), value.bytes.size());
        if (value.virtual_cache) status("virtual_cache_delivered");
        pending_bytes -= value.bytes.size(); pending.pop_front(); return 0;
    } catch (const std::exception& e) { log(e.what()); return 4; }
    catch (...) { return 4; }
}
int hook_send(void* self, std::uint32_t type, const void* data, std::uint32_t size) noexcept {
    try {
        std::unique_lock guard(lock);
        if (self != coordinator || stopping || !(type & 0x80000000u) || !data || size > max_message)
            return original_send(self, type, data, size);
        Packet request;
        try { request = Packet::parse(type, View(static_cast<const std::uint8_t*>(data), size)); }
        catch (...) { return original_send(self, type, data, size); }
        auto message_id = type & 0x7FFFFFFFu;
        if (message_id == 7503 && !emoticon_mask.empty()) {
            auto response = emotes::response(local_account, emoticon_mask, &request); enqueue({response.type, response.encode()}); return 0;
        }
        if (message_id != 1059 && message_id != 2569 && message_id != 2571 && message_id != 2577) {
            if (inventory::mentions_virtual(request.body)) throw std::runtime_error("unsupported operation on a local-only item");
            return original_send(self, type, data, size);
        }
        auto candidate = *local; std::vector<Packet> generated;
        if (!candidate.outgoing(request, generated)) {
            if (auto saved = candidate.save(); saved != local->save()) { persist(L"loadout.txt", std::move(saved)); *local = std::move(candidate); skins_dirty = true; }
            return original_send(self, type, data, size);
        }
        std::vector<Pending> prepared;
        std::size_t bytes = pending_bytes;
        for (auto& message : generated) { auto encoded = message.encode(); bytes += encoded.size(); prepared.push_back({message.type, std::move(encoded), false, 0, true}); }
        if (bytes > 64 * 1024 * 1024 || pending.size() + prepared.size() > 128) throw std::runtime_error("local message queue full");
        persist(L"loadout.txt", candidate.save());
        *local = std::move(candidate); skins_dirty = true;
        for (auto& value : prepared) enqueue(std::move(value));
        auto log_equip = [](const Message& e) {
            const auto id = e.number(1);
            log("Equip: hero " + std::to_string(e.number(2)) + " slot " + std::to_string(e.number(3)) + " -> "
                + (inventory::reserved(id) ? "item " + std::to_string(static_cast<std::uint32_t>(id)) : id ? std::string("owned item") : std::string("default")));
        };
        if (message_id == 1059) log_equip(request.body);
        if (message_id == 2569) for (const auto& f : request.body.fields) if (f.number == 1 && f.type == 2) log_equip(Message::parse(f.data));
        return 0;
    } catch (const std::exception& e) { log(std::string("Local operation rejected: ") + e.what()); return 4; }
    catch (...) { return 4; }
}

void tier_watcher() {
    std::string previous;
    for (;;) {
        Sleep(500);
        try {
            auto path = state_directory / "tiers.txt";
            if (!fs::exists(path)) continue;
            auto bytes = read_file(path); std::string text(bytes.begin(), bytes.end());
            if (text == previous) continue;
            std::optional<Packet> request;
            { std::lock_guard guard(lock);
              if (!ranks) continue;
              ranks->configure(text); previous = text;
              if (auto update = ranks->update()) enqueue({update->type, update->encode(), false, 0, false, true});
              request = ranks->request(); }
            auto encoded = request->encode();
            original_send(coordinator, request->type, encoded.data(), static_cast<unsigned>(encoded.size()));
            log("Hero levels applied");
        } catch (const std::exception& e) { log(std::string("Hero levels: ") + e.what()); }
    }
}
void skins_worker() {
    std::unique_ptr<assets::VPK> pak; std::string last_signature;
    const auto root = state_directory / L"live-skins";
    livemount::Settle settle;
    for (;;) {
        Sleep(250);
        if (skins_dirty.exchange(false)) settle.touch(GetTickCount64());
        if (!settle.ready(GetTickCount64())) continue;
        try {
            std::string loadout, owned;
            { std::lock_guard guard(lock); if (!local || !local->active()) { settle.touch(GetTickCount64()); continue; } loadout = local->save(); owned = local->owned_text(); }
            if (!pak) {
                auto filesystem_module = GetModuleHandleW(L"filesystem_stdio.dll");
                if (!filesystem_module) throw std::runtime_error("Dota file system module not loaded");
                wchar_t path[32768]; auto length = GetModuleFileNameW(filesystem_module, path, 32768);
                if (!length || length == 32768) throw std::runtime_error("Dota directory unavailable");
                auto game = fs::path(std::wstring(path, length)).parent_path().parent_path().parent_path();
                pak = std::make_unique<assets::VPK>(game / L"dota/pak01_dir.vpk", true);
            }
            std::vector<std::string> notes; auto files = skins::overrides(*pak, loadout, owned, notes);
            std::string signature; for (const auto& [key, payload] : files) signature += key + ':' + std::to_string(skins::crc32(payload)) + ';';
            if (signature == last_signature) continue;
            last_signature = signature;
            std::string folder;
            if (!files.empty()) {
                auto target = root / std::to_wstring(GetTickCount64()); fs::create_directories(target);
                auto archive = skins::pack(files);
                security::write(target / L"pak01_999.vpk", archive.archive); security::write(target / L"pak01_dir.vpk", archive.directory);
                folder = assets::utf8(target.wstring()); std::replace(folder.begin(), folder.end(), char(92), '/');
            }
            { std::lock_guard guard(lock); mount_next = folder; mount_pending = true; }
            std::string report; for (const auto& note : notes) report += note + '\n';
            persist(L"skins.txt", report);
            log("Match skins: " + std::to_string(files.size()) + " models prepared (see skins.txt)");
            std::string keep_next, keep_current; { std::lock_guard guard(lock); keep_next = mount_next; keep_current = mount_current; }
            if (fs::exists(root)) for (const auto& entry : fs::directory_iterator(root)) {
                auto name = assets::utf8(entry.path().wstring()); std::replace(name.begin(), name.end(), char(92), '/');
                if (name == keep_next || name == keep_current) continue;
                std::error_code ignored; fs::remove_all(entry.path(), ignored);
            }
        } catch (const std::exception& e) { log(std::string("Match skins: ") + e.what()); }
    }
}

DWORD WINAPI initialize(void*) {
    try {
        wchar_t path[32768]; auto length = GetModuleFileNameW(module, path, 32768);
        if (!length || length == 32768) return 1;
        directory = fs::path(std::wstring(path, length)).parent_path();
        state_directory = fs::exists(directory / "installed.marker") ? directory.parent_path() / "state" : directory;
        fs::create_directories(state_directory);
        std::thread(writer).detach();
        status("initializing");
        hud::start({state_directory, &log, &persist});
        auto catalog = inventory::Catalog::parse(read_file(directory / "catalog.bin"));
        HMODULE api = nullptr;
        using UserHandle = int (*)();
        using FindInterface = void* (*)(int, const char*);
        using SteamID = std::uint64_t (*)(void*);
        using LoggedOn = bool (*)(void*);
        UserHandle get_user_handle = nullptr;
        FindInterface find_interface = nullptr;
        SteamID get_steam_id = nullptr;
        LoggedOn logged_on = nullptr;
        void* user = nullptr;
        std::uint64_t account_id = 0;
        const auto deadline = GetTickCount64() + 300000;
        auto last_log = GetTickCount64();
        status("waiting_steam");
        log("Waiting for Steam sign-in and the game coordinator");
        for (;;) {
            api = GetModuleHandleW(L"steam_api64.dll");
            if (api) {
                get_user_handle = symbol<UserHandle>(api, "SteamAPI_GetHSteamUser");
                find_interface = symbol<FindInterface>(api, "SteamInternal_FindOrCreateUserInterface");
                get_steam_id = symbol<SteamID>(api, "SteamAPI_ISteamUser_GetSteamID");
                logged_on = symbol<LoggedOn>(api, "SteamAPI_ISteamUser_BLoggedOn");
                if (!get_user_handle || !find_interface || !get_steam_id || !logged_on)
                    throw std::runtime_error("Steam API exports incompatible");
                auto handle = get_user_handle();
                user = nullptr; coordinator = nullptr; account_id = 0;
                if (handle > 0) {
                    for (int version = 30; version >= 19 && !user; --version) {
                        char name[64]; std::snprintf(name, sizeof(name), "SteamAPI_SteamUser_v%03d", version);
                        auto getter = symbol<void* (*)()>(api, name);
                        if (getter) {
                            user = getter();
                            if (!user) {
                                std::snprintf(name, sizeof(name), "SteamUser%03d", version);
                                user = find_interface(handle, name);
                            }
                        }
                    }
                    if (user && logged_on(user)) {
                        account_id = get_steam_id(user);
                        if (account_id && (account_id & 0xFFFFFFFFu)) coordinator = find_interface(handle, "SteamGameCoordinator001");
                    }
                }
                if (user && account_id && coordinator) break;
            }
            if (GetTickCount64() >= deadline)
                throw std::runtime_error("Dota/Steam not ready after 5 minutes; sign in to Steam and retry");
            if (GetTickCount64() - last_log >= 10000) { log("Still waiting for Dota/Steam; nothing installed yet"); last_log = GetTickCount64(); }
            Sleep(250);
        }
        status("initializing");
        local = std::make_unique<inventory::Inventory>(std::move(catalog), account_id);
        local_account = static_cast<unsigned>(account_id);
        roster = std::make_unique<matchdata::Roster>(local_account);
        if (fs::exists(directory / "emoticons.bin")) emoticon_mask = read_file(directory / "emoticons.bin");
        if (fs::exists(state_directory / "loadout.txt")) {
            auto saved = read_file(state_directory / "loadout.txt");
            try { local->restore(std::string(saved.begin(), saved.end())); }
            catch (const std::exception& e) { log(std::string("Saved loadout ignored: ") + e.what()); }
        }
        if (fs::exists(directory / "heroes.tsv")) {
            auto heroes = read_file(directory / "heroes.tsv");
            ranks = std::make_unique<tiers::Ranks>(static_cast<unsigned>(account_id), tiers::parse_heroes(std::string(heroes.begin(), heroes.end())));
        }
        {
            std::string token;
            if (fs::exists(state_directory / "gsi.token")) { auto bytes = read_file(state_directory / "gsi.token"); token.assign(bytes.begin(), bytes.end()); }
            auto why = game_state.start(token);
            log(why.empty() ? "Game state integration: listening on port " + std::to_string(gsi::port) : "Game state integration unavailable: " + why);
        }
        auto get_pipe = symbol<int (*)()>(api, "SteamAPI_GetHSteamPipe");
        auto provider = GetModuleHandleW(L"steamclient64.dll");
        if (!get_pipe || !provider || (callback_pipe = get_pipe()) <= 0) throw std::runtime_error("Steam callback pipe unavailable");
        callback_user = get_user_handle();
        callback_hooks.prepare(api, provider, hook_callback, hook_free_callback);
        callback_hooks.apply();
        original_table = *static_cast<void***>(coordinator);
        original_send = reinterpret_cast<Send>(original_table[0]);
        original_available = reinterpret_cast<Available>(original_table[1]);
        original_retrieve = reinterpret_cast<Retrieve>(original_table[2]);
        for (unsigned i = 0; i < 3; ++i) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(original_table[i], &info, sizeof(info)) || info.State != MEM_COMMIT ||
                !(info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                throw std::runtime_error("Coordinator vtable is not executable");
        }
        replacement_table[0] = reinterpret_cast<void*>(&hook_send);
        replacement_table[1] = reinterpret_cast<void*>(&hook_available);
        replacement_table[2] = reinterpret_cast<void*>(&hook_retrieve);
        DWORD protection;
        if (!VirtualProtect(coordinator, sizeof(void*), PAGE_READWRITE, &protection)) throw std::runtime_error("Cannot replace coordinator vtable");
        auto previous = InterlockedCompareExchangePointer(static_cast<void* volatile*>(coordinator), replacement_table, original_table);
        DWORD ignored; VirtualProtect(coordinator, sizeof(void*), protection, &ignored);
        if (previous != original_table) throw std::runtime_error("Coordinator changed during setup");
        adapter_installed = true;

        log("Adapter installed; waiting for the inventory. Items: " + std::to_string(local->count()));
        status("awaiting_cache");
        auto refresh = local->refresh(); auto encoded = refresh.encode();
        original_send(coordinator, refresh.type, encoded.data(), static_cast<std::uint32_t>(encoded.size()));
        if (!emoticon_mask.empty()) { auto query = packet(7503, {}); auto data = query.encode(); original_send(coordinator, query.type, data.data(), static_cast<unsigned>(data.size())); }
        if (ranks) std::thread(tier_watcher).detach();
        std::thread(skins_worker).detach();
        return 0;
    } catch (const std::exception& e) { if (!adapter_installed) callback_hooks.restore(); log(std::string("Initialization failed: ") + e.what()); status("failed"); return 1; }
    catch (...) { if (!adapter_installed) callback_hooks.restore(); log("Initialization failed"); status("failed"); return 1; }
}
}

extern "C" __declspec(dllexport) DWORD WINAPI InventoryStart(void*) {
    static std::once_flag once; static DWORD result = 1;
    std::call_once(once, [] { result = initialize(nullptr); }); return result;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { module = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}
