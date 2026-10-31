#pragma once
// Self-contained D3D11 2D renderer for the in-game HUD: signed-distance rounded shapes, analytic
// anti-aliased polygons, arcs, frosted-glass backdrop blur and DirectWrite text baked into textures.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hud {
template<class T> void release(T*& p) { if (p) p->Release(); p = nullptr; }

struct Color {
    float r = 0, g = 0, b = 0, a = 0;
    Color alpha(float k) const { return {r, g, b, a * k}; }
};
inline Color rgb(int r, int g, int b, float a = 1) { return {r / 255.f, g / 255.f, b / 255.f, a}; }
inline Color mix(Color x, Color y, float t) { return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t}; }
inline Color white(float a) { return {1, 1, 1, a}; }
struct Vec2 { float x = 0, y = 0; };

struct Texture {
    ID3D11Texture2D* texture = nullptr; ID3D11ShaderResourceView* view = nullptr; int w = 0, h = 0;
    Texture() = default; Texture(const Texture&) = delete; Texture& operator=(const Texture&) = delete;
    ~Texture() { release(view); release(texture); }
    static std::unique_ptr<Texture> create(ID3D11Device* device, int w, int h, const void* bgra) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = UINT(w); d.Height = UINT(h); d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA s{bgra, UINT(w * 4), 0};
        auto t = std::make_unique<Texture>(); t->w = w; t->h = h;
        if (FAILED(device->CreateTexture2D(&d, &s, &t->texture)) || FAILED(device->CreateShaderResourceView(t->texture, nullptr, &t->view))) return nullptr;
        return t;
    }
};

// One vertex carries everything the pixel shader needs to evaluate its shape analytically.
struct Vertex { float x, y, u, v, lx, ly, hw, hh, radius, feather, r, g, b, a, border, mode, opacity, fade, c0, c1, c2, c3; };
enum Mode { Solid = 0, Textured = 1, Backdrop = 2, Polygon = 3, Arc = 4 };

class Canvas {
public:
    struct Transform { float s = 1, tx = 0, ty = 0, opacity = 1; };
    struct Batch { ID3D11ShaderResourceView* view; std::uint32_t start, count; };
    std::vector<Vertex> vertices; std::vector<Batch> batches; bool wants_backdrop = false;

    void reset() { vertices.clear(); batches.clear(); stack_.assign(1, Transform{}); clips_.assign(1, Clip{}); wants_backdrop = false; }
    // Scales by `scale` around (ox, oy), then shifts by (dx, dy), all in the current coordinate space.
    void push(float ox, float oy, float scale, float dx, float dy, float opacity) {
        auto& t = stack_.back(); Transform n;
        n.s = t.s * scale; n.tx = (ox * (1 - scale) + dx) * t.s + t.tx; n.ty = (oy * (1 - scale) + dy) * t.s + t.ty; n.opacity = t.opacity * opacity;
        stack_.push_back(n);
    }
    void pop() { if (stack_.size() > 1) stack_.pop_back(); }
    void clip(float x0, float y0, float x1, float y1, float fade) {
        auto a = apply({x0, y0}), b = apply({x1, y1}); float s = scale();
        auto& c = clips_.back(); clips_.push_back({std::max(a.x, c.x0), std::max(a.y, c.y0), std::min(b.x, c.x1), std::min(b.y, c.y1), std::max(fade * s, c.fade)});
    }
    void unclip() { if (clips_.size() > 1) clips_.pop_back(); }
    float scale() const { return stack_.back().s; }
    float opacity() const { return stack_.back().opacity; }

