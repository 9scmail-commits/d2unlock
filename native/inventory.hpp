#pragma once
#include "wire.hpp"
#include <array>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace inventory {
using namespace wire;
constexpr std::uint64_t virtual_prefix = 0x70004F5600000000ULL;
inline bool reserved(std::uint64_t id) { return (id & 0xFFFFFFFF00000000ULL) == virtual_prefix; }
inline std::uint64_t virtual_id(std::uint32_t definition) { return virtual_prefix | definition; }
struct Definition { std::uint32_t index, quality; std::set<std::uint32_t> styles; };
using Slot = std::pair<std::uint32_t, std::uint32_t>;

struct Catalog {
    std::map<std::uint32_t, Definition> definitions;
    std::uint32_t style_attribute = 0;
    std::array<std::uint8_t, 32> schema_hash{};
    static Catalog parse(View in) {
        if (in.size() < 52 || std::string(in.begin(), in.begin() + 8) != "OVICAT02")
            throw std::runtime_error("invalid catalog");
        std::size_t at = 8;
        auto take = [&]() -> std::uint32_t {
            if (in.size() - at < 4) throw std::runtime_error("truncated catalog");
            auto value = little(in.subspan(at, 4)); at += 4; return static_cast<std::uint32_t>(value);
        };
        if (take() != 2) throw std::runtime_error("unsupported catalog");
        auto count = take();
        if (!count || count > 100000) throw std::runtime_error("catalog count limit");
        Catalog result; result.style_attribute = take();
        std::copy_n(in.begin() + at, 32, result.schema_hash.begin()); at += 32;
        for (std::uint32_t n = 0; n < count; ++n) {
            Definition item; item.index = take(); item.quality = take(); auto styles = take();
            if (!item.index || item.quality > 100 || !styles || styles > 255)
                throw std::runtime_error("invalid catalog definition");
            for (unsigned s = 0; s < styles; ++s) { auto style = take();
                if (style >= 255 || !item.styles.insert(style).second) throw std::runtime_error("invalid style"); }
            if (!result.definitions.emplace(item.index, item).second) throw std::runtime_error("duplicate definition");
        }
        if (at != in.size()) throw std::runtime_error("trailing catalog data");
        return result;
    }
};

inline bool mentions_virtual(const Message& msg, unsigned depth = 0) {
    for (const auto& f : msg.fields) {
        if (f.type != 2 && reserved(f.value)) return true;
        if (f.type == 2 && depth < 8 && f.data.size() <= 1024 * 1024) {
            try { if (mentions_virtual(Message::parse(f.data), depth + 1)) return true; } catch (const std::exception&) {}
        }
    }
    return false;
}

class Inventory {
public:
    explicit Inventory(Catalog catalog, std::uint64_t steam_id) : catalog_(std::make_shared<const Catalog>(std::move(catalog))), steam_id_(steam_id) {
        if (!steam_id_ || !(steam_id_ & 0xFFFFFFFFu)) throw std::runtime_error("invalid local SteamID");
    }
    bool active() const { return active_; }
    unsigned hidden_hero() const { return hidden_hero_; }
    std::optional<Packet> hide_hero(unsigned hero) {
        if (hero == global_class) hero = 0;
        if (hidden_hero_ == hero) return std::nullopt;
        const auto previous = hidden_hero_;
        hidden_hero_ = hero;
        if (!active_) return std::nullopt;
        auto affected = [&](const Slot& slot) { return slot.first && (slot.first == previous || slot.first == hero); };
        std::set<std::uint64_t> dirty;
        for (const auto& [slot, id] : loadout_) if (affected(slot)) dirty.insert(id);
        for (const auto& [id, original] : real_) for (const auto& f : original.fields)
            if (f.number == 18 && f.type == 2) {
                auto equipped = Message::parse(f.data);
                Slot slot{static_cast<std::uint32_t>(equipped.number(1)), static_cast<std::uint32_t>(equipped.number(2))};
                if (affected(slot) && loadout_.contains(slot)) dirty.insert(id);
            }
        if (dirty.empty()) return std::nullopt;
        return updates(dirty);
    }
    std::size_t count() const { return catalog_->definitions.size(); }
    bool is_virtual(std::uint64_t id) const { return reserved(id) && catalog_->definitions.contains(static_cast<std::uint32_t>(id)); }
    Message owner() const { Message m; m.add(1, 1); m.add(2, steam_id_); return m; }
    Packet refresh() const { Message m; m.add_bytes(2, owner().encode()); return packet(28, std::move(m)); }

