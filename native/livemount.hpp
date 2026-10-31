#pragma once
#include <windows.h>
#include "symbol.hpp"
#include <cstdint>
#include <cstring>
#include <string>
namespace livemount {
constexpr int add_slot = 31, remove_slot = 32, print_slot = 71;

// Swapping search paths while the client is connected to a game server (picking, strategy, the match,
// post-game) drops the connection with "connection to the remote server lost". Mounts wait for the menu.
// Fresh game state integration is authoritative (phase -1 = no map loaded); the lobby is the fallback.
inline bool connected_to_game(bool state_fresh, int phase, bool lobby_in_match) { return state_fresh ? phase >= 0 : lobby_in_match; }

// Collapses a burst of loadout changes (clicking through sets) into one rebuild after the clicking stops.
class Settle {
    unsigned long long changed_ = 0; bool pending_ = false;
public:
    static constexpr unsigned long long quiet = 2500;
    void touch(unsigned long long now) { changed_ = now; pending_ = true; }
    bool ready(unsigned long long now) { if (!pending_ || now - changed_ < quiet) return false; pending_ = false; return true; }
};
enum : int { add_to_head = 0, priority_vpk = 2 };

class FileSystem {
    void* self = nullptr; void** table = nullptr;
    const unsigned char* begin = nullptr; const unsigned char* end = nullptr;
    bool inside(const void* p, std::size_t n) const { auto c = static_cast<const unsigned char*>(p); return c >= begin && c + n <= end; }
    bool references(const unsigned char* fn, std::size_t length, const char* text) const {
        const auto size = std::strlen(text) + 1;
        if (!inside(fn, length)) return false;
        for (std::size_t i = 0; i + 7 <= length; ++i) {
            const auto* at = fn + i;
            if ((at[0] == 0x48 || at[0] == 0x4C) && at[1] == 0x8D && (at[2] & 0xC7) == 0x05) {
                std::int32_t disp; std::memcpy(&disp, at + 3, 4);
                const auto* target = at + 7 + disp;
                if (inside(target, size) && !std::memcmp(target, text, size)) return true;
            }
        }
        return false;
    }
    bool calls_referencing(const unsigned char* fn, std::size_t length, const char* text) const {
        if (!inside(fn, length)) return false;
        for (std::size_t i = 0; i + 5 <= length; ++i) if (fn[i] == 0xE8) {
            std::int32_t rel; std::memcpy(&rel, fn + i + 1, 4);
            const auto* target = fn + i + 5 + rel;
            if (inside(target, 0x900) && references(target, 0x900, text)) return true;
        }
        return false;
    }
public:
    bool ready() const { return self != nullptr; }
    std::string open() {
        auto module = GetModuleHandleW(L"filesystem_stdio.dll");
        if (!module) return "filesystem_stdio.dll not loaded";
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const unsigned char*>(module) + dos->e_lfanew);
        begin = reinterpret_cast<const unsigned char*>(module); end = begin + nt->OptionalHeader.SizeOfImage;
        auto create = symbol<void* (*)(const char*, int*)>(module, "CreateInterface");
        if (!create) return "CreateInterface missing";
        auto candidate = create("VFileSystem017", nullptr);
        if (!candidate) return "VFileSystem017 unavailable";
        auto vt = *static_cast<void***>(candidate);
        if (!inside(vt, (print_slot + 1) * sizeof(void*))) return "unexpected interface table";
        auto slot = [&](int i) { return static_cast<const unsigned char*>(vt[i]); };
        if (!references(slot(print_slot), 0x200, "File Path:")) return "PrintSearchPaths slot changed";
        if (!calls_referencing(slot(add_slot), 0x900, "pak*_dir.vpk")) return "AddSearchPath slot changed";
        if (!calls_referencing(slot(remove_slot), 0x900, "ugc:") || calls_referencing(slot(remove_slot), 0x900, "pak*_dir.vpk")) return "RemoveSearchPath slot changed";
        self = candidate; table = vt; return {};
    }
    void add(const std::string& path) const {
        reinterpret_cast<void (*)(void*, const char*, const char*, int, int, int)>(table[add_slot])(self, path.c_str(), "GAME", add_to_head, priority_vpk, 0);
    }
    bool remove(const std::string& path) const {
        return reinterpret_cast<bool (*)(void*, const char*, const char*)>(table[remove_slot])(self, path.c_str(), "GAME");
    }
    void print() const { reinterpret_cast<void (*)(void*)>(table[print_slot])(self); }
};
}