    void rect(float x, float y, float w, float h, float radius, Color c, float feather = 0) { shape(x, y, w, h, radius, c, c, false, feather, 0, Solid, nullptr); }
    void vertical(float x, float y, float w, float h, float radius, Color top, Color bottom, float feather = 0) { shape(x, y, w, h, radius, top, bottom, false, feather, 0, Solid, nullptr); }
    void horizontal(float x, float y, float w, float h, float radius, Color left, Color right) { shape(x, y, w, h, radius, left, right, true, 0, 0, Solid, nullptr); }
    void border(float x, float y, float w, float h, float radius, float width, Color top, Color bottom) { shape(x, y, w, h, radius, top, bottom, false, 0, width, Solid, nullptr); }
    void shadow(float x, float y, float w, float h, float radius, float blur, Color c) { shape(x, y, w, h, radius, c, c, false, blur, 0, Solid, nullptr); }
    void circle(float cx, float cy, float r, Color c, float feather = 0) { shape(cx - r, cy - r, r * 2, r * 2, r, c, c, false, feather, 0, Solid, nullptr); }
    // tint.a is how much of the tint replaces the blurred game image.
    void glass(float x, float y, float w, float h, float radius, Color tint, float brightness = 1, float saturation = 1.35f) {
        wants_backdrop = true; shape(x, y, w, h, radius, tint, tint, false, 0, 0, Backdrop, nullptr, brightness, saturation);
    }
    void image(const Texture* t, float x, float y, float w, float h, float radius, Color tint = white(1)) { if (t) shape(x, y, w, h, radius, tint, tint, false, 0, 0, Textured, t->view); }
    void ring(float cx, float cy, float r, float thickness, float progress, Color c) {
        if (progress <= 0) return;
        auto p = apply({cx, cy}); float s = scale(); float e = (r + thickness) * s + 2;
        Vertex v = base(c, Arc); v.hw = r * s; v.hh = thickness * s; v.radius = std::min(progress, 1.f);
        emit_quad(v, p.x - e, p.y - e, p.x + e, p.y + e, -e, -e, e, e, nullptr, c, c, false);
    }
    // Convex polygon with a one pixel analytic fringe; duplicate points are tolerated (icon morphs).
    void polygon(const Vec2* points, int n, Color c) {
        std::array<Vec2, 24> p{}; int m = 0;
        for (int i = 0; i < n && m < 24; ++i) { auto q = apply(points[i]); if (m && std::hypot(q.x - p[m - 1].x, q.y - p[m - 1].y) < 0.02f) continue; p[m++] = q; }
        while (m > 2 && std::hypot(p[m - 1].x - p[0].x, p[m - 1].y - p[0].y) < 0.02f) --m;
        if (m < 3) return;
        float area = 0; for (int i = 0; i < m; ++i) { auto& a = p[i]; auto& b = p[(i + 1) % m]; area += a.x * b.y - b.x * a.y; }
        if (std::abs(area) < 0.05f) return;
        const float flip = area > 0 ? 1.f : -1.f;
        std::array<Vec2, 24> normal{};
        for (int i = 0; i < m; ++i) { auto& a = p[i]; auto& b = p[(i + 1) % m]; float dx = b.x - a.x, dy = b.y - a.y, l = std::hypot(dx, dy); normal[i] = {dy / l * flip, -dx / l * flip}; }
        std::array<Vec2, 24> inner{}, outer{};
        for (int i = 0; i < m; ++i) {
            auto& n0 = normal[(i + m - 1) % m]; auto& n1 = normal[i]; float nx = (n0.x + n1.x) / 2, ny = (n0.y + n1.y) / 2, d = std::max(nx * nx + ny * ny, 0.25f);
            nx /= d; ny /= d; inner[i] = {p[i].x - nx * 0.5f, p[i].y - ny * 0.5f}; outer[i] = {p[i].x + nx * 0.5f, p[i].y + ny * 0.5f};
        }
        Color clear = c.alpha(0);
        for (int i = 1; i + 1 < m; ++i) { tri(inner[0], c, inner[i], c, inner[i + 1], c); }
        for (int i = 0; i < m; ++i) { int j = (i + 1) % m; tri(inner[i], c, inner[j], c, outer[j], clear); tri(inner[i], c, outer[j], clear, outer[i], clear); }
    }
    void line(Vec2 a, Vec2 b, float width, Color c) {
        float dx = b.x - a.x, dy = b.y - a.y, l = std::hypot(dx, dy); if (l < 0.001f) return;
        float nx = -dy / l * width / 2, ny = dx / l * width / 2; Vec2 q[4] = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}}; polygon(q, 4, c);
    }
    // Plain quad with per-corner colours and no fringe: neighbouring strips share edges seamlessly.
    void quad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color ca, Color cb, Color cc, Color cd) {
        auto pa = apply(a), pb = apply(b), pc = apply(c), pd = apply(d); tri(pa, ca, pb, cb, pc, cc); tri(pa, ca, pc, cc, pd, cd);
    }
    Vec2 apply(Vec2 p) const { auto& t = stack_.back(); return {p.x * t.s + t.tx, p.y * t.s + t.ty}; }

