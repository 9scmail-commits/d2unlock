#pragma once
// In-game HUD host: hooks the D3D11 swap chain inside Dota, feeds input and frame timing to the widgets
// and follows state/hud.cfg. Nothing is hooked until a widget is switched on in the launcher.
#include "hud_widgets.hpp"
#include <dxgi1_2.h>
#include <windowsx.h>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace hud {
struct Host {
    std::filesystem::path state;
    void (*log)(const std::string&) = nullptr;
    void (*persist)(const wchar_t*, std::string) = nullptr;
};

namespace detail {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

struct State {
    Host host;
    std::atomic_bool want_player{false}, want_clock{false}, hooked{false};
    PresentFn present = nullptr; Present1Fn present1 = nullptr; ResizeFn resize = nullptr;

    std::mutex render;  // everything below up to `input` belongs to the presenting thread
    IDXGISwapChain* chain = nullptr; double chain_seen = 0, last_frame = 0, collected = 0;
    HWND window = nullptr; WNDPROC original_proc = nullptr;
    ID3D11Device* device = nullptr; ID3D11DeviceContext* context = nullptr;
    Gpu gpu; Typesetter type; Canvas canvas; Player player; Clock clock; media::Bridge media;
    bool ready = false, failed = false;
    FrameStats stats; std::deque<double> recent;

    std::mutex input;   // shared with the window procedure (game main thread)
    std::vector<Click> clicks; std::vector<Rect> blocking; bool captured = false; float buffer_w = 1, buffer_h = 1;
    std::atomic_int cursor{0};   // resize edges under the mouse, see Widget::cursor
};
inline State& state() { static auto* s = new State; return *s; }  // never destroyed: hooks may fire during exit
inline void log(const std::string& text) { if (auto f = state().host.log) f("HUD: " + text); }

inline bool blocked(State& s, float x, float y) { for (auto& r : s.blocking) if (r.contains(x, y)) return true; return false; }
inline bool set_cursor(int edges) {
    if (!edges) return false;
    const bool h = edges & (Widget::L | Widget::R), v = edges & (Widget::T | Widget::B);
    const bool main_diagonal = edges == (Widget::L | Widget::T) || edges == (Widget::R | Widget::B);
    const WORD id = h && v ? (main_diagonal ? 32642 : 32643) : h ? 32644 : 32645;  // IDC_SIZENWSE, SIZENESW, SIZEWE, SIZENS
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(id)));
    return true;
}
inline LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto& s = state();
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK || m == WM_LBUTTONUP || m == WM_MOUSEMOVE) {
        RECT client{}; GetClientRect(h, &client);
        std::lock_guard g(s.input);
        float x = GET_X_LPARAM(l) * s.buffer_w / float(std::max<LONG>(1, client.right)), y = GET_Y_LPARAM(l) * s.buffer_h / float(std::max<LONG>(1, client.bottom));
        if (m != WM_MOUSEMOVE && m != WM_LBUTTONUP && blocked(s, x, y)) { s.captured = true; s.clicks.push_back({x, y, true}); return 0; }
        if (m == WM_LBUTTONUP && s.captured) { s.captured = false; s.clicks.push_back({x, y, false}); return 0; }
        // The game (SDL) also reads button state from WM_MOUSEMOVE flags; hide our press from it.
        if (m == WM_MOUSEMOVE && s.captured) w &= ~WPARAM(MK_LBUTTON);
    }
    // Windows-style resize cursors over widget edges; elsewhere the game keeps its own cursor.
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT && set_cursor(s.cursor)) return TRUE;
    if (m == WM_MOUSEMOVE) { auto result = CallWindowProcW(s.original_proc, h, m, w, l); set_cursor(s.cursor); return result; }
    if ((m == WM_KILLFOCUS || m == WM_CAPTURECHANGED) && s.captured) { std::lock_guard g(s.input); s.captured = false; s.clicks.push_back({-1e6f, -1e6f, false}); }
    return CallWindowProcW(s.original_proc, h, m, w, l);
}

inline std::string positions(State& s) {
    std::ostringstream out; out.precision(5);
    out << "player " << s.player.nx << ' ' << s.player.ny << ' ' << s.player.size << "\nclock " << s.clock.nx << ' ' << s.clock.ny << ' ' << s.clock.size << '\n';
    return out.str();
}