    Message item(std::uint64_t id) const {
        Message result;
        if (is_virtual(id)) {
            const auto& definition = catalog_->definitions.at(static_cast<std::uint32_t>(id));
            result.add(1, id); result.add(2, steam_id_ & 0xFFFFFFFFu);
            result.add(3, definition.index);
            result.add(4, definition.index); result.add(5, 1); result.add(6, 1);
            result.add(7, definition.quality); result.add(8, 0); result.add(9, 0);
            auto style = styles_.find(id); result.add(15, style == styles_.end() ? *definition.styles.begin() : style->second);
            if (catalog_->style_attribute) {
                Message attr; attr.add(1, catalog_->style_attribute);
                Bytes mask(4, 0xFF); attr.add_bytes(3, std::move(mask)); result.add_bytes(12, attr.encode());
            }
        } else {
            auto it = real_.find(id); if (it == real_.end()) throw std::runtime_error("unknown real item");
            result = it->second;
            std::erase_if(result.fields, [&](const Field& f) {
                if (f.number != 18 || f.type != 2) return false;
                auto equip = Message::parse(f.data);
                Slot slot{static_cast<std::uint32_t>(equip.number(1)), static_cast<std::uint32_t>(equip.number(2))};
                return loadout_.contains(slot) && !hidden(slot);
            });
        }
        for (const auto& [slot, selected] : loadout_) if (selected == id && !hidden(slot)) {
            Message equip; equip.add(1, slot.first); equip.add(2, slot.second); result.add_bytes(18, equip.encode());
        }
        return result;
    }

    bool incoming(Packet& message) {
        auto type = message.type & 0x7FFFFFFFu;
        if (type == 24) return subscribe(message.body);
        if (type == 4004) {
            bool changed = false;
            for (auto& field : message.body.fields) if (field.number == 3 && field.type == 2) {
                auto cache = Message::parse(field.data);
                if (subscribe(cache)) { field.data = cache.encode(); changed = true; }
            }
            return changed;
        }
        if (type == 25 && owned(message.body, 2)) { active_ = false; real_.clear(); return false; }
        if ((type == 21 || type == 22 || type == 23) && owned(message.body, 5) && message.body.number(2) == 1 && message.body.number(6)==service_id_) {
            version_ = message.body.number(4, version_);
            auto econ = Message::parse(message.body.bytes(3)); auto id = econ.number(1);
            if (reserved(id)) throw std::runtime_error("real/virtual ID collision");
            if (type == 23) { real_.erase(id); std::erase_if(loadout_, [&](auto& entry) { return entry.second == id; }); return false; }
            real_[id] = econ;
            message.body.erase(3); message.body.add_bytes(3, item(id).encode()); return true;
        }
        if (type == 26 && owned(message.body, 6) && message.body.number(7)==service_id_) {
            version_ = message.body.number(3, version_);
            bool changed = false;
            for (auto& field : message.body.fields) if ((field.number == 2 || field.number == 4 || field.number == 5) && field.type == 2) {
                auto object = Message::parse(field.data);
                if (object.number(1) != 1) continue;
                auto econ = Message::parse(object.bytes(2)); auto id = econ.number(1);
                if (reserved(id)) throw std::runtime_error("real/virtual ID collision");
                if (field.number == 5) { real_.erase(id); std::erase_if(loadout_, [&](auto& entry) { return entry.second == id; }); continue; }
                real_[id] = econ; object.erase(2); object.add_bytes(2, item(id).encode()); field.data = object.encode(); changed = true;
            }
            return changed;
        }
        return false;
    }