private:
    struct Clip { float x0 = -1e6f, y0 = -1e6f, x1 = 1e6f, y1 = 1e6f, fade = 0; };
    std::vector<Transform> stack_{Transform{}};
    std::vector<Clip> clips_{Clip{}};

    Vertex base(Color c, int mode) const {
        auto& k = clips_.back(); Vertex v{}; v.r = c.r; v.g = c.g; v.b = c.b; v.a = c.a; v.mode = float(mode); v.opacity = opacity();
        v.fade = k.fade; v.c0 = k.x0; v.c1 = k.y0; v.c2 = k.x1; v.c3 = k.y1; return v;
    }
    void use(ID3D11ShaderResourceView* view, std::uint32_t count) {
        if (batches.empty() || (view && batches.back().view && batches.back().view != view)) batches.push_back({view, std::uint32_t(vertices.size()), 0});
        if (view) batches.back().view = view;
        batches.back().count += count;
    }
    void tri(Vec2 a, Color ca, Vec2 b, Color cb, Vec2 c, Color cc) {
        use(nullptr, 3);
        for (auto [p, col] : {std::pair{a, ca}, std::pair{b, cb}, std::pair{c, cc}}) { Vertex v = base(col, Polygon); v.x = p.x; v.y = p.y; vertices.push_back(v); }
    }
    void emit_quad(Vertex v, float x0, float y0, float x1, float y1, float l0, float t0, float l1, float t1, ID3D11ShaderResourceView* view, Color c0, Color c1, bool horizontal, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
        use(view, 6);
        auto corner = [&](float x, float y, float lx, float ly, float u, float vv, bool second) {
            Vertex q = v; q.x = x; q.y = y; q.lx = lx; q.ly = ly;
            if (v.mode != float(Backdrop)) { q.u = u; q.v = vv; }
            Color c = second ? c1 : c0; q.r = c.r; q.g = c.g; q.b = c.b; q.a = c.a; return q;
        };
        Vertex tl = corner(x0, y0, l0, t0, u0, v0, false), tr = corner(x1, y0, l1, t0, u1, v0, horizontal), br = corner(x1, y1, l1, t1, u1, v1, true), bl = corner(x0, y1, l0, t1, u0, v1, !horizontal);
        vertices.insert(vertices.end(), {tl, tr, br, tl, br, bl});
    }
    void shape(float x, float y, float w, float h, float radius, Color c0, Color c1, bool horizontal, float feather, float border, int mode, ID3D11ShaderResourceView* view, float b0 = 0, float b1 = 0) {
        if (w <= 0 || h <= 0) return;
        float s = scale(); auto a = apply({x, y}), b = apply({x + w, y + h});
        if (mode == Textured && std::abs(s - 1) < 1e-3f) { float sx = std::round(a.x) - a.x, sy = std::round(a.y) - a.y; a.x += sx; a.y += sy; b.x += sx; b.y += sy; }  // crisp text
        float hw = (b.x - a.x) / 2, hh = (b.y - a.y) / 2, cx = a.x + hw, cy = a.y + hh, f = std::max(feather * s, 0.5f), e = f + 1.5f;
        Vertex v = base(c0, mode); v.hw = hw; v.hh = hh; v.radius = std::min({radius * s, hw, hh}); v.feather = f; v.border = border * s;
        if (mode == Backdrop) { v.u = b0; v.v = b1; }
        float u0 = -e / (2 * hw), v0 = -e / (2 * hh);
        emit_quad(v, cx - hw - e, cy - hh - e, cx + hw + e, cy + hh + e, -hw - e, -hh - e, hw + e, hh + e, view, c0, c1, horizontal, u0, v0, 1 - u0, 1 - v0);
    }
};

