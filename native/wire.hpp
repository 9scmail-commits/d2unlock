#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace wire {
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;
constexpr std::size_t max_message = 32 * 1024 * 1024;

inline void varint(Bytes& out, std::uint64_t value) {
    do { auto byte = static_cast<std::uint8_t>(value & 127); value >>= 7;
        out.push_back(byte | (value ? 128 : 0)); } while (value);
}
inline std::uint64_t read_varint(View in, std::size_t& at) {
    std::uint64_t value = 0;
    for (unsigned n = 0; n < 10; ++n) {
        if (at >= in.size()) throw std::runtime_error("truncated varint");
        auto byte = in[at++];
        if (n == 9 && byte > 1) throw std::runtime_error("varint overflow");
        value |= std::uint64_t(byte & 127) << (n * 7);
        if (!(byte & 128)) return value;
    }
    throw std::runtime_error("varint overflow");
}
inline std::uint64_t little(View in) {
    if (in.size() > 8) throw std::runtime_error("invalid fixed integer");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < in.size(); ++i) value |= std::uint64_t(in[i]) << (i * 8);
    return value;
}
inline void put_little(Bytes& out, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

struct Field {
    std::uint32_t number = 0;
    unsigned type = 0;
    std::uint64_t value = 0;
    Bytes data;
};

struct Message {
    std::vector<Field> fields;
    static Message parse(View in) {
        if (in.size() > max_message) throw std::runtime_error("message limit");
        Message result;
        std::size_t at = 0;
        while (at < in.size()) {
            auto tag = read_varint(in, at);
            if (!(tag >> 3) || (tag >> 3) > 0x1FFFFFFF) throw std::runtime_error("invalid field number");
            Field f; f.number = static_cast<std::uint32_t>(tag >> 3); f.type = tag & 7;
            if (f.type == 0) f.value = read_varint(in, at);
            else if (f.type == 1 || f.type == 5) {
                std::size_t size = f.type == 1 ? 8 : 4;
                if (size > in.size() - at) throw std::runtime_error("truncated fixed value");
                f.value = little(in.subspan(at, size)); at += size;
            } else if (f.type == 2) {
                auto size = read_varint(in, at);
                if (size > in.size() - at) throw std::runtime_error("truncated bytes");
                f.data.assign(in.begin() + at, in.begin() + at + size); at += size;
            } else throw std::runtime_error("unsupported protobuf wire type");
            result.fields.push_back(std::move(f));
            if (result.fields.size() > 200000) throw std::runtime_error("field count limit");
        }
        return result;
    }
    Bytes encode() const {
        Bytes out;
        for (const auto& f : fields) {
            varint(out, std::uint64_t(f.number) * 8 + f.type);
            if (f.type == 0) varint(out, f.value);
            else if (f.type == 1 || f.type == 5) put_little(out, f.value, f.type == 1 ? 8 : 4);
            else if (f.type == 2) { varint(out, f.data.size()); out.insert(out.end(), f.data.begin(), f.data.end()); }
            else throw std::runtime_error("unsupported protobuf wire type");
            if (out.size() > max_message) throw std::runtime_error("message limit");
        }
        return out;
    }
    std::uint64_t number(unsigned field, std::uint64_t fallback = 0) const {
        for (auto i = fields.rbegin(); i != fields.rend(); ++i)
            if (i->number == field && i->type != 2) return i->value;
        return fallback;
    }
    Bytes bytes(unsigned field) const {
        for (auto i = fields.rbegin(); i != fields.rend(); ++i)
            if (i->number == field && i->type == 2) return i->data;
        return {};
    }
    void erase(unsigned field) { std::erase_if(fields, [&](const Field& f) { return f.number == field; }); }
    void add(unsigned field, std::uint64_t value, unsigned type = 0) { fields.push_back({field, type, value, {}}); }
    void add_bytes(unsigned field, Bytes data) { fields.push_back({field, 2, 0, std::move(data)}); }
    void set(unsigned field, std::uint64_t value, unsigned type = 0) { erase(field); add(field, value, type); }
};

struct Packet {
    std::uint32_t type;
    Message header;
    Message body;
    static Packet parse(std::uint32_t type, View bytes) {
        if (!(type & 0x80000000u) || bytes.size() < 8 || bytes.size() > max_message)
            throw std::runtime_error("not a GC protobuf packet");
        if (little(bytes.first(4)) != type) throw std::runtime_error("GC type mismatch");
        auto size = little(bytes.subspan(4, 4));
        if (size > bytes.size() - 8) throw std::runtime_error("truncated GC header");
        return {type, Message::parse(bytes.subspan(8, size)), Message::parse(bytes.subspan(8 + size))};
    }
    Bytes encode() const {
        auto h = header.encode(), b = body.encode();
        if (h.size() + b.size() + 8 > max_message) throw std::runtime_error("packet limit");
        Bytes out; put_little(out, type, 4); put_little(out, h.size(), 4);
        out.insert(out.end(), h.begin(), h.end()); out.insert(out.end(), b.begin(), b.end()); return out;
    }
};

inline Packet packet(std::uint32_t type, Message body) { return {type | 0x80000000u, {}, std::move(body)}; }
inline Packet reply(std::uint32_t type, Message body, const Packet& request) {
    auto p = packet(type, std::move(body));
    p.header.add(13, 1);
    p.header.add(200, 3);
    auto job = request.header.number(10, UINT64_MAX);
    if (job != UINT64_MAX) p.header.add(11, job, 1);
    return p;
}
}