    bool outgoing(const Packet& request, std::vector<Packet>& replies) {
        auto type = request.type & 0x7FFFFFFFu;
        if (!active_) {
            if (mentions_virtual(request.body)) throw std::runtime_error("inventory is not synchronized");
            return false;
        }
        if (type == 2569 || type == 1059) {
            std::vector<Message> equips;
            if (type == 1059) equips.push_back(request.body);
            else for (const auto& f : request.body.fields) if (f.number == 1 && f.type == 2) equips.push_back(Message::parse(f.data));
            if (hidden_hero_ && std::all_of(equips.begin(), equips.end(), [&](const Message& e) { return !reserved(e.number(1)) && e.number(2) == hidden_hero_; })) {
                for (const auto& e : equips) {
                    auto id = e.number(1); Slot slot{static_cast<std::uint32_t>(e.number(2)), static_cast<std::uint32_t>(e.number(3))};
                    if (slot.second == UINT32_MAX) std::erase_if(loadout_, [&](auto& entry) { return entry.second == id && entry.first.first == slot.first; });
                    else loadout_.erase(slot);
                }
                return false;
            }
            bool local = false;
            for (const auto& e : equips)
                local |= reserved(e.number(1)) || loadout_.contains({static_cast<std::uint32_t>(e.number(2)), static_cast<std::uint32_t>(e.number(3))});
            if (!local) return false;
            if (equips.size() > 128) throw std::runtime_error("too many equips");
            for (const auto& e : equips) validate_equip(e);
            std::set<std::uint64_t> dirty;
            std::set<Slot> touched;
            for (const auto& e : equips) {
                auto id = e.number(1);
                if (id) dirty.insert(id);
                Slot slot{static_cast<unsigned>(e.number(2)), static_cast<unsigned>(e.number(3))};
                if (slot.second != UINT32_MAX) touched.insert(slot);
                else for (auto [previous, selected] : loadout_)
                    if (selected == id && previous.first == slot.first) touched.insert(previous);
            }
            for (auto slot : touched) {
                auto previous = loadout_.find(slot);
                if (previous != loadout_.end()) dirty.insert(previous->second);
            }
            for (const auto& [id, original] : real_) for (const auto& f : original.fields)
                if (f.number == 18 && f.type == 2) {
                    auto equipped = Message::parse(f.data);
                    if (touched.contains({static_cast<unsigned>(equipped.number(1)), static_cast<unsigned>(equipped.number(2))})) dirty.insert(id);
                }
            auto next_loadout = loadout_; auto next_styles = styles_;
            for (const auto& e : equips) {
                auto id = e.number(1); Slot slot{static_cast<std::uint32_t>(e.number(2)), static_cast<std::uint32_t>(e.number(3))};
                if (slot.second == UINT32_MAX) std::erase_if(next_loadout, [&](auto& entry) { return entry.second == id && entry.first.first == slot.first; });
                else if (id == 0) next_loadout.erase(slot);
                else next_loadout[slot] = id;
                if (is_virtual(id) && e.number(4, 255) != 255) next_styles[id] = static_cast<std::uint32_t>(e.number(4));
            }
            loadout_ = std::move(next_loadout); styles_ = std::move(next_styles);
            if (type == 2569) { Message answer; answer.add(1, version_, 1); replies.push_back(reply(2570, std::move(answer), request)); }
            replies.push_back(updates(dirty));
            return true;
        }
        if ((type == 2577 || type == 2571) && reserved(request.body.number(1))) {
            auto id = request.body.number(1), style = request.body.number(2, 255);
            if (!is_virtual(id) || !catalog_->definitions.at(static_cast<std::uint32_t>(id)).styles.contains(static_cast<std::uint32_t>(style)))
                throw std::runtime_error("unknown virtual style");
            styles_[id] = static_cast<std::uint32_t>(style);
            replies.push_back(updates({id})); Message answer; answer.add(1, 0);
            if (type == 2571) { answer.add(2, id); answer.add(3, style); }
            replies.push_back(reply(type == 2577 ? 2578 : 2572, std::move(answer), request)); return true;
        }
        if (mentions_virtual(request.body)) throw std::runtime_error("unsupported operation on a local-only item");
        return false;
    }