inline const char* shader_source = R"(
cbuffer Frame : register(b0) { float2 screen; float linear_out; float pad0; float4 blur; };
struct VIn { float2 pos:POSITION; float2 uv:TEXCOORD0; float2 local:TEXCOORD1; float4 shape:TEXCOORD2; float4 color:COLOR0; float4 params:TEXCOORD3; float4 clip:TEXCOORD4; };
struct PIn { float4 pos:SV_Position; float2 uv:TEXCOORD0; float2 local:TEXCOORD1; float4 shape:TEXCOORD2; float4 color:COLOR0; float4 params:TEXCOORD3; float4 clip:TEXCOORD4; float2 px:TEXCOORD5; };
Texture2D tex : register(t0); Texture2D back : register(t1); SamplerState smp : register(s0);
PIn vs_main(VIn i) {
    PIn o; o.pos = float4(i.pos.x / screen.x * 2 - 1, 1 - i.pos.y / screen.y * 2, 0, 1);
    o.uv = i.uv; o.local = i.local; o.shape = i.shape; o.color = i.color; o.params = i.params; o.clip = i.clip; o.px = i.pos; return o;
}
float box(float2 p, float2 b, float r) { float2 q = abs(p) - b + r; return length(max(q, 0)) + min(max(q.x, q.y), 0) - r; }
float edge(float d, float f) { return f <= 0.51 ? saturate(0.5 - d) : 1 - smoothstep(-f, f, d); }
float4 ps_main(PIn i) : SV_Target {
    int mode = (int)(i.params.y + 0.5);
    float cover = 1;
    if (mode <= 2) {
        float d = box(i.local, i.shape.xy, i.shape.z);
        cover = edge(d, i.shape.w);
        if (i.params.x > 0) cover = saturate(cover - edge(d + i.params.x, i.shape.w));
    } else if (mode == 4) {
        float2 p = i.local; float R = i.shape.x, th = i.shape.y, end = i.shape.z * 6.2831853;
        float a = atan2(p.x, -p.y); if (a < 0) a += 6.2831853;
        float d;
        if (a <= end) d = abs(length(p) - R) - th * 0.5;
        else { float2 s = float2(0, -R), e = float2(sin(end), -cos(end)) * R; d = min(length(p - s), length(p - e)) - th * 0.5; }
        cover = saturate(0.5 - d);
    }
    float f = max(i.params.w, 0.75);
    cover *= saturate((i.px.x - i.clip.x) / f) * saturate((i.clip.z - i.px.x) / f) * saturate((i.px.y - i.clip.y) / f) * saturate((i.clip.w - i.px.y) / f);
    float4 o;
    if (mode == 1) { o = tex.Sample(smp, i.uv) * float4(i.color.rgb, 1) * i.color.a; }
    else if (mode == 2) {
        float3 b = back.Sample(smp, i.px / screen).rgb;
        if (linear_out > 0.5) b = pow(max(b, 0), 1 / 2.2);
        float l = dot(b, float3(0.299, 0.587, 0.114));
        b = lerp(l.xxx, b, i.uv.y) * i.uv.x;
        o = float4(lerp(b, i.color.rgb, i.color.a), 1);
    } else o = float4(i.color.rgb * i.color.a, i.color.a);
    o *= cover * i.params.z;
    if (linear_out > 0.5 && o.a > 0) o.rgb = pow(max(o.rgb / o.a, 0), 2.2) * o.a;
    return o;
}
struct QIn { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
QIn vs_quad(uint id : SV_VertexID) { QIn o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
float4 ps_down(QIn i) : SV_Target {
    float2 t = blur.xy;
    return (tex.Sample(smp, i.uv + t * float2(-1, -1)) + tex.Sample(smp, i.uv + t * float2(1, -1)) + tex.Sample(smp, i.uv + t * float2(-1, 1)) + tex.Sample(smp, i.uv + t * float2(1, 1))) * 0.25;
}
float4 ps_blur(QIn i) : SV_Target {
    float2 d = blur.zw;
    float4 c = tex.Sample(smp, i.uv) * 0.2270270270;
    c += (tex.Sample(smp, i.uv + d * 1.3846153846) + tex.Sample(smp, i.uv - d * 1.3846153846)) * 0.3162162162;
    c += (tex.Sample(smp, i.uv + d * 3.2307692308) + tex.Sample(smp, i.uv - d * 3.2307692308)) * 0.0702702703;
    return c;
}
)";

// Snapshot of every pipeline binding the HUD touches, restored after drawing so the game never notices.
struct SavedState {
    UINT scissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{}; D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11RasterizerState* rs = nullptr; ID3D11BlendState* blend = nullptr; FLOAT factor[4]{}; UINT mask = 0; ID3D11DepthStencilState* depth = nullptr; UINT ref = 0;
    ID3D11ShaderResourceView* srv[2]{}; ID3D11SamplerState* sampler = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11VertexShader* vs = nullptr; ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr; ID3D11Buffer* vcb = nullptr; ID3D11Buffer* pcb = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology{}; ID3D11Buffer* ib = nullptr; DXGI_FORMAT ib_format{}; UINT ib_offset = 0; ID3D11Buffer* vb = nullptr; UINT stride = 0, offset = 0; ID3D11InputLayout* layout = nullptr;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{}; ID3D11DepthStencilView* dsv = nullptr;
    void save(ID3D11DeviceContext* c) {
        c->RSGetScissorRects(&scissors, scissor); c->RSGetViewports(&viewports, viewport); c->RSGetState(&rs);
        c->OMGetBlendState(&blend, factor, &mask); c->OMGetDepthStencilState(&depth, &ref); c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->PSGetShaderResources(0, 2, srv); c->PSGetSamplers(0, 1, &sampler); c->PSGetShader(&ps, nullptr, nullptr); c->VSGetShader(&vs, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr); c->HSGetShader(&hs, nullptr, nullptr); c->DSGetShader(&ds, nullptr, nullptr);
        c->VSGetConstantBuffers(0, 1, &vcb); c->PSGetConstantBuffers(0, 1, &pcb); c->IAGetPrimitiveTopology(&topology);
        c->IAGetIndexBuffer(&ib, &ib_format, &ib_offset); c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset); c->IAGetInputLayout(&layout);
    }
    void restore(ID3D11DeviceContext* c) {
        c->RSSetScissorRects(scissors, scissor); c->RSSetViewports(viewports, viewport); c->RSSetState(rs);
        c->OMSetBlendState(blend, factor, mask); c->OMSetDepthStencilState(depth, ref); c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->PSSetShaderResources(0, 2, srv); c->PSSetSamplers(0, 1, &sampler); c->PSSetShader(ps, nullptr, 0); c->VSSetShader(vs, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0); c->HSSetShader(hs, nullptr, 0); c->DSSetShader(ds, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &vcb); c->PSSetConstantBuffers(0, 1, &pcb); c->IASetPrimitiveTopology(topology);
        c->IASetIndexBuffer(ib, ib_format, ib_offset); c->IASetVertexBuffers(0, 1, &vb, &stride, &offset); c->IASetInputLayout(layout);
        release(rs); release(blend); release(depth); for (auto& v : rtv) release(v); release(dsv); for (auto& v : srv) release(v); release(sampler);
        release(ps); release(vs); release(gs); release(hs); release(ds); release(vcb); release(pcb); release(ib); release(vb); release(layout);
    }
};

