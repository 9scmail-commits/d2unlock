#pragma once
#include "resources.hpp"
#include <wincrypt.h>
#include <functional>
#include <set>
#include <sstream>
namespace skins {
using namespace assets;
constexpr std::uint64_t virtual_prefix = 0x70004F5600000000ULL;
constexpr unsigned archive_index = 999;

struct Choice { unsigned hero = 0, slot = 0, definition = 0, style = 0; };
using Slot = std::pair<unsigned, unsigned>;

inline std::vector<Choice> choices(const std::string& text) {
    std::istringstream in(text); std::string tag; std::uint64_t owner = 0;
    if (!(in >> tag >> owner) || tag != "OVILOAD1") return {};
    std::map<std::uint64_t, unsigned> styles; std::vector<std::pair<std::uint64_t, Slot>> equips;
    while (in >> tag) {
        std::uint64_t id; unsigned a;
        if (!(in >> id >> a)) break;
        if (tag == "E") { unsigned b; if (!(in >> b)) break; equips.push_back({id, {a, b}}); }
        else if (tag == "S") styles[id] = a;
        else break;
    }
    std::vector<Choice> out;
    for (const auto& [id, slot] : equips) {
        if ((id & 0xFFFFFFFF00000000ULL) != virtual_prefix || slot.first == 1000) continue;
        auto style = styles.find(id);
        out.push_back({slot.first, slot.second, static_cast<unsigned>(id), style == styles.end() ? 0u : style->second});
    }
    return out;
}
inline std::map<Slot, unsigned> owned(const std::string& text) {
    std::istringstream in(text); std::string tag; std::map<Slot, unsigned> out;
    if (!(in >> tag) || tag != "OVIREAL1") return out;
    unsigned hero, slot, definition;
    while (in >> hero >> slot >> definition) out[{hero, slot}] = definition;
    return out;
}

struct Plan { std::map<std::string, std::string> files; std::vector<std::string> notes; };

inline Plan plan(const VPK& archive, const std::vector<Choice>& picks, const std::map<Slot, unsigned>& real) {
    Plan result; if (picks.empty()) return result;
    auto document = Parser(decode(archive.get("scripts/items/items_game.txt"))).parse();
    const auto& schema = document.get("items_game");
    std::map<std::string, const KV*> prefabs; std::map<std::string, KV> cache;
    for (const auto& [k, v] : schema.get("prefabs").children) prefabs[k] = &v;
    std::set<std::string> resolving; std::function<KV(const KV&)> resolve;
    resolve = [&](const KV& node) {
        KV out; std::istringstream list(node.text("prefab")); std::string name;
        while (list >> name) {
            if (!cache.contains(name)) {
                if (!prefabs.contains(name) || !resolving.insert(name).second) throw std::runtime_error("Invalid prefab: " + name);
                cache[name] = resolve(*prefabs.at(name)); resolving.erase(name);
            }
            out = merge(std::move(out), cache.at(name));
        }
        return merge(std::move(out), node);
    };
    std::map<unsigned, const KV*> items;
    for (const auto& [k, v] : schema.get("items").children)
        if (!k.empty() && std::all_of(k.begin(), k.end(), [](unsigned char c) { return isdigit(c); }) && k.size() < 10) items[std::stoul(k)] = &v;

    std::map<unsigned, std::string> heroes;
    auto heroes_doc = Parser(decode(archive.get("scripts/npc/npc_heroes.txt"))).parse();
    auto definitions = heroes_doc.get("DOTAHeroes");
    for (const auto& [key, value] : heroes_doc.children)
        if (key == "#base" && value.value.rfind("heroes/", 0) == 0 && value.value.find("..") == std::string::npos && archive.has("scripts/npc/" + value.value)) {
            auto included = Parser(decode(archive.get("scripts/npc/" + value.value))).parse();
            for (const auto& entry : included.get("DOTAHeroes").children) definitions.children.push_back(entry);
        }
    for (const auto& [name, hero] : definitions.children) {
        auto id = hero.text("HeroID"); if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return isdigit(c); }) || id.size() > 6) continue;
        heroes[std::stoul(id)] = name;
    }

    struct Look { std::string model, slot; };
    auto look = [&](unsigned definition, unsigned style) -> Look {
        auto it = items.find(definition); if (it == items.end()) return {};
        auto item = resolve(*it->second);
        auto styled = item.get("visuals").get("styles").get(std::to_string(style)).text("model_player");
        return {lower(styled.empty() ? item.text("model_player") : styled), lower(item.text("item_slot"))};
    };
    std::map<std::pair<std::string, std::string>, std::string> defaults; bool defaults_ready = false;
    auto default_model = [&](const std::string& hero, const std::string& slot) -> std::string {
        if (!defaults_ready) {
            defaults_ready = true;
            for (const auto& [_, node] : items) {
                if (node->get("used_by_heroes").children.empty() && node->text("prefab").empty()) continue;
                auto item = resolve(*node);
                if (item.text("baseitem") != "1") continue;
                for (const auto& [user, __] : item.get("used_by_heroes").children)
                    defaults.emplace(std::pair{lower(user), lower(item.text("item_slot"))}, lower(item.text("model_player")));
            }
        }
        auto it = defaults.find({lower(hero), slot}); return it == defaults.end() ? std::string{} : it->second;
    };

    for (const auto& pick : picks) {
        auto hero = heroes.find(pick.hero);
        auto label = "hero " + std::to_string(pick.hero) + " slot " + std::to_string(pick.slot) + " item " + std::to_string(pick.definition);
        auto chosen = look(pick.definition, pick.style);
        if (hero == heroes.end() || chosen.slot.empty()) { result.notes.push_back(label + ": unknown hero or item"); continue; }
        const auto& slot = chosen.slot;
        auto owned_item = real.find({pick.hero, pick.slot});
        auto target = owned_item != real.end() ? look(owned_item->second, 0).model : default_model(hero->second, slot);
        const auto& source = chosen.model;
        if (target.empty()) { result.notes.push_back(label + " (" + slot + "): the match spawns no model in this slot"); continue; }
        if (source.empty()) { result.notes.push_back(label + " (" + slot + "): item has no own model (effects only)"); continue; }
        if (source == target) continue;
        if (!archive.has(source + "_c") || !archive.has(target + "_c")) { result.notes.push_back(label + ": model missing in pak01"); continue; }
        if (!result.files.emplace(target + "_c", source + "_c").second) { result.notes.push_back(label + ": model path already replaced"); continue; }
        result.notes.push_back(label + " (" + slot + "): " + target + " <- " + source);
    }
    return result;
}