inline void frame(IDXGISwapChain* chain) {
    static thread_local bool inside = false; if (inside) return;
    auto& s = state(); std::unique_lock lock(s.render, std::try_to_lock); if (!lock) return;
    inside = true; struct Exit { ~Exit() { inside = false; } } exit_guard;
    const double now = media::clock();
    if (chain != s.chain) {
        if (s.chain && now - s.chain_seen < 1.0) return;  // a secondary swap chain while the main one is alive
        DXGI_SWAP_CHAIN_DESC d{}; if (FAILED(chain->GetDesc(&d)) || !d.OutputWindow || d.BufferDesc.Width < 640 || d.BufferDesc.Height < 360) return;
        DWORD pid = 0; GetWindowThreadProcessId(d.OutputWindow, &pid); if (pid != GetCurrentProcessId()) return;
        s.gpu.release_targets(); s.chain = chain; s.last_frame = 0; s.recent.clear();
        if (s.window != d.OutputWindow) {
            if (s.window && s.original_proc) SetWindowLongPtrW(s.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s.original_proc));
            s.window = d.OutputWindow;
            s.original_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(s.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
        }
        log("drawing on " + std::to_string(d.BufferDesc.Width) + "x" + std::to_string(d.BufferDesc.Height) + " swap chain, format " + std::to_string(int(d.BufferDesc.Format)));
    }
    s.chain_seen = now;

    // Frame timing straight from Present: the interval history feeds the graph, a 0.5 s window the counter.
    float dt = s.last_frame > 0 ? float(now - s.last_frame) : 1 / 60.f; s.last_frame = now;
    if (dt > 0 && dt < 1) { s.stats.history.push_back(dt * 1000); if (s.stats.history.size() > 240) s.stats.history.erase(s.stats.history.begin()); }
    s.recent.push_back(now); while (s.recent.size() > 2 && s.recent.front() < now - 0.5) s.recent.pop_front();
    if (s.recent.size() > 1) { double span = s.recent.back() - s.recent.front(); s.stats.fps = float((s.recent.size() - 1) / std::max(span, 1e-6)); s.stats.ms = 1000.f / std::max(s.stats.fps, 1.f); }

    const bool player = s.want_player, clock = s.want_clock;
    s.player.enabled = player; s.clock.enabled = clock; s.media.activate(player);
    if (!player && !clock && s.player.visibility() <= 0 && s.clock.visibility() <= 0) { std::lock_guard g(s.input); s.blocking.clear(); s.cursor = 0; return; }

    if (!s.ready) {
        if (s.failed) return;
        if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&s.device)))) { s.failed = true; log("the game is not running on Direct3D 11; widgets are unavailable (Vulkan?)"); return; }
        s.device->GetImmediateContext(&s.context);
        if (!s.gpu.init(s.device)) { s.failed = true; log("renderer failed: " + s.gpu.error); return; }
        if (!s.type.init(s.device)) { s.failed = true; log("text renderer failed"); return; }
        s.ready = true; log("renderer ready");
    }
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) return;
    D3D11_TEXTURE2D_DESC desc; buffer->GetDesc(&desc);

    Input input;
    {
        std::lock_guard g(s.input);
        s.buffer_w = float(desc.Width); s.buffer_h = float(desc.Height);
        // A release outside the window never reaches us: settle a stuck press from the real button state.
        if (s.captured && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { s.captured = false; s.clicks.push_back({-1e6f, -1e6f, false}); }
        input.clicks.swap(s.clicks);
    }
    POINT p{}; RECT client{};
    if (GetForegroundWindow() == s.window && GetCursorPos(&p) && ScreenToClient(s.window, &p) && GetClientRect(s.window, &client) && client.right > 0 && client.bottom > 0
        && p.x >= 0 && p.y >= 0 && p.x < client.right && p.y < client.bottom) {
        input.x = p.x * float(desc.Width) / client.right; input.y = p.y * float(desc.Height) / client.bottom;
    }

    s.canvas.reset(); s.type.now = now;
    const float W = float(desc.Width), H = float(desc.Height);
    Context c{s.canvas, s.type, s.device, W, H, std::clamp(H / 1080.f, 0.85f, 3.f), now, std::clamp(dt, 0.f, 0.1f), input};
    try {
        if (player || s.player.visibility() > 0) s.player.frame(c, s.media.snapshot(), [&s](media::Command cmd, double v) { s.media.send(cmd, v); });
        s.clock.frame(c, s.stats);
    } catch (...) { s.canvas.reset(); }
    { std::lock_guard g(s.input); s.blocking.clear(); for (auto* r : {&s.player.bounds, &s.clock.bounds}) if (r->w > 0) s.blocking.push_back(*r); }
    s.cursor = s.player.cursor ? s.player.cursor : s.clock.cursor;
    if ((s.player.moved || s.clock.moved) && s.host.persist) { s.player.moved = s.clock.moved = false; s.host.persist(L"hud.pos", positions(s)); }
    if (!s.gpu.draw(s.context, buffer, s.canvas)) log("draw failed: " + s.gpu.error);
    buffer->Release();
    if (now - s.collected > 5) { s.collected = now; s.type.collect(); }
}