class Gpu {
    ID3D11Device* device_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr; ID3D11VertexShader* quad_vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr; ID3D11PixelShader* down_ps_ = nullptr; ID3D11PixelShader* blur_ps_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr; ID3D11Buffer* vb_ = nullptr; ID3D11Buffer* cb_ = nullptr; UINT vb_capacity_ = 0;
    ID3D11BlendState* blend_ = nullptr; ID3D11BlendState* opaque_ = nullptr; ID3D11RasterizerState* raster_ = nullptr; ID3D11DepthStencilState* depth_ = nullptr; ID3D11SamplerState* sampler_ = nullptr;
    // Backdrop chain: copy of the frame -> quarter resolution -> separable gaussian ping-pong.
    ID3D11Texture2D* copy_ = nullptr; ID3D11ShaderResourceView* copy_view_ = nullptr; DXGI_FORMAT copy_format_{}; UINT copy_w_ = 0, copy_h_ = 0;
    ID3D11Texture2D* small_[2]{}; ID3D11ShaderResourceView* small_view_[2]{}; ID3D11RenderTargetView* small_rtv_[2]{}; UINT small_w_ = 0, small_h_ = 0;
    ID3D11RenderTargetView* target_ = nullptr; ID3D11Texture2D* target_texture_ = nullptr;

    struct Constants { float w, h, linear, pad; float blur[4]; };

    static ID3DBlob* compile(const char* entry, const char* profile, std::string& error) {
        static auto compiler = [] {
            for (auto name : {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"}) if (auto m = LoadLibraryW(name)) return m;
            return HMODULE(nullptr);
        }();
        if (!compiler) { error = "d3dcompiler missing"; return nullptr; }
        auto fn = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")));
        if (!fn) { error = "D3DCompile missing"; return nullptr; }
        ID3DBlob* code = nullptr; ID3DBlob* messages = nullptr;
        auto hr = fn(shader_source, std::strlen(shader_source), "hud", nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
        if (FAILED(hr)) { error = messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize()) : "compile failed"; release(messages); release(code); return nullptr; }
        release(messages); return code;
    }
    void release_backdrop() { release(copy_view_); release(copy_); for (int i = 0; i < 2; ++i) { release(small_rtv_[i]); release(small_view_[i]); release(small_[i]); } copy_w_ = copy_h_ = small_w_ = small_h_ = 0; }

public:
    std::string error;
    ~Gpu() { shutdown(); }
    ID3D11Device* device() const { return device_; }
    bool init(ID3D11Device* device) {
        shutdown(); device_ = device; device_->AddRef();
        ID3DBlob* vs = compile("vs_main", "vs_4_0", error); if (!vs) return false;
        ID3DBlob* blobs[4] = {compile("ps_main", "ps_4_0", error), compile("vs_quad", "vs_4_0", error), compile("ps_down", "ps_4_0", error), compile("ps_blur", "ps_4_0", error)};
        bool ok = blobs[0] && blobs[1] && blobs[2] && blobs[3];
        if (ok) {
            D3D11_INPUT_ELEMENT_DESC e[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}, {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}, {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0}, {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 72, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            ok = SUCCEEDED(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_))
              && SUCCEEDED(device_->CreateInputLayout(e, 7, vs->GetBufferPointer(), vs->GetBufferSize(), &layout_))
              && SUCCEEDED(device_->CreatePixelShader(blobs[0]->GetBufferPointer(), blobs[0]->GetBufferSize(), nullptr, &ps_))
              && SUCCEEDED(device_->CreateVertexShader(blobs[1]->GetBufferPointer(), blobs[1]->GetBufferSize(), nullptr, &quad_vs_))
              && SUCCEEDED(device_->CreatePixelShader(blobs[2]->GetBufferPointer(), blobs[2]->GetBufferSize(), nullptr, &down_ps_))
              && SUCCEEDED(device_->CreatePixelShader(blobs[3]->GetBufferPointer(), blobs[3]->GetBufferSize(), nullptr, &blur_ps_));
            if (!ok) error = "shader objects";
        }
        release(vs); for (auto& b : blobs) release(b);
        if (!ok) return false;
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = sizeof(Constants); cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_BLEND_DESC bd{}; auto& t = bd.RenderTarget[0]; t.BlendEnable = TRUE; t.SrcBlend = D3D11_BLEND_ONE; t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOp = D3D11_BLEND_OP_ADD;
        t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        D3D11_BLEND_DESC od{}; od.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateBuffer(&cb, nullptr, &cb_)) || FAILED(device_->CreateBlendState(&bd, &blend_)) || FAILED(device_->CreateBlendState(&od, &opaque_))
            || FAILED(device_->CreateRasterizerState(&rd, &raster_)) || FAILED(device_->CreateDepthStencilState(&dd, &depth_)) || FAILED(device_->CreateSamplerState(&sd, &sampler_))) { error = "pipeline states"; return false; }
        return true;
    }
    void release_targets() { release(target_); release(target_texture_); release_backdrop(); }
    void shutdown() {
        release_targets(); release(vs_); release(quad_vs_); release(ps_); release(down_ps_); release(blur_ps_); release(layout_); release(vb_); release(cb_);
        release(blend_); release(opaque_); release(raster_); release(depth_); release(sampler_); vb_capacity_ = 0; release(device_);
    }