    std::string owned_text() const {
        std::ostringstream out; out << "OVIREAL1\n";
        for (const auto& [id, original] : real_) for (const auto& f : original.fields)
            if (f.number == 18 && f.type == 2) {
                auto equipped = Message::parse(f.data);
                out << equipped.number(1) << ' ' << equipped.number(2) << ' ' << original.number(4) << '\n';
            }
        return out.str();
    }
    std::string save() const {
        std::ostringstream out; out << "OVILOAD1 " << steam_id_ << '\n';
        for (auto [slot, id] : loadout_) if (is_virtual(id)) out << "E " << id << ' ' << slot.first << ' ' << slot.second << '\n';
        for (auto [id, style] : styles_) if (is_virtual(id)) out << "S " << id << ' ' << style << '\n';
        return out.str();
    }
    void restore(const std::string& text) {
        std::istringstream in(text); std::string tag; std::uint64_t owner_id;
        if (!(in >> tag >> owner_id) || tag != "OVILOAD1" || owner_id != steam_id_) return;
        auto slots = loadout_; auto styles = styles_; unsigned count = 0;
        while (in >> tag) {
            if (++count > 100000) throw std::runtime_error("loadout limit");
            std::uint64_t id; std::uint32_t a, b;
            if (!(in >> id >> a)) throw std::runtime_error("invalid saved loadout");
            if (tag == "E") {
                if (!(in >> b) || a > 10000 || b > 255) throw std::runtime_error("invalid saved slot");
                if (is_virtual(id)) slots[{a, b}] = id;
            } else if (tag == "S") {
                if (is_virtual(id) && catalog_->definitions.at(static_cast<std::uint32_t>(id)).styles.contains(a)) styles[id] = a;
            } else throw std::runtime_error("unknown saved record");
        }
        loadout_ = std::move(slots); styles_ = std::move(styles);
    }

private:
    std::shared_ptr<const Catalog> catalog_;
    std::uint64_t steam_id_, version_ = 0;
    std::uint32_t service_id_ = 0;
    bool active_ = false;
    std::uint32_t hidden_hero_ = 0;
    static constexpr std::uint32_t global_class = 1000;
    bool hidden(const Slot& slot) const { return hidden_hero_ && slot.first == hidden_hero_; }
    std::map<std::uint64_t, Message> real_;
    std::map<Slot, std::uint64_t> loadout_;
    std::map<std::uint64_t, std::uint32_t> styles_;

    bool owned(const Message& m, unsigned field) const {
        auto value = m.bytes(field); if (value.empty()) return false;
        auto id = Message::parse(value); return id.number(1) == 1 && id.number(2) == steam_id_;
    }
    bool subscribe(Message& cache) {
        if (!owned(cache, 4)) return false;
        std::map<std::uint64_t, Message> real;
        bool found = false;
        for (auto& field : cache.fields) if (field.number == 2 && field.type == 2) {
            auto objects = Message::parse(field.data);
            if (objects.number(1) != 1) continue;
            found = true;
            for (auto& f : objects.fields) if (f.number == 2 && f.type == 2) {
                auto econ = Message::parse(f.data); auto id = econ.number(1);
                if (reserved(id)) throw std::runtime_error("real/virtual ID collision");
                if (econ.number(2) != (steam_id_ & 0xFFFFFFFFu)) throw std::runtime_error("unexpected item owner");
                real[id] = std::move(econ);
            }
        }
        if(active_&&!found&&cache.number(5)!=service_id_)return false;
        real_ = std::move(real); version_ = cache.number(3); service_id_=static_cast<std::uint32_t>(cache.number(5));
        Message objects; objects.add(1, 1);
        for (auto& [id, _] : real_) objects.add_bytes(2, item(id).encode());
        for (auto& [index, _] : catalog_->definitions) objects.add_bytes(2, item(virtual_id(index)).encode());
        std::erase_if(cache.fields, [](const Field& f) { return f.number == 2 && f.type == 2 && Message::parse(f.data).number(1) == 1; });
        cache.add_bytes(2, objects.encode()); active_ = true; (void)found; return true;
    }
    void validate_equip(const Message& e) const {
        auto id = e.number(1), hero = e.number(2), slot = e.number(3), style = e.number(4, 255);
        if (hero > 10000 || (slot > 255 && slot != UINT32_MAX)) throw std::runtime_error("invalid equip slot");
        if (id && !is_virtual(id) && !real_.contains(id)) throw std::runtime_error("unknown equip item");
        if (is_virtual(id) && style != 255 && !catalog_->definitions.at(static_cast<std::uint32_t>(id)).styles.contains(static_cast<std::uint32_t>(style)))
            throw std::runtime_error("unsupported style");
    }
    Packet updates(const std::set<std::uint64_t>& dirty) const {
        Message result; result.add(3, version_, 1); result.add_bytes(6, owner().encode());
        if(service_id_)result.add(7,service_id_);
        for (auto id : dirty) {
            Message entry; entry.add(1, 1); entry.add_bytes(2, item(id).encode());
            result.add_bytes(2, entry.encode());
        }
        auto update=packet(26, std::move(result));update.header.add(200,3);return update;
    }
};
}