inline std::uint32_t crc32(const Bytes& data) {
    static const auto table = [] { std::vector<std::uint32_t> t(256); for (std::uint32_t i = 0; i < 256; ++i) { auto c = i; for (int j = 0; j < 8; ++j) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0); t[i] = c; } return t; }();
    std::uint32_t crc = ~0u; for (auto v : data) crc = table[(crc ^ v) & 255] ^ (crc >> 8); return ~crc;
}
inline Bytes md5(const Bytes& data) {
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0; Bytes digest(16); DWORD size = 16;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)
        && CryptHashData(hash, data.data(), DWORD(data.size()), 0) && CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &size, 0);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    if (!ok) throw std::runtime_error("MD5 failed");
    return digest;
}

struct Archive { Bytes directory, archive; };
inline Archive pack(const std::map<std::string, Bytes>& files) {
    std::map<std::string, std::map<std::string, std::vector<std::pair<std::string, const Bytes*>>>> groups;
    for (const auto& [key, payload] : files) {
        auto slash = key.rfind('/'); auto file = slash == std::string::npos ? key : key.substr(slash + 1); auto dot = file.rfind('.');
        groups[dot == std::string::npos ? " " : file.substr(dot + 1)][slash == std::string::npos ? " " : key.substr(0, slash)]
            .push_back({dot == std::string::npos ? file : file.substr(0, dot), &payload});
    }
    Bytes tree, archive; auto text = [&](const std::string& s) { tree.insert(tree.end(), s.begin(), s.end()); tree.push_back(0); };
    for (const auto& [extension, directories] : groups) {
        text(extension);
        for (const auto& [folder, entries] : directories) {
            text(folder);
            for (const auto& [name, payload] : entries) {
                if (archive.size() + payload->size() > 0xFFFFFFFFu) throw std::runtime_error("Skin archive too large");
                text(name); append(tree, crc32(*payload), 4); append(tree, 0, 2); append(tree, archive_index, 2);
                append(tree, archive.size(), 4); append(tree, payload->size(), 4); append(tree, 0xFFFF, 2);
                archive.insert(archive.end(), payload->begin(), payload->end());
            }
            tree.push_back(0);
        }
        tree.push_back(0);
    }
    tree.push_back(0);
    Bytes chunks;
    for (std::size_t at = 0; at < archive.size(); at += 1 << 20) {
        auto size = std::min<std::size_t>(1 << 20, archive.size() - at);
        append(chunks, archive_index, 4); append(chunks, at, 4); append(chunks, size, 4);
        auto hash = md5(Bytes(archive.begin() + at, archive.begin() + at + size)); chunks.insert(chunks.end(), hash.begin(), hash.end());
    }
    Bytes out; append(out, 0x55AA1234, 4); append(out, 2, 4); append(out, tree.size(), 4); append(out, 0, 4);
    append(out, chunks.size(), 4); append(out, 48, 4); append(out, 0, 4);
    out.insert(out.end(), tree.begin(), tree.end()); out.insert(out.end(), chunks.begin(), chunks.end());
    auto tree_hash = md5(tree), chunks_hash = md5(chunks);
    out.insert(out.end(), tree_hash.begin(), tree_hash.end()); out.insert(out.end(), chunks_hash.begin(), chunks_hash.end());
    auto whole = md5(out); out.insert(out.end(), whole.begin(), whole.end());
    return {std::move(out), std::move(archive)};
}

inline std::map<std::string, Bytes> overrides(const VPK& pak, const std::string& loadout, const std::string& owned_text, std::vector<std::string>& notes) {
    auto result = plan(pak, choices(loadout), owned(owned_text)); notes = std::move(result.notes);
    std::map<std::string, Bytes> files;
    for (const auto& [to, from] : result.files) files[to] = pak.get(from);
    return files;
}
}