    // Draws the canvas onto `backbuffer`. Everything the game had bound is restored afterwards.
    bool draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, const Canvas& canvas) {
        if (canvas.vertices.empty()) return true;
        D3D11_TEXTURE2D_DESC desc; backbuffer->GetDesc(&desc);
        if (target_texture_ != backbuffer) {
            release(target_); release(target_texture_);
            D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = desc.Format;
            rd.ViewDimension = desc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(device_->CreateRenderTargetView(backbuffer, &rd, &target_))) { error = "render target"; return false; }
            target_texture_ = backbuffer; backbuffer->AddRef();
        }
        // Float and sRGB targets expect linear light; the shader converts its gamma-space colours.
        const bool linear = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        UINT bytes = UINT(canvas.vertices.size() * sizeof(Vertex));
        if (bytes > vb_capacity_) {
            release(vb_); vb_capacity_ = std::max<UINT>(bytes * 2, 256 * 1024);
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = vb_capacity_; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device_->CreateBuffer(&bd, nullptr, &vb_))) { vb_capacity_ = 0; error = "vertex buffer"; return false; }
        }
        SavedState saved; saved.save(ctx);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { std::memcpy(m.pData, canvas.vertices.data(), bytes); ctx->Unmap(vb_, 0); }
        ID3D11ShaderResourceView* backdrop = canvas.wants_backdrop ? blur(ctx, backbuffer, desc) : nullptr;
        set_constants(ctx, float(desc.Width), float(desc.Height), linear, {0, 0, 0, 0});
        D3D11_VIEWPORT vp{0, 0, float(desc.Width), float(desc.Height), 0, 1};
        UINT stride = sizeof(Vertex), offset = 0; const float factor[4]{};
        ctx->OMSetRenderTargets(1, &target_, nullptr); ctx->OMSetBlendState(blend_, factor, 0xffffffff); ctx->OMSetDepthStencilState(depth_, 0);
        ctx->RSSetViewports(1, &vp); ctx->RSSetState(raster_);
        ctx->IASetInputLayout(layout_); ctx->IASetVertexBuffers(0, 1, &vb_, &stride, &offset); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        ctx->VSSetShader(vs_, nullptr, 0); ctx->PSSetShader(ps_, nullptr, 0); ctx->GSSetShader(nullptr, nullptr, 0); ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb_); ctx->PSSetConstantBuffers(0, 1, &cb_); ctx->PSSetSamplers(0, 1, &sampler_);
        for (auto& b : canvas.batches) {
            ID3D11ShaderResourceView* views[2] = {b.view, backdrop};
            ctx->PSSetShaderResources(0, 2, views); ctx->Draw(b.count, b.start);
        }
        saved.restore(ctx);
        return true;
    }

