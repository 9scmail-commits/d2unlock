#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
namespace gsi {
constexpr unsigned short port = 38517;
constexpr const wchar_t* config_name = L"gamestate_integration_d2unlock.cfg";

inline std::string config(const std::string& token) {
    return "\"D2Unlock\"\n{\n"
           "\t\"uri\"\t\t\"http://127.0.0.1:" + std::to_string(port) + "/\"\n"
           "\t\"timeout\"\t\"1.0\"\n\t\"buffer\"\t\"0.0\"\n\t\"throttle\"\t\"0.1\"\n\t\"heartbeat\"\t\"5.0\"\n"
           "\t\"auth\"\n\t{\n\t\t\"token\"\t\"" + token + "\"\n\t}\n"
           "\t\"data\"\n\t{\n\t\t\"map\"\t\t\"1\"\n\t\t\"hero\"\t\t\"1\"\n\t}\n}\n";
}

inline int phase(const std::string& name) {
    static const std::pair<const char*, int> names[] = {
        {"DOTA_GAMERULES_STATE_INIT", 0}, {"DOTA_GAMERULES_STATE_WAIT_FOR_PLAYERS_TO_LOAD", 1},
        {"DOTA_GAMERULES_STATE_HERO_SELECTION", 2}, {"DOTA_GAMERULES_STATE_STRATEGY_TIME", 3},
        {"DOTA_GAMERULES_STATE_PRE_GAME", 4}, {"DOTA_GAMERULES_STATE_GAME_IN_PROGRESS", 5},
        {"DOTA_GAMERULES_STATE_POST_GAME", 6}, {"DOTA_GAMERULES_STATE_DISCONNECT", 7},
        {"DOTA_GAMERULES_STATE_TEAM_SHOWCASE", 8}, {"DOTA_GAMERULES_STATE_CUSTOM_GAME_SETUP", 9},
        {"DOTA_GAMERULES_STATE_WAIT_FOR_MAP_TO_LOAD", 10}, {"DOTA_GAMERULES_STATE_PLAYER_DRAFT", 12}};
    for (const auto& [text, value] : names) if (name == text) return value;
    return -1;
}

struct State { int phase = -1; unsigned hero = 0; };

inline std::optional<std::string> value(const std::string& body, const std::string& key, std::size_t from = 0, std::size_t to = std::string::npos) {
    auto at = body.find("\"" + key + "\"", from);
    if (at == std::string::npos || at >= to) return std::nullopt;
    at = body.find(':', at + key.size() + 2);
    if (at == std::string::npos || at >= to) return std::nullopt;
    at = body.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos || at >= to) return std::nullopt;
    if (body[at] == '"') { auto end = body.find('"', at + 1); if (end == std::string::npos || end > to) return std::nullopt; return body.substr(at + 1, end - at - 1); }
    auto end = body.find_first_of(",}\r\n", at);
    return body.substr(at, (end == std::string::npos ? body.size() : end) - at);
}
inline std::pair<std::size_t, std::size_t> object(const std::string& body, const std::string& key) {
    auto at = body.find("\"" + key + "\"");
    if (at == std::string::npos) return {std::string::npos, std::string::npos};
    auto open = body.find('{', at);
    if (open == std::string::npos) return {std::string::npos, std::string::npos};
    int depth = 0;
    for (auto i = open; i < body.size(); ++i) {
        if (body[i] == '{') ++depth;
        else if (body[i] == '}' && --depth == 0) return {open, i};
    }
    return {std::string::npos, std::string::npos};
}
inline std::optional<State> parse(const std::string& body, const std::string& token) {
    auto [auth, auth_end] = object(body, "auth");
    if (auth == std::string::npos || value(body, "token", auth, auth_end) != token) return std::nullopt;
    State state;
    auto [map, map_end] = object(body, "map");
    if (map != std::string::npos) if (auto name = value(body, "game_state", map, map_end)) state.phase = phase(*name);
    auto [hero, hero_end] = object(body, "hero");
    if (hero != std::string::npos) if (auto id = value(body, "id", hero, hero_end)) {
        auto n = std::strtol(id->c_str(), nullptr, 10); if (n > 0 && n < 1000) state.hero = static_cast<unsigned>(n);
    }
    return state;
}

class Listener {
    std::atomic<int> phase_{-1};
    std::atomic<unsigned> hero_{0};
    std::atomic<unsigned long long> seen_{0};
    std::string token_;
    SOCKET server_ = INVALID_SOCKET;
    void serve(SOCKET client) {
        DWORD timeout = 2000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request; char buffer[8192]; std::size_t header = std::string::npos, length = 0;
        while (request.size() < (1u << 20)) {
            int n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.append(buffer, n);
            if (header == std::string::npos && (header = request.find("\r\n\r\n")) != std::string::npos) {
                auto lower = request.substr(0, header); for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                auto at = lower.find("content-length:");
                length = at == std::string::npos ? 0 : std::strtoul(lower.c_str() + at + 15, nullptr, 10);
            }
            if (header != std::string::npos && request.size() >= header + 4 + length) break;
        }
        static const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(client, reply, sizeof(reply) - 1, 0);
        if (header == std::string::npos || request.size() < header + 4 + length) return;
        if (auto state = parse(request.substr(header + 4, length), token_)) {
            phase_ = state->phase; hero_ = state->hero; seen_ = GetTickCount64();
        }
    }
public:
    std::string start(std::string token) {
        if (token.empty()) return "no token";
        token_ = std::move(token);
        WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data)) return "WSAStartup failed";
        server_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_ == INVALID_SOCKET) return "socket failed";
        BOOL exclusive = TRUE; setsockopt(server_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(server_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(server_, 8)) {
            closesocket(server_); server_ = INVALID_SOCKET; return "port " + std::to_string(port) + " is in use";
        }
        std::thread([this] { for (;;) { auto client = accept(server_, nullptr, nullptr); if (client == INVALID_SOCKET) { Sleep(100); continue; } serve(client); closesocket(client); } }).detach();
        return {};
    }
    bool fresh(unsigned long long now) const { auto seen = seen_.load(); return seen && now - seen < 15000; }
    int phase() const { return phase_; }
    unsigned hero() const { return hero_; }
};
}
