#pragma once
#include "wire.hpp"
#include <set>
namespace matchdata {
struct Player { unsigned account = 0, hero = 0, team = 0; };
class Roster {
    unsigned self;
    std::uint64_t lobby = 0, state = 0, phase = 0;
    std::vector<Player> players;
    bool accept(const wire::Message& m, bool removed = false) {
        if (removed) { if (m.number(1) != lobby) return false; lobby = state = phase = 0; players.clear(); return true; }
        std::vector<wire::Message> members; std::vector<unsigned> indices;
        for (const auto& f : m.fields) {
            if (f.number == 120 && f.type == 2) members.push_back(wire::Message::parse(f.data));
            if (f.number == 121 && f.type == 0) indices.push_back(static_cast<unsigned>(f.value));
            if (f.number == 121 && f.type == 2) { std::size_t at = 0; while (at < f.data.size()) indices.push_back(static_cast<unsigned>(wire::read_varint(f.data, at))); }
        }
        if (members.size() > 256 || indices.size() > 256) throw std::runtime_error("Lobby size limit");
        if (indices.empty()) for (unsigned i = 0; i < members.size(); ++i) indices.push_back(i);
        std::vector<Player> next; std::set<unsigned> seen; bool own = false;
        for (auto i : indices) {
            if (i >= members.size()) continue;
            const auto& v = members[i]; auto steam = v.number(1);
            unsigned id = static_cast<unsigned>(steam), team = static_cast<unsigned>(v.number(3));
            if (team > 1 || !id || id == UINT32_MAX || !seen.insert(id).second) continue;
            if ((steam >> 56) != 1 || ((steam >> 52) & 15) != 1) continue;
            own |= id == self; next.push_back({id, static_cast<unsigned>(v.number(2)), team});
        }
        if (!own || next.size() > 10) return false;
        players = std::move(next); lobby = m.number(1); state = m.number(4); phase = m.number(22); return true;
    }
    bool cache(const wire::Message& m) {
        bool changed = false;
        for (const auto& f : m.fields) if (f.number == 2 && f.type == 2) {
            auto group = wire::Message::parse(f.data); if (group.number(1) != 2004) continue;
            for (const auto& o : group.fields) if (o.number == 2 && o.type == 2) changed |= accept(wire::Message::parse(o.data));
        }
        return changed;
    }
public:
    explicit Roster(unsigned account) : self(account) {}
    bool incoming(const wire::Packet& p) {
        auto type = p.type & 0x7fffffff;
        if (type == 24) return cache(p.body);
        bool changed = false;
        if (type == 4004) for (const auto& f : p.body.fields) if (f.number == 3 && f.type == 2) changed |= cache(wire::Message::parse(f.data));
        if (type >= 21 && type <= 23 && p.body.number(2) == 2004) changed = accept(wire::Message::parse(p.body.bytes(3)), type == 23);
        if (type == 26) for (const auto& f : p.body.fields) if ((f.number == 2 || f.number == 4 || f.number == 5) && f.type == 2) {
            auto o = wire::Message::parse(f.data);
            if (o.number(1) == 2004) changed |= accept(wire::Message::parse(o.bytes(2)), f.number == 5);
        }
        return changed;
    }
    bool in_match() const { return lobby != 0 && (state == 1 || state == 2 || state == 4 || state == 6); }
    unsigned game_phase() const { return static_cast<unsigned>(phase); }
    std::uint64_t lobby_id() const { return lobby; }
    unsigned own_hero() const { for (const auto& p : players) if (p.account == self) return p.hero; return 0; }
};
}