private:
    void set_constants(ID3D11DeviceContext* ctx, float w, float h, bool linear, std::array<float, 4> blur) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { Constants c{w, h, linear ? 1.f : 0.f, 0, {blur[0], blur[1], blur[2], blur[3]}}; std::memcpy(m.pData, &c, sizeof(c)); ctx->Unmap(cb_, 0); }
    }
    ID3D11ShaderResourceView* blur(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc) {
        if (copy_w_ != desc.Width || copy_h_ != desc.Height || copy_format_ != desc.Format) {
            release_backdrop();
            D3D11_TEXTURE2D_DESC c = desc; c.SampleDesc = {1, 0}; c.Usage = D3D11_USAGE_DEFAULT; c.BindFlags = D3D11_BIND_SHADER_RESOURCE; c.CPUAccessFlags = 0; c.MiscFlags = 0; c.MipLevels = 1; c.ArraySize = 1;
            if (FAILED(device_->CreateTexture2D(&c, nullptr, &copy_)) || FAILED(device_->CreateShaderResourceView(copy_, nullptr, &copy_view_))) { release_backdrop(); return nullptr; }
            small_w_ = std::max(1u, desc.Width / 4); small_h_ = std::max(1u, desc.Height / 4);
            D3D11_TEXTURE2D_DESC s{}; s.Width = small_w_; s.Height = small_h_; s.MipLevels = 1; s.ArraySize = 1; s.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; s.SampleDesc.Count = 1;
            s.Usage = D3D11_USAGE_DEFAULT; s.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            for (int i = 0; i < 2; ++i)
                if (FAILED(device_->CreateTexture2D(&s, nullptr, &small_[i])) || FAILED(device_->CreateShaderResourceView(small_[i], nullptr, &small_view_[i])) || FAILED(device_->CreateRenderTargetView(small_[i], nullptr, &small_rtv_[i]))) { release_backdrop(); return nullptr; }
            copy_w_ = desc.Width; copy_h_ = desc.Height; copy_format_ = desc.Format;
        }
        if (desc.SampleDesc.Count > 1) ctx->ResolveSubresource(copy_, 0, backbuffer, 0, desc.Format); else ctx->CopyResource(copy_, backbuffer);
        const float factor[4]{}; D3D11_VIEWPORT vp{0, 0, float(small_w_), float(small_h_), 0, 1};
        ctx->OMSetBlendState(opaque_, factor, 0xffffffff); ctx->OMSetDepthStencilState(depth_, 0); ctx->RSSetState(raster_); ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(quad_vs_, nullptr, 0); ctx->GSSetShader(nullptr, nullptr, 0); ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb_); ctx->PSSetSamplers(0, 1, &sampler_);
        auto pass = [&](ID3D11PixelShader* ps, ID3D11ShaderResourceView* from, int to, std::array<float, 4> k) {
            ID3D11ShaderResourceView* none[2]{}; ctx->PSSetShaderResources(0, 2, none);
            set_constants(ctx, float(small_w_), float(small_h_), false, k);
            ctx->OMSetRenderTargets(1, &small_rtv_[to], nullptr); ctx->PSSetShader(ps, nullptr, 0); ctx->PSSetShaderResources(0, 1, &from); ctx->Draw(3, 0);
        };
        const float sx = 1.f / small_w_, sy = 1.f / small_h_;
        pass(down_ps_, copy_view_, 0, {1.f / desc.Width, 1.f / desc.Height, 0, 0});
        for (float spread : {1.f, 2.f}) {
            pass(blur_ps_, small_view_[0], 1, {0, 0, sx * spread, 0});
            pass(blur_ps_, small_view_[1], 0, {0, 0, 0, sy * spread});
        }
        ID3D11ShaderResourceView* none[2]{}; ctx->PSSetShaderResources(0, 2, none);
        ID3D11RenderTargetView* no_target = nullptr; ctx->OMSetRenderTargets(1, &no_target, nullptr);
        return small_view_[0];
    }
};

// DirectWrite text baked into textures. Strings are cached; single characters back the rolling digits.
enum class Face { Title, Body, Digits, Label, Symbol };
struct Text { std::unique_ptr<Texture> texture; float width = 0, height = 0, pad = 0, baseline = 0, cap = 0; double used = 0; };

class Typesetter {
    IDWriteFactory* dwrite_ = nullptr; ID2D1Factory* d2d_ = nullptr; IWICImagingFactory* wic_ = nullptr; ID3D11Device* device_ = nullptr;
    std::map<std::tuple<int, int, std::wstring>, Text> cache_;
    std::map<std::pair<int, int>, IDWriteTextFormat*> formats_;
    std::map<int, std::wstring> families_;
    std::wstring family(Face f) {
        auto key = int(f); if (auto i = families_.find(key); i != families_.end()) return i->second;
        static const std::vector<std::vector<const wchar_t*>> options = {
            {L"Segoe UI Variable Display", L"Segoe UI"}, {L"Segoe UI Variable Text", L"Segoe UI"}, {L"Bahnschrift", L"Segoe UI"}, {L"Bahnschrift", L"Segoe UI"}, {L"Segoe UI Symbol", L"Segoe UI"}};
        IDWriteFontCollection* fonts = nullptr; dwrite_->GetSystemFontCollection(&fonts, FALSE);
        std::wstring chosen = L"Segoe UI";
        for (auto name : options[key]) { UINT32 index = 0; BOOL exists = FALSE; if (fonts && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists) { chosen = name; break; } }
        release(fonts); return families_[key] = chosen;
    }
    IDWriteTextFormat* format(Face f, int px) {
        auto key = std::pair{int(f), px}; if (auto i = formats_.find(key); i != formats_.end()) return i->second;
        DWRITE_FONT_WEIGHT weight = f == Face::Body ? DWRITE_FONT_WEIGHT_NORMAL : f == Face::Symbol ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_SEMI_BOLD;
        DWRITE_FONT_STRETCH stretch = f == Face::Label ? DWRITE_FONT_STRETCH_SEMI_CONDENSED : DWRITE_FONT_STRETCH_NORMAL;
        IDWriteTextFormat* format = nullptr;
        if (FAILED(dwrite_->CreateTextFormat(family(f).c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, stretch, float(px), L"ru-ru", &format))) return nullptr;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return formats_[key] = format;
    }