inline HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* chain, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) try { frame(chain); } catch (...) {}
    return state().present(chain, sync, flags);
}
inline HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain1* chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    if (!(flags & DXGI_PRESENT_TEST)) try { frame(chain); } catch (...) {}
    return state().present1(chain, sync, flags, params);
}
inline HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain* chain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    auto& s = state();
    { std::lock_guard g(s.render); s.gpu.release_targets(); }  // our view keeps a back-buffer reference
    return s.resize(chain, count, w, h, format, flags);
}

inline bool patch(void** slot, void* hook, void** original) {
    if (*slot == hook) return true;
    DWORD old = 0; if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = *slot; InterlockedExchangePointer(slot, hook);
    DWORD ignored; VirtualProtect(slot, sizeof(void*), old, &ignored); FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

// The swap chain methods live in one vtable inside dxgi.dll shared by every swap chain of the process;
// a throwaway chain tells us where it is.
inline bool install() {
    if (!GetModuleHandleW(L"d3d11.dll") || !GetModuleHandleW(L"dxgi.dll")) return false;
    HWND dummy = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    if (!dummy) { log("helper window failed"); return false; }
    DXGI_SWAP_CHAIN_DESC d{}; d.BufferCount = 1; d.BufferDesc.Width = d.BufferDesc.Height = 64; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = dummy; d.SampleDesc.Count = 1; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* chain = nullptr; ID3D11Device* device = nullptr; ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &chain, &device, nullptr, &context);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &chain, &device, nullptr, &context);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        auto& s = state(); void** table = *reinterpret_cast<void***>(chain);
        ok = patch(&table[13], reinterpret_cast<void*>(&resize_hook), reinterpret_cast<void**>(&s.resize))
          && patch(&table[8], reinterpret_cast<void*>(&present_hook), reinterpret_cast<void**>(&s.present));
        IDXGISwapChain1* chain1 = nullptr;
        if (ok && SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&chain1))) && *reinterpret_cast<void***>(chain1) == table)
            patch(&table[22], reinterpret_cast<void*>(&present1_hook), reinterpret_cast<void**>(&s.present1));
        release(chain1);
        log(ok ? "swap chain hooked" : "swap chain hook failed");
    } else log("helper device failed: " + std::to_string(unsigned(hr)));
    release(context); release(chain); release(device); DestroyWindow(dummy);
    return ok;
}

inline std::string read_text(const std::filesystem::path& p) { std::ifstream in(p, std::ios::binary); std::ostringstream o; o << in.rdbuf(); return o.str(); }
inline std::string setting(const std::string& cfg, const char* key) {
    std::istringstream in(cfg); std::string line, prefix = std::string(key) + "=";
    while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size()); }
    return {};
}
inline bool flag(const std::string& cfg, const char* key) { return setting(cfg, key) == "1"; }

inline void watch() {
    // Keeps a multithreaded apartment alive so COM-based helpers (WIC) work from the render thread.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto& s = state(); std::string previous = "\x01"; unsigned failures = 0;
    {
        std::istringstream in(read_text(s.host.state / L"hud.pos")); std::string line;
        std::lock_guard g(s.render);
        while (std::getline(in, line)) {
            std::istringstream fields(line); std::string name; float x = -1, y = -1, size = 0.8f; fields >> name >> x >> y; if (!(fields >> size)) size = 0.8f;
            Widget* w = name == "player" ? static_cast<Widget*>(&s.player) : name == "clock" ? static_cast<Widget*>(&s.clock) : nullptr;
            if (w) { w->nx = x; w->ny = y; w->size = std::clamp(size, Widget::min_size, Widget::max_size); }
        }
    }
    for (;;) {
        std::string cfg; try { cfg = read_text(s.host.state / L"hud.cfg"); } catch (...) {}
        if (cfg != previous) {
            // "reset=<n>" from the launcher puts both widgets back to their default place and size.
            if (previous != "\x01" && setting(cfg, "reset") != setting(previous, "reset") && !setting(cfg, "reset").empty()) {
                std::lock_guard g(s.render); s.player.reset_layout(); s.clock.reset_layout();
                if (s.host.persist) s.host.persist(L"hud.pos", positions(s));
                log("layout reset");
            }
            previous = cfg; s.want_player = flag(cfg, "player"); s.want_clock = flag(cfg, "clock");
            log(std::string("player ") + (s.want_player ? "on" : "off") + ", clock " + (s.want_clock ? "on" : "off"));
        }
        if ((s.want_player || s.want_clock) && !s.hooked && failures < 5) { if (install()) s.hooked = true; else ++failures; }
        Sleep(400);
    }
}
}

inline void start(Host host) {
    auto& s = detail::state(); s.host = std::move(host);
    std::thread(detail::watch).detach();
}
}
