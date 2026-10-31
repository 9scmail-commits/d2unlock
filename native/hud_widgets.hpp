#pragma once
// HUD widgets: the media player card and the clock / FPS pill. Pure drawing + interaction logic on top of
// hud::Canvas, so the same code runs inside Dota and in the offline preview harness.
#include "hud_render.hpp"
#include "hud_media.hpp"
#include <functional>

namespace hud {
constexpr float pi = 3.14159265f;
inline float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
inline float ease_out(float t) { t = clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
inline float ease_in_out(float t) { t = clamp01(t); return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2; }
inline float approach(float current, float target, float dt, float speed) { return target + (current - target) * std::exp(-speed * dt); }
inline Color approach(Color c, Color t, float dt, float speed) { float k = 1 - std::exp(-speed * dt); return mix(c, t, k); }

// Damped spring integrated in fixed sub-steps so it stays stable at any frame rate.
struct Spring {
    float x = 0, v = 0;
    float step(float target, float dt, float stiffness = 260, float damping = 24) {
        dt = std::min(dt, 0.1f); int n = std::max(1, int(std::ceil(dt * 480)));
        for (float h = dt / n; n--;) { v += (stiffness * (target - x) - damping * v) * h; x += v * h; }
        return x;
    }
};

struct Rect { float x = 0, y = 0, w = 0, h = 0; bool contains(float px, float py) const { return w > 0 && px >= x && py >= y && px < x + w && py < y + h; } };
struct Click { float x, y; bool down; };
struct Input { float x = -1e6f, y = -1e6f; std::vector<Click> clicks; };
struct FrameStats { float fps = 0, ms = 0; std::vector<float> history; };  // frame times in ms, newest last

struct Context {
    Canvas& canvas; Typesetter& type; ID3D11Device* device;
    float W, H, s;   // backbuffer size and UI scale (1.0 at 1080p)
    double now; float dt;
    const Input& input;
};

// Glyph-by-glyph text: digits share one advance so changing numbers never jitter sideways.
inline float glyph_advance(Typesetter& t, wchar_t ch, Face f, float size) {
    if (ch >= L'0' && ch <= L'9') { float w = 0; for (wchar_t d = L'0'; d <= L'9'; ++d) if (auto g = t.get(std::wstring(1, d), f, size)) w = std::max(w, g->width); return w; }
    auto g = t.get(std::wstring(1, ch), f, size); return g ? g->width : size * 0.3f;
}
inline float mono_width(Typesetter& t, const std::wstring& s, Face f, float size) { float w = 0; for (auto ch : s) w += glyph_advance(t, ch, f, size); return w; }
inline float mono(Canvas& c, Typesetter& t, const std::wstring& s, Face f, float size, float x, float cy, Color color) {
    for (auto ch : s) {
        float adv = glyph_advance(t, ch, f, size);
        if (auto g = t.get(std::wstring(1, ch), f, size)) draw_text_centered(c, g, x + (adv - g->width) / 2, cy, color);
        x += adv;
    }
    return x;
}

// Odometer-style digits: each slot rolls vertically when its character changes.
struct Roller {
    struct Slot { wchar_t now = 0, before = 0; double since = -100; float dir = 1; };
    std::vector<Slot> slots;
    void set(const std::wstring& text, double t, bool directional) {
        if (slots.size() != text.size()) {
            std::vector<Slot> next(text.size());
            for (size_t i = 0; i < next.size(); ++i) {  // keep slots aligned from the right (units stay units)
                size_t from_right = next.size() - 1 - i;
                if (from_right < slots.size()) next[i] = slots[slots.size() - 1 - from_right]; else next[i].since = t, next[i].before = L' ';
            }
            slots = std::move(next);
        }
        for (size_t i = 0; i < text.size(); ++i) {
            auto& s = slots[i]; if (s.now == text[i]) continue;
            s.dir = directional && s.now >= L'0' && s.now <= L'9' && text[i] < s.now ? -1.f : 1.f;
            s.before = s.now; s.now = text[i]; s.since = t;
        }
    }
    float width(Typesetter& t, Face f, float size) const { float w = 0; for (auto& s : slots) w += glyph_advance(t, s.now, f, size); return w; }
    void draw(Canvas& c, Typesetter& t, Face f, float size, float x, float cy, Color color, double now, float travel, const std::function<float(size_t)>& alpha = nullptr) const {
        for (size_t i = 0; i < slots.size(); ++i) {
            auto& s = slots[i]; float adv = glyph_advance(t, s.now, f, size), k = ease_out(float((now - s.since) / 0.38));
            Color col = alpha ? color.alpha(alpha(i)) : color;
            auto put = [&](wchar_t ch, float dy, float a) { if (ch && ch != L' ' && a > 0.003f) if (auto g = t.get(std::wstring(1, ch), f, size)) draw_text_centered(c, g, x + (adv - g->width) / 2, cy + dy, col.alpha(a)); };
            if (k < 1) { put(s.before, -s.dir * travel * k, 1 - k); put(s.now, s.dir * travel * (1 - k), k); }
            else put(s.now, 0, 1);
            x += adv;
        }
    }
};

// Shared behaviour: show/hide animation, placement, dragging, Windows-style resizing and press/hover tracking.
class Widget {
public:
    bool enabled = false, moved = false;
    float nx = -1, ny = -1;   // saved position as a fraction of the screen; negative = default spot
    float size = 0.8f;        // user scale on top of the resolution scale, changed by dragging an edge
    Rect bounds;              // area that captures the mouse this frame
    int cursor = 0;           // resize edges under the mouse (L|R|T|B), for the host's cursor shape
    static constexpr int L = 1, R = 2, T = 4, B = 8;
    static constexpr float min_size = 0.45f, max_size = 1.8f;
    float visibility() const { return vis_.x; }
    void reset_layout() { nx = ny = -1; size = 0.8f; placed_ = false; }

protected:
    enum { None = 0, Body = 1, Resize = 100 };   // Resize + edge mask
    Spring vis_, x_, y_, lift_, edge_glow_, grip_; bool placed_ = false, dragging_ = false;
    int pressed_ = None, hot_ = None; float grab_x_ = 0, grab_y_ = 0, press_x_ = 0, press_y_ = 0, w_ = 0, h_ = 0;
    float start_x_ = 0, start_y_ = 0, start_w_ = 1, start_h_ = 1, start_size_ = 1, anchor_x_ = 0, anchor_y_ = 0;
    std::vector<std::pair<int, Rect>> spots_;
    std::vector<std::pair<int, float>> activated_;

    int spot_at(float px, float py) const { for (auto i = spots_.rbegin(); i != spots_.rend(); ++i) if (i->second.contains(px, py)) return i->first; return None; }
    void spot(int id, Rect r) { spots_.push_back({id, r}); }
    bool interactive() const { return enabled && vis_.x > 0.85f; }
    bool resizing() const { return pressed_ >= Resize; }
    int hot(const Context&) const { return hot_; }
    float scale(const Context& c) const { return c.s * size; }

    // Runs before layout so the widget is measured with the size the user is dragging to right now.
    void resize_step(const Context& c) {
        if (!resizing()) return;
        const int edges = pressed_ - Resize;
        const float dx = c.input.x - press_x_, dy = c.input.y - press_y_;
        const float rw = (start_w_ + ((edges & R) ? dx : (edges & L) ? -dx : 0)) / start_w_;
        const float rh = (start_h_ + ((edges & B) ? dy : (edges & T) ? -dy : 0)) / start_h_;
        const bool horizontal = edges & (L | R), vertical = edges & (T | B);
        const float ratio = horizontal && vertical ? (rw + rh) / 2 : horizontal ? rw : rh;
        size = std::clamp(start_size_ * ratio, min_size, max_size);
        // The opposite edge stays put, exactly like a window.
        const float k = size / start_size_;
        anchor_x_ = (edges & L) ? start_x_ + start_w_ - start_w_ * k : start_x_;
        anchor_y_ = (edges & T) ? start_y_ + start_h_ - start_h_ * k : start_y_;
    }

    bool begin(Context& c, float w, float h, float default_x, float default_y) {
        w_ = w; h_ = h;
        vis_.step(enabled ? 1.f : 0.f, c.dt, enabled ? 150 : 220, enabled ? 17 : 30);
        if (!enabled && vis_.x < 0.004f) { vis_ = {}; bounds = {}; cursor = 0; pressed_ = None; dragging_ = false; spots_.clear(); placed_ = false; return false; }
        activated_.clear();
        for (auto& k : c.input.clicks) {
            if (k.down) {
                pressed_ = interactive() ? spot_at(k.x, k.y) : None; press_x_ = k.x; press_y_ = k.y;
                if (pressed_ == Body) { grab_x_ = k.x - x_.x; grab_y_ = k.y - y_.x; }
                if (resizing()) { start_x_ = anchor_x_ = x_.x; start_y_ = anchor_y_ = y_.x; start_w_ = std::max(w_, 1.f); start_h_ = std::max(h_, 1.f); start_size_ = size; }
            } else {
                if (dragging_ || resizing()) { nx = target_x(c) / c.W; ny = target_y(c) / c.H; moved = true; dragging_ = false; }
                else if (pressed_ && pressed_ == spot_at(k.x, k.y)) activated_.push_back({pressed_, k.x});
                pressed_ = None;
            }
        }
        if (pressed_ == Body && !dragging_ && std::hypot(c.input.x - press_x_, c.input.y - press_y_) > 4 * c.s) dragging_ = true;
        default_x_ = default_x; default_y_ = default_y;
        const float tx = target_x(c), ty = target_y(c);
        if (!placed_ || resizing()) { x_ = {tx, 0}; y_ = {ty, 0}; placed_ = true; }  // no lag while resizing: the anchored edge must not wobble
        else { x_.step(tx, c.dt, 700, 48); y_.step(ty, c.dt, 700, 48); }
        lift_.step(dragging_ ? 1.f : 0.f, c.dt, 300, 26);
        // Hover is resolved against last frame's hotspots, then they are rebuilt while drawing.
        const int under = spot_at(c.input.x, c.input.y);
        hot_ = interactive() && !dragging_ && (!pressed_ || pressed_ == under) ? under : None;
        cursor = resizing() ? pressed_ - Resize : (hot_ >= Resize && !pressed_) ? hot_ - Resize : 0;
        edge_glow_.step(cursor ? 1.f : 0.f, c.dt, 400, 30);
        grip_.step(hot_ != None || resizing() ? 1.f : 0.f, c.dt, 260, 26);
        spots_.clear();
        return true;
    }
    float target_x(const Context& c) const {
        const float m = 10 * c.s, t = resizing() ? anchor_x_ : dragging_ ? c.input.x - grab_x_ : nx >= 0 ? nx * c.W : default_x_;
        return std::clamp(t, m, std::max(m, c.W - w_ - m));
    }
    float target_y(const Context& c) const {
        const float m = 10 * c.s, t = resizing() ? anchor_y_ : dragging_ ? c.input.y - grab_y_ : ny >= 0 ? ny * c.H : default_y_;
        return std::clamp(t, m, std::max(m, c.H - h_ - m));
    }
    // Pop-in transform: rises, scales up and fades in on a spring with a little overshoot.
    void enter(Canvas& cv, float cx, float cy, float s) {
        float v = vis_.x, k = 0.92f + 0.08f * std::min(v, 1.04f) + 0.02f * lift_.x;
        cv.push(cx, cy, k, 0, (1 - std::min(v, 1.f)) * 22 * s - lift_.x * 3 * s, clamp01(v * 1.2f));
    }
    // Resize handles on every edge and corner (corners win), plus their visual feedback.
    void edges(Context& c, float x, float y, float W, float H, float radius) {
        auto& cv = c.canvas; const float t = 7 * c.s, g = edge_glow_.x;
        if (g > 0.01f) {
            cv.shadow(x, y, W, H, radius, 12 * c.s, white(0.07f * g));
            cv.border(x - 1.5f * c.s, y - 1.5f * c.s, W + 3 * c.s, H + 3 * c.s, radius + 1.5f * c.s, 2 * c.s, white(0.60f * g), white(0.35f * g));
        }
        if (grip_.x > 0.01f) {   // three-dot grip in the lower right corner
            const float a = 0.5f * grip_.x, d = 4.2f * c.s, gx = x + W - radius * 0.42f, gy = y + H - radius * 0.42f;
            cv.circle(gx, gy, 1.3f * c.s, white(a)); cv.circle(gx - d, gy, 1.3f * c.s, white(a)); cv.circle(gx, gy - d, 1.3f * c.s, white(a));
        }
        if (resizing()) {
            wchar_t b[16]; swprintf(b, 16, L"%d%%", int(std::lround(size * 125)));
            if (auto label = c.type.get(b, Face::Label, 13 * c.s)) {
                const float pw = label->width + 20 * c.s, ph = 24 * c.s, px = x + (W - pw) / 2, py = y - ph - 10 * c.s;
                cv.shadow(px, py + 4 * c.s, pw, ph, ph / 2, 10 * c.s, Color{0, 0, 0, 0.4f});
                cv.glass(px, py, pw, ph, ph / 2, Color{0.05f, 0.05f, 0.07f, 0.7f}, 0.8f);
                cv.border(px, py, pw, ph, ph / 2, 1 * c.s, white(0.25f), white(0.08f));
                draw_text_centered(cv, label, px + 10 * c.s, py + ph / 2, white(0.95f));
            }
        }
        spot(Resize + L, {x - t, y + t, 2 * t, H - 2 * t}); spot(Resize + R, {x + W - t, y + t, 2 * t, H - 2 * t});
        spot(Resize + T, {x + t, y - t, W - 2 * t, 2 * t}); spot(Resize + B, {x + t, y + H - t, W - 2 * t, 2 * t});
        const float k = 2.4f * t;
        spot(Resize + (L | T), {x - t, y - t, k, k}); spot(Resize + (R | T), {x + W - k + t, y - t, k, k});
        spot(Resize + (L | B), {x - t, y + H - k + t, k, k}); spot(Resize + (R | B), {x + W - k + t, y + H - k + t, k, k});
        bounds = interactive() || dragging_ || resizing() ? Rect{x - t, y - t, W + 2 * t, H + 2 * t} : Rect{};
    }
private:
    float default_x_ = 0, default_y_ = 0;
};

inline std::wstring clock_text(double seconds) {
    long t = long(std::max(0.0, seconds)); wchar_t b[32];
    if (t >= 3600) swprintf(b, 32, L"%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60); else swprintf(b, 32, L"%ld:%02ld", t / 60, t % 60);
    return b;
}

class Player : public Widget {
    enum { Prev = 2, Play = 3, Next = 4, Bar = 5 };
    std::unique_ptr<Texture> art_, old_art_; unsigned art_version_ = ~0u; float art_t_ = 1;
    std::wstring title_, artist_, old_title_, old_artist_; unsigned track_ = ~0u; float text_t_ = 1; double marquee_since_ = 0;
    Spring morph_, hover_[6], press_[6], nudge_prev_, nudge_next_, bar_;
    float playing_ = 0, progress_ = 0, presence_ = 0, ripple_ = 1;
    Color accent_ = rgb(150, 160, 255), accent_target_ = accent_;

    // Play <-> pause: two quads that are the halves of the triangle or the two bars.
    static void play_pause(Canvas& c, float cx, float cy, float size, float pause, Color col) {
        float t = ease_in_out(pause), h = size / 2;
        auto lerp = [&](Vec2 a, Vec2 b) { return Vec2{cx + (a.x + (b.x - a.x) * t) * size, cy + (a.y + (b.y - a.y) * t) * size}; };
        const float L = -0.36f, R = 0.56f, M = (L + R) / 2, top = -0.5f + 0.5f * (M - L) / (R - L);
        Vec2 left[4] = {lerp({L, -0.5f}, {-0.42f, -0.5f}), lerp({M, top}, {-0.12f, -0.5f}), lerp({M, -top}, {-0.12f, 0.5f}), lerp({L, 0.5f}, {-0.42f, 0.5f})};
        Vec2 right[4] = {lerp({M, top}, {0.12f, -0.5f}), lerp({R, 0}, {0.42f, -0.5f}), lerp({R, 0}, {0.42f, 0.5f}), lerp({M, -top}, {0.12f, 0.5f})};
        (void)h; c.polygon(left, 4, col); c.polygon(right, 4, col);
    }
    static void skip(Canvas& c, float cx, float cy, float size, bool forward, Color col) {
        float d = forward ? 1.f : -1.f;
        Vec2 tri[3] = {{cx - d * 0.42f * size, cy - 0.44f * size}, {cx + d * 0.26f * size, cy}, {cx - d * 0.42f * size, cy + 0.44f * size}};
        c.polygon(tri, 3, col);
        float bx = cx + d * 0.30f * size, bw = 0.15f * size; c.rect(std::min(bx, bx + d * bw), cy - 0.44f * size, bw, 0.88f * size, bw / 2, col);
    }
    void text_line(Context& c, const std::wstring& now_text, const std::wstring& old_text, Face face, float size, float x0, float x1, float top, Color col, float t, bool marquee) {
        auto& cv = c.canvas; float s = c.s;
        // Old line leaves first, the new one follows: they never sit on top of each other.
        const float out = ease_out(t / 0.45f), in = ease_out((t - 0.28f) / 0.72f);
        auto draw = [&](const std::wstring& text, float dy, float a, bool scroll) {
            if (text.empty() || a <= 0.003f) return;
            auto g = c.type.get(text, face, size); if (!g) return;
            float avail = x1 - x0, offset = 0, gap = 42 * s;
            if (scroll && g->width > avail) {
                float travel = g->width + gap, speed = 34 * s, run = travel / speed, cycle = 3.2f + run;
                float phase = float(std::fmod(std::max(0.0, c.now - marquee_since_), double(cycle)));
                offset = phase < 3.2f ? 0 : ease_in_out((phase - 3.2f) / run) * travel;
            }
            float lead = std::min(offset / (16 * s), 1.f);
            cv.clip(x0 - 18 * s * (1 - lead), top - 8 * s, x1, top + g->height + 8 * s, 14 * s);
            draw_text(cv, g, x0 - offset, top + dy, col.alpha(a));
            if (offset > 0) draw_text(cv, g, x0 - offset + g->width + gap, top + dy, col.alpha(a));
            cv.unclip();
        };
        draw(old_text, -10 * s * out, 1 - out, false);
        draw(now_text, 12 * s * (1 - in), in, marquee);
    }

public:
    void frame(Context& c, const media::Snapshot& m, const std::function<void(media::Command, double)>& send) {
        resize_step(c);
        const float s = scale(c), W = 404 * s, H = 132 * s, r = 26 * s;
        if (!begin(c, W, H, 22 * c.s, c.H * 0.30f)) return;
        auto& cv = c.canvas; const float dt = c.dt;

        for (auto [id, at] : activated_) {
            if (!m.session) continue;
            if (id == Play && m.can_toggle) { send(media::Command::Toggle, 0); ripple_ = 0; }
            if (id == Next && m.can_next) { send(media::Command::Next, 0); nudge_next_.v += 260; }
            if (id == Prev && m.can_prev) { send(media::Command::Previous, 0); nudge_prev_.v -= 260; }
            if (id == Bar && m.can_seek && m.duration > 0) { float k = clamp01((at - bar_x0_) / std::max(1.f, bar_x1_ - bar_x0_)); send(media::Command::Seek, k * m.duration); progress_ = k; }
        }

        // State that animates independently of the layout.
        if (m.track != track_) {
            const bool first = track_ == ~0u; track_ = m.track;
            auto title = m.session ? m.title : std::wstring(L"Ничего не играет"), artist = m.session ? m.artist : std::wstring(L"Spotify · Яндекс Музыка · браузер");
            if (title != title_ || artist != artist_) { old_title_ = title_; old_artist_ = artist_; title_ = title; artist_ = artist; text_t_ = first ? 1 : 0; marquee_since_ = c.now + 0.5; }
        }
        text_t_ = std::min(text_t_ + dt / 0.6f, 1.6f);
        if (m.art_version != art_version_) {
            art_version_ = m.art_version; old_art_ = std::move(art_); art_t_ = 0;
            if (m.art && !m.art->pixels.empty()) art_ = Texture::create(c.device, m.art->w, m.art->h, m.art->pixels.data());
            accent_target_ = m.art ? Color{m.art->accent[0], m.art->accent[1], m.art->accent[2], 1} : rgb(150, 160, 255);
        }
        art_t_ = std::min(art_t_ + dt / 0.6f, 1.f); if (art_t_ >= 1) old_art_.reset();
        accent_ = approach(accent_, accent_target_, dt, 3.5f);
        playing_ = approach(playing_, m.session && m.playing ? 1.f : 0.f, dt, 6);
        presence_ = approach(presence_, m.session ? 1.f : 0.f, dt, 6);
        morph_.step(m.playing ? 1.f : 0.f, dt, 340, 24);
        nudge_prev_.step(0, dt, 520, 22); nudge_next_.step(0, dt, 520, 22);
        ripple_ = std::min(ripple_ + dt / 0.6f, 1.f);
        const int hot_id = hot(c);
        for (int id = Body; id <= Bar; ++id) { hover_[id].step(hot_id == id ? 1.f : 0.f, dt, 380, 26); press_[id].step(pressed_ == id && hot_id == id ? 1.f : 0.f, dt, 600, 30); }
        const float card_hover = std::max({hover_[Body].x, hover_[Prev].x, hover_[Play].x, hover_[Next].x, hover_[Bar].x});

        const float x = x_.x, y = y_.x;
        enter(cv, x + W / 2, y + H / 2, s);
        const Color accent = accent_, light = mix(accent, white(1), 0.35f);
        const float breathe = 0.5f + 0.5f * std::sin(float(c.now) * 1.7f);

        // Atmosphere: coloured light from the cover, contact shadow, then the frosted glass card.
        cv.shadow(x + 6 * s, y + 18 * s, W * 0.5f, H - 24 * s, r, 52 * s, accent.alpha((0.20f + 0.10f * breathe * playing_) * (0.5f + 0.5f * presence_)));
        cv.shadow(x, y + 12 * s + lift_.x * 8 * s, W, H, r, (26 + 14 * lift_.x) * s, Color{0, 0, 0, 0.42f + 0.12f * lift_.x});
        cv.glass(x, y, W, H, r, Color{0.055f, 0.055f, 0.075f, 0.58f}, 0.82f, 1.55f);
        cv.horizontal(x, y, W, H, r, accent.alpha(0.20f), accent.alpha(0.0f));
        cv.vertical(x, y, W, H, r, white(0.075f), white(0.0f));
        cv.border(x, y, W, H, r, 1 * s, white(0.20f + 0.10f * card_hover), white(0.05f));
        spot(Body, {x, y, W, H});

        // Cover art with a coloured shadow, cross-fading between tracks.
        const float as = 100 * s, ax = x + 16 * s, ay = y + 16 * s, ar = 18 * s;
        cv.shadow(ax + 4 * s, ay + 10 * s, as - 8 * s, as - 6 * s, ar, 18 * s, accent.alpha(0.55f));
        cv.shadow(ax, ay + 3 * s, as, as, ar, 6 * s, Color{0, 0, 0, 0.35f});
        const float e = ease_out(art_t_), dim = 0.62f + 0.38f * std::max(playing_, 1 - presence_);
        auto cover = [&](const Texture* t, float scale, float a) {
            if (a <= 0.003f) return;
            cv.push(ax + as / 2, ay + as / 2, scale, 0, 0, a);
            if (t) cv.image(t, ax, ay, as, as, ar, Color{dim, dim, dim, 1});
            else {
                float hue = float(c.now) * 0.4f;
                cv.vertical(ax, ay, as, as, ar, mix(accent, white(1), 0.18f + 0.06f * std::sin(hue)), mix(accent, Color{0.05f, 0.05f, 0.09f, 1}, 0.72f));
                cv.shadow(ax + as * (0.3f + 0.2f * std::sin(hue)), ay + as * 0.25f, as * 0.4f, as * 0.4f, as * 0.2f, 18 * s, white(0.22f));
                if (auto g = c.type.get(L"♫", Face::Symbol, 40 * s)) draw_text(cv, g, ax + (as - g->width) / 2, ay + (as - g->height) / 2, white(0.92f));
            }
            cv.pop();
        };
        if (old_art_ || art_t_ < 1) cover(old_art_.get(), 1 - 0.06f * e, 1 - e);
        cover(art_.get(), 1.06f - 0.06f * e, art_ ? e : std::max(e, old_art_ ? e : 1.f));
        cv.border(ax, ay, as, as, ar, 1 * s, white(0.22f), white(0.04f));

        // Title, artist and a tiny equaliser.
        const float x0 = ax + as + 18 * s, x1 = x + W - 20 * s, eq_w = 16 * s;
        text_line(c, title_, old_title_, Face::Title, 18 * s, x0, x1 - eq_w - 10 * s, y + 18 * s, white(0.97f), text_t_, true);
        text_line(c, artist_, old_artist_, Face::Body, 14 * s, x0, x1, y + 43 * s, white(0.60f), text_t_ - 0.12f, true);
        {
            const float base = y + 33 * s, bw = 2.6f * s, gap = 1.9f * s;
            static const float f1[4] = {7.1f, 9.3f, 6.2f, 8.4f}, f2[4] = {2.3f, 3.1f, 1.7f, 2.7f}, ph[4] = {0.f, 1.7f, 3.1f, 4.6f};
            for (int i = 0; i < 4; ++i) {
                float t = float(c.now), a = 0.5f + 0.5f * std::sin(t * f1[i] + ph[i]), b = 0.55f + 0.45f * std::sin(t * f2[i] + ph[i] * 1.3f);
                float h = (2.6f + 11.f * a * b * playing_) * s, bx = x1 - eq_w + i * (bw + gap);
                cv.rect(bx, base - h, bw, h, bw / 2, mix(light, white(1), 0.2f).alpha(0.35f + 0.6f * presence_));
            }
        }

        // Transport controls.
        const float cy = y + 87 * s, enabled_alpha = 0.32f + 0.68f * presence_;
        auto press_scale = [&](int id) { return 1 + 0.07f * hover_[id].x - 0.12f * press_[id].x; };
        {
            const float cx = x0 + 52 * s, R = 19 * s * press_scale(Play);
            cv.circle(cx, cy + 2 * s, R + 9 * s * hover_[Play].x, light.alpha(0.75f * hover_[Play].x), 14 * s);
            if (ripple_ < 1) { float k = ease_out(ripple_); cv.border(cx - R - 16 * s * k, cy - R - 16 * s * k, 2 * (R + 16 * s * k), 2 * (R + 16 * s * k), R + 16 * s * k, 2 * s * (1 - k) + 0.5f, white(0.5f * (1 - k)), white(0.5f * (1 - k))); }
            cv.circle(cx, cy + 3 * s, R, Color{0, 0, 0, 0.35f}, 6 * s);
            cv.vertical(cx - R, cy - R, R * 2, R * 2, R, white(enabled_alpha), mix(white(1), light, 0.25f).alpha(enabled_alpha));
            play_pause(cv, cx, cy, 13 * s * press_scale(Play), morph_.x, Color{0.07f, 0.07f, 0.10f, 1});
            spot(Play, {cx - 22 * s, cy - 22 * s, 44 * s, 44 * s});
        }
        for (auto [id, cx, forward, nudge] : {std::tuple{Prev, x0 + 12 * s, false, nudge_prev_.x}, std::tuple{Next, x0 + 92 * s, true, nudge_next_.x}}) {
            float hv = hover_[id].x, k = press_scale(id), ok = (forward ? m.can_next : m.can_prev) || !m.session ? enabled_alpha : 0.3f;
            cv.circle(cx, cy, (11 + 6 * hv) * s * k, white(0.11f * hv), 0.5f);
            skip(cv, cx + nudge / 60 * s, cy, 15 * s * k, forward, white((0.74f + 0.26f * hv) * ok));
            spot(id, {cx - 18 * s, cy - 18 * s, 36 * s, 36 * s});
        }

        // Timeline: interpolated between updates, hover reveals a knob, click seeks.
        const bool timed = m.session && m.duration > 0.5;
        const float target = timed ? float(m.at(c.now) / m.duration) : 0;
        progress_ = std::abs(target - progress_) > 0.25f ? approach(progress_, target, dt, 9) : approach(progress_, target, dt, 22);
        bar_x0_ = x0; bar_x1_ = x1;
        const float by = y + H - 19 * s, hv = hover_[Bar].x, th = (4 + 2.5f * hv) * s, bw = x1 - x0;
        if (timed || presence_ < 0.99f) {
            const float a = timed ? 1.f : 0.35f;
            cv.rect(x0, by - th / 2, bw, th, th / 2, white(0.15f * a));
            const float scrubbing = pressed_ == Bar ? clamp01((c.input.x - x0) / bw) : -1;
            const float p = scrubbing >= 0 ? scrubbing : progress_, fill = std::max(th, bw * p);
            if (hv > 0.01f && scrubbing < 0) { float g = clamp01((c.input.x - x0) / bw); cv.rect(x0, by - th / 2, std::max(th, bw * g), th, th / 2, white(0.10f * hv)); }
            cv.shadow(x0, by - th / 2, fill, th, th / 2, 7 * s, accent.alpha(0.45f * a));
            cv.horizontal(x0, by - th / 2, fill, th, th / 2, accent.alpha(a), light.alpha(a));
            const float kr = (2.5f + 4.5f * std::max(hv, scrubbing >= 0 ? 1.f : 0.f)) * s;
            cv.circle(x0 + fill - th / 2 + th / 2, by, kr + 4 * s, light.alpha(0.35f * hv), 5 * s);
            cv.circle(x0 + fill, by, kr, white(a));
            spot(Bar, {x0 - 6 * s, by - 9 * s, bw + 12 * s, 16 * s});
            if (timed) {
                auto pos = clock_text((scrubbing >= 0 ? scrubbing : p) * m.duration), dur = clock_text(m.duration);
                const float ty = cy, size = 13 * s, wd = mono_width(c.type, L" / " + dur, Face::Label, size), wp = mono_width(c.type, pos, Face::Label, size);
                mono(cv, c.type, L" / " + dur, Face::Label, size, x1 - wd, ty, white(0.40f));
                mono(cv, c.type, pos, Face::Label, size, x1 - wd - wp, ty, white(0.86f));
            }
        }
        edges(c, x, y, W, H, r);
        cv.pop();
    }

private:
    float bar_x0_ = 0, bar_x1_ = 1;
};

inline const wchar_t* weekday_ru(int d) { static const wchar_t* n[] = {L"ВС", L"ПН", L"ВТ", L"СР", L"ЧТ", L"ПТ", L"СБ"}; return n[std::clamp(d, 0, 6)]; }
inline const wchar_t* month_ru(int m) { static const wchar_t* n[] = {L"ЯНВ", L"ФЕВ", L"МАР", L"АПР", L"МАЯ", L"ИЮН", L"ИЮЛ", L"АВГ", L"СЕН", L"ОКТ", L"НОЯ", L"ДЕК"}; return n[std::clamp(m - 1, 0, 11)]; }

class Clock : public Widget {
    Roller time_, fps_; Spring open_, width_; Color fps_color_ = rgb(110, 230, 170); float graph_scale_ = 16; double fps_shown_at_ = -1; int last_second_ = -1; double second_since_ = 0;
public:
    SYSTEMTIME fixed_time{};  // preview harness only: draws this instead of the wall clock when wYear != 0
    void frame(Context& c, const FrameStats& stats) {
        resize_step(c);
        const float s = scale(c), H = 64 * s, pad = 20 * s;
        SYSTEMTIME st; if (fixed_time.wYear) st = fixed_time; else GetLocalTime(&st);
        wchar_t hm[8]; swprintf(hm, 8, L"%02u:%02u", st.wHour, st.wMinute);
        time_.set(hm, c.now, false);
        if (c.now - fps_shown_at_ > 0.5) { fps_shown_at_ = c.now; fps_.set(std::to_wstring(std::clamp(int(std::lround(stats.fps)), 0, 999)), c.now, true); }
        if (st.wSecond != last_second_) { last_second_ = st.wSecond; second_since_ = c.now; }

        const float digit = 30 * s, ring_r = 11.5f * s;
        const float time_x = pad + ring_r * 2 + 14 * s, time_w = time_.width(c.type, Face::Digits, digit);
        const float divider_x = time_x + time_w + 20 * s, fps_x = divider_x + 20 * s, fps_w = fps_.width(c.type, Face::Digits, digit);
        const auto* fps_label = c.type.get(L"FPS", Face::Label, 12 * s);
        const float full = fps_x + 12 * s + fps_w + 6 * s + (fps_label ? fps_label->width : 20 * s) + pad + 4 * s;
        if (width_.x <= 0) width_.x = full;
        width_.step(full, c.dt, 260, 30);
        if (resizing()) width_ = {full, 0};
        if (!begin(c, width_.x, H, c.W - full - 26 * c.s, 62 * c.s)) { open_ = {}; return; }
        open_.step(enabled ? 1.f : 0.f, c.dt, 120, 17);
        auto& cv = c.canvas;
        const float x = x_.x, y = y_.x, open = clamp01(open_.x), W = H + (width_.x - H) * std::min(open_.x, 1.03f), r = H / 2;
        const float content = clamp01((open - 0.35f) / 0.65f);
        enter(cv, x + W / 2, y + H / 2, s);
        const int hot_id = hot(c);

        // Frame-time colour: mint when smooth, through amber, to red under 30 fps.
        const float f = stats.fps;
        Color target = f >= 100 ? rgb(96, 232, 176) : f >= 60 ? mix(rgb(244, 212, 92), rgb(96, 232, 176), (f - 60) / 40) : f >= 30 ? mix(rgb(255, 120, 84), rgb(244, 212, 92), (f - 30) / 30) : rgb(255, 84, 92);
        fps_color_ = approach(fps_color_, target, c.dt, 4);
        const Color ice = rgb(126, 200, 255);

        cv.shadow(x, y + 10 * s + lift_.x * 6 * s, W, H, r, (24 + 12 * lift_.x) * s, Color{0, 0, 0, 0.40f});
        cv.glass(x, y, W, H, r, Color{0.05f, 0.05f, 0.07f, 0.58f}, 0.82f, 1.5f);
        cv.horizontal(x, y, W, H, r, ice.alpha(0.10f), fps_color_.alpha(0.10f));
        cv.vertical(x, y, W, H, r, white(0.08f), white(0.0f));
        cv.border(x, y, W, H, r, 1 * s, white(0.20f + 0.08f * (hot_id == Body)), white(0.05f));
        spot(Body, {x, y, W, H});

        cv.push(0, 0, 1, 0, 0, content);
        cv.clip(x + 2 * s, y, x + W - r * 0.55f, y + H, 10 * s);

        // Frame-time graph behind the FPS block, newest sample on the right.
        if (!stats.history.empty()) {
            const float gx0 = x + divider_x + 6 * s, gx1 = x + W - r * 0.5f, gb = y + H - 5 * s, gh = 20 * s;
            const size_t n = std::min<size_t>(stats.history.size(), 96);
            float peak = 0; for (size_t i = stats.history.size() - n; i < stats.history.size(); ++i) peak = std::max(peak, stats.history[i]);
            graph_scale_ = approach(graph_scale_, std::max(peak * 1.25f, 1000.f / std::max(f, 1.f) * 2.2f), c.dt, 3);
            auto at = [&](size_t i) { float v = stats.history[stats.history.size() - n + i]; return Vec2{gx0 + (gx1 - gx0) * float(i) / float(std::max<size_t>(n - 1, 1)), gb - gh * std::min(v / graph_scale_, 1.f)}; };
            cv.clip(gx0, y, gx1, y + H, 18 * s);
            for (size_t i = 0; i + 1 < n; ++i) {
                auto a = at(i), b = at(i + 1);
                cv.quad(a, b, {b.x, gb}, {a.x, gb}, fps_color_.alpha(0.30f), fps_color_.alpha(0.30f), fps_color_.alpha(0.0f), fps_color_.alpha(0.0f));
            }
            for (size_t i = 0; i + 1 < n; ++i) cv.line(at(i), at(i + 1), 1.4f * s, fps_color_.alpha(0.85f));
            cv.unclip();
        }

        // Seconds ring with a glowing head, and a heartbeat dot in the middle.
        const float rx = x + pad + ring_r, ry = y + H / 2;
        const float sec = (st.wSecond + (fixed_time.wYear ? 0.f : float(st.wMilliseconds) / 1000.f)) / 60.f;
        cv.ring(rx, ry, ring_r, 2.6f * s, 1, white(0.13f));
        cv.ring(rx, ry, ring_r, 2.6f * s, std::max(sec, 0.001f), ice);
        if (sec < 0.02f) { float k = sec / 0.02f; cv.ring(rx, ry, ring_r, 2.6f * s, 1, ice.alpha(1 - k)); }
        { float a = sec * 2 * pi; float hx = rx + std::sin(a) * ring_r, hy = ry - std::cos(a) * ring_r; cv.circle(hx, hy, 4.5f * s, ice.alpha(0.55f), 4 * s); cv.circle(hx, hy, 2.1f * s, white(1)); }
        { float k = ease_out(float(c.now - second_since_) / 0.5f); cv.circle(rx, ry, (2.6f + 1.6f * (1 - k)) * s, white(0.35f + 0.5f * (1 - k))); }

        // HH:MM rolling digits with a breathing colon, date underneath.
        const float ty = y + 27 * s;
        cv.clip(x + time_x - 4 * s, ty - 15 * s, x + time_x + time_w + 4 * s, ty + 15 * s, 5 * s);
        const float colon = 0.35f + 0.65f * std::pow(0.5f + 0.5f * std::cos(2 * pi * float(std::fmod(c.now, 1.0))), 2.f);
        time_.draw(cv, c.type, Face::Digits, digit, x + time_x, ty, white(0.97f), c.now, 20 * s, [&](size_t i) { return i == 2 ? colon : 1.f; });
        cv.unclip();
        wchar_t date[32]; swprintf(date, 32, L"%ls, %u %ls", weekday_ru(st.wDayOfWeek), st.wDay, month_ru(st.wMonth));
        if (auto g = c.type.get(date, Face::Label, 12 * s)) draw_text_centered(cv, g, x + time_x + 1 * s, y + 48 * s, white(0.48f));

        cv.vertical(x + divider_x, y + 14 * s, 1 * s, H - 28 * s, 0, white(0.0f), white(0.16f));
        cv.vertical(x + divider_x, y + H / 2, 1 * s, H / 2 - 14 * s, 0, white(0.16f), white(0.0f));

        // FPS: status dot, rolling number, frame time.
        cv.circle(x + fps_x + 3.5f * s, ty, 6 * s, fps_color_.alpha(0.45f), 4 * s);
        cv.circle(x + fps_x + 3.5f * s, ty, 3.4f * s, fps_color_);
        const float nx0 = x + fps_x + 14 * s;
        cv.clip(nx0 - 4 * s, ty - 15 * s, nx0 + fps_w + 4 * s, ty + 15 * s, 5 * s);
        fps_.draw(cv, c.type, Face::Digits, digit, nx0, ty, mix(fps_color_, white(1), 0.55f), c.now, 20 * s);
        cv.unclip();
        if (fps_label) draw_text(cv, fps_label, nx0 + fps_w + 6 * s, ty - fps_label->baseline + fps_label->cap / 2 - 7 * s, white(0.55f));
        wchar_t ms[24]; swprintf(ms, 24, L"%.1f MS", stats.ms);
        if (auto g = c.type.get(ms, Face::Label, 12 * s)) draw_text_centered(cv, g, nx0, y + 48 * s, white(0.45f));

        cv.unclip(); cv.pop();
        edges(c, x, y, W, H, r);
        cv.pop();
    }
};
}