    std::map<int, float> caps_;
    float cap_height(Face face, IDWriteTextFormat* fmt) {
        if (auto i = caps_.find(int(face)); i != caps_.end()) return i->second;
        float cap = 0.7f; IDWriteFontCollection* fonts = nullptr;
        if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts, FALSE)) && fonts) {
            UINT32 index = 0; BOOL exists = FALSE; IDWriteFontFamily* fam = nullptr; IDWriteFont* font = nullptr;
            if (SUCCEEDED(fonts->FindFamilyName(family(face).c_str(), &index, &exists)) && exists && SUCCEEDED(fonts->GetFontFamily(index, &fam)) && SUCCEEDED(fam->GetFirstMatchingFont(fmt->GetFontWeight(), fmt->GetFontStretch(), DWRITE_FONT_STYLE_NORMAL, &font))) {
                DWRITE_FONT_METRICS fm{}; font->GetMetrics(&fm); if (fm.designUnitsPerEm && fm.capHeight) cap = float(fm.capHeight) / fm.designUnitsPerEm;
            }
            release(font); release(fam); release(fonts);
        }
        return caps_[int(face)] = cap;
    }

public:
    double now = 0;
    ~Typesetter() { shutdown(); }
    bool init(ID3D11Device* device) {
        shutdown(); device_ = device; device_->AddRef();
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite_)))) return false;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(&d2d_)))) return false;
        const GUID clsid{0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&wic_)))) return false;
        return true;
    }
    void shutdown() { cache_.clear(); caps_.clear(); for (auto& [_, f] : formats_) release(f); formats_.clear(); release(wic_); release(d2d_); release(dwrite_); release(device_); }
    // Drops strings nobody drew for a while (old track titles, past clock minutes).
    void collect() { for (auto i = cache_.begin(); i != cache_.end();) { if (now - i->second.used > 20 && std::get<2>(i->first).size() > 1) i = cache_.erase(i); else ++i; } }

    const Text* get(const std::wstring& s, Face face, float size) {
        int px = std::max(6, int(std::lround(size)));
        auto key = std::tuple{int(face), px, s};
        if (auto i = cache_.find(key); i != cache_.end()) { i->second.used = now; return &i->second; }
        auto fmt = format(face, px); if (!fmt || !wic_) return nullptr;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(dwrite_->CreateTextLayout(s.c_str(), UINT32(s.size()), fmt, 8192.f, 512.f, &layout))) return nullptr;
        DWRITE_TEXT_METRICS tm{}; layout->GetMetrics(&tm);
        DWRITE_LINE_METRICS lm{}; UINT32 lines = 0; layout->GetLineMetrics(&lm, 1, &lines);
        float cap = cap_height(face, fmt) * px;
        const float pad = std::ceil(px * 0.3f) + 2;
        int w = std::max(1, int(std::ceil(tm.widthIncludingTrailingWhitespace + pad * 2))), h = std::max(1, int(std::ceil(tm.height + pad * 2)));
        Text out; out.width = tm.widthIncludingTrailingWhitespace; out.height = tm.height; out.pad = pad; out.baseline = lm.baseline; out.cap = cap; out.used = now;
        IWICBitmap* bitmap = nullptr; ID2D1RenderTarget* rt = nullptr; ID2D1SolidColorBrush* brush = nullptr;
        D2D1_RENDER_TARGET_PROPERTIES props{}; props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE; props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        const GUID pbgra{0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};
        const D2D1_COLOR_F white_color{1, 1, 1, 1};
        bool ok = SUCCEEDED(wic_->CreateBitmap(UINT(w), UINT(h), pbgra, WICBitmapCacheOnLoad, &bitmap)) && SUCCEEDED(d2d_->CreateWicBitmapRenderTarget(bitmap, &props, &rt))
               && SUCCEEDED(rt->CreateSolidColorBrush(&white_color, nullptr, &brush));
        if (ok) {
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->BeginDraw(); rt->Clear(D2D1_COLOR_F{0, 0, 0, 0}); rt->DrawTextLayout(D2D1_POINT_2F{pad, pad}, layout, brush); ok = SUCCEEDED(rt->EndDraw());
        }
        if (ok) {
            std::vector<std::uint32_t> pixels(size_t(w) * h);
            ok = SUCCEEDED(bitmap->CopyPixels(nullptr, UINT(w * 4), UINT(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
            if (ok) out.texture = Texture::create(device_, w, h, pixels.data());
        }
        release(brush); release(rt); release(bitmap); release(layout);
        if (!out.texture) return nullptr;
        return &(cache_[key] = std::move(out));
    }
};

// Draws a cached string with its top-left at (x, y); `y` may instead be the vertical centre of capitals.
inline void draw_text(Canvas& c, const Text* t, float x, float y, Color color) {
    if (!t) return; c.image(t->texture.get(), x - t->pad, y - t->pad, float(t->texture->w), float(t->texture->h), 0, color);
}
inline void draw_text_centered(Canvas& c, const Text* t, float x, float cy, Color color) {
    if (!t) return; draw_text(c, t, x, cy - t->baseline + t->cap / 2, color);
}
}
