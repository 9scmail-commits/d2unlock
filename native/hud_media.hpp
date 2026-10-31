#pragma once
// Windows global media session (GSMTC) bridge: whatever Windows shows in its media flyout
// (Spotify, Yandex Music, browsers) is mirrored into a snapshot the HUD player draws.
#include <windows.h>
#include <inspectable.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media {
struct Art { int w = 0, h = 0; std::vector<std::uint32_t> pixels; float accent[3]{}; };  // premultiplied BGRA

struct Snapshot {
    bool session = false, playing = false, can_prev = false, can_next = false, can_toggle = false, can_seek = false;
    std::wstring title, artist;
    double position = 0, duration = 0, stamp = 0;  // seconds; position was valid at clock() == stamp
    unsigned track = 0, art_version = 0;
    std::shared_ptr<const Art> art;
    double at(double now) const { double p = position + (playing ? now - stamp : 0); return duration > 0 ? std::clamp(p, 0.0, duration) : std::max(0.0, p); }
};

inline double clock() {
    static const double frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return double(t.QuadPart) / frequency;
}

enum class Command { Toggle, Next, Previous, Seek };

namespace abi {
struct TimeSpan { INT64 Duration; };
struct DateTime { INT64 UniversalTime; };
struct IAsyncInfo : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct IAsyncOperation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void* result) = 0;
};
struct IStreamReference : IInspectable { virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IAsyncOperation**) = 0; };
struct IMediaProperties : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Genres(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IStreamReference**) = 0;
};
struct ITimeline : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(TimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(TimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(TimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(TimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(TimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LastUpdatedTime(DateTime*) = 0;
};
struct IControls : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelUpEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelDownEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayPauseToggleEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsShuffleEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRepeatEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlaybackRateEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlaybackPositionEnabled(boolean*) = 0;
};
struct IPlaybackInfo : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IControls**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int*) = 0;
};
struct ISession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(ITimeline**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(IPlaybackInfo**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeAutoRepeatModeAsync(int, IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackRateAsync(double, IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeShuffleActiveAsync(boolean, IAsyncOperation**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackPositionAsync(INT64, IAsyncOperation**) = 0;
};
struct IManager : IInspectable { virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(ISession**) = 0; };
struct IManagerStatics : IInspectable { virtual HRESULT STDMETHODCALLTYPE RequestAsync(IAsyncOperation**) = 0; };

// Interface IDs as published in the Windows SDK (windows.media.control.h, windows.storage.streams.h).
constexpr GUID IID_AsyncInfo{0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID IID_ManagerStatics{0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
constexpr GUID IID_Stream{0x0000000c, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID CLSID_Wic{0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
constexpr GUID IID_Wic{0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
constexpr GUID Format_PBGRA{0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

template<class T> struct Ref {
    T* p = nullptr;
    Ref() = default; Ref(const Ref&) = delete; Ref& operator=(const Ref&) = delete;
    Ref(Ref&& o) noexcept : p(o.p) { o.p = nullptr; }
    Ref& operator=(Ref&& o) noexcept { if (this != &o) { reset(); p = o.p; o.p = nullptr; } return *this; }
    ~Ref() { reset(); }
    void reset() { if (p) p->Release(); p = nullptr; }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct Runtime {
    HRESULT (WINAPI* initialize)(int) = nullptr;
    HRESULT (WINAPI* factory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT (WINAPI* create)(PCWSTR, UINT32, HSTRING*) = nullptr;
    HRESULT (WINAPI* remove)(HSTRING) = nullptr;
    PCWSTR (WINAPI* buffer)(HSTRING, UINT32*) = nullptr;
    HRESULT (WINAPI* stream)(IUnknown*, REFIID, void**) = nullptr;
    bool load() {
        auto combase = LoadLibraryW(L"combase.dll"), shcore = LoadLibraryW(L"shcore.dll");
        if (!combase || !shcore) return false;
        initialize = reinterpret_cast<decltype(initialize)>(reinterpret_cast<void*>(GetProcAddress(combase, "RoInitialize")));
        factory = reinterpret_cast<decltype(factory)>(reinterpret_cast<void*>(GetProcAddress(combase, "RoGetActivationFactory")));
        create = reinterpret_cast<decltype(create)>(reinterpret_cast<void*>(GetProcAddress(combase, "WindowsCreateString")));
        remove = reinterpret_cast<decltype(remove)>(reinterpret_cast<void*>(GetProcAddress(combase, "WindowsDeleteString")));
        buffer = reinterpret_cast<decltype(buffer)>(reinterpret_cast<void*>(GetProcAddress(combase, "WindowsGetStringRawBuffer")));
        stream = reinterpret_cast<decltype(stream)>(reinterpret_cast<void*>(GetProcAddress(shcore, "CreateStreamOverRandomAccessStream")));
        return initialize && factory && create && remove && buffer && stream;
    }
    std::wstring text(HSTRING s) const { if (!s) return {}; UINT32 n = 0; auto p = buffer(s, &n); std::wstring out(p ? p : L"", p ? n : 0); remove(s); return out; }
};

// Blocks the bridge thread until a WinRT async operation settles, then fetches its result.
inline bool wait(IAsyncOperation* op, void* result, const std::atomic_bool& stop, unsigned timeout = 4000) {
    if (!op) return false;
    Ref<IAsyncInfo> info;
    if (FAILED(op->QueryInterface(IID_AsyncInfo, reinterpret_cast<void**>(info.put())))) return false;
    const auto deadline = GetTickCount64() + timeout;
    for (int status = 0;;) {
        if (FAILED(info->get_Status(&status))) return false;
        if (status == 1) return result ? SUCCEEDED(op->GetResults(result)) : true;
        if (status != 0 || stop || GetTickCount64() > deadline) { info->Cancel(); return false; }
        Sleep(4);
    }
}
}

class Bridge {
    std::thread thread_;
    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<std::pair<Command, double>> commands_;
    std::atomic_bool stop_{false}, active_{false};
    Snapshot snapshot_;
    double hold_play_until_ = 0, hold_seek_until_ = 0;
    abi::Runtime rt_;
    abi::Ref<abi::IManager> manager_;
    abi::Ref<IWICImagingFactory> wic_;
    std::wstring key_;
    double track_since_ = 0, art_checked_ = 0, properties_checked_ = 0;
    std::uint64_t art_hash_ = 0;

    std::shared_ptr<Art> decode(abi::IStreamReference* reference) {
        if (!reference || !wic_) return nullptr;
        abi::Ref<abi::IAsyncOperation> op; abi::Ref<IUnknown> random;
        if (FAILED(reference->OpenReadAsync(op.put())) || !abi::wait(op.p, random.put(), stop_) || !random) return nullptr;
        abi::Ref<IStream> stream; abi::Ref<IWICBitmapDecoder> decoder; abi::Ref<IWICBitmapFrameDecode> frame;
        if (FAILED(rt_.stream(random.p, abi::IID_Stream, reinterpret_cast<void**>(stream.put())))) return nullptr;
        if (FAILED(wic_->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) || FAILED(decoder->GetFrame(0, frame.put()))) return nullptr;
        UINT w = 0, h = 0; frame->GetSize(&w, &h); if (!w || !h) return nullptr;
        // Browsers hand over 16:9 video stills; a centred square crop keeps covers and stills consistent.
        UINT side = std::min(w, h); WICRect crop{INT((w - side) / 2), INT((h - side) / 2), INT(side), INT(side)};
        abi::Ref<IWICBitmapClipper> clipper; abi::Ref<IWICBitmapScaler> scaler; abi::Ref<IWICFormatConverter> converter;
        constexpr UINT N = 192;
        if (FAILED(wic_->CreateBitmapClipper(clipper.put())) || FAILED(clipper->Initialize(frame.p, &crop))) return nullptr;
        if (FAILED(wic_->CreateBitmapScaler(scaler.put()))) return nullptr;
        if (FAILED(scaler->Initialize(clipper.p, N, N, WICBitmapInterpolationMode(4) /* HighQualityCubic */)) && FAILED(scaler->Initialize(clipper.p, N, N, WICBitmapInterpolationModeFant))) return nullptr;
        if (FAILED(wic_->CreateFormatConverter(converter.put())) || FAILED(converter->Initialize(scaler.p, abi::Format_PBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return nullptr;
        auto art = std::make_shared<Art>(); art->w = art->h = N; art->pixels.resize(N * N);
        if (FAILED(converter->CopyPixels(nullptr, N * 4, N * N * 4, reinterpret_cast<BYTE*>(art->pixels.data())))) return nullptr;
        // Accent: saturation-weighted average, then pushed to a vivid but readable tone.
        double r = 0, g = 0, b = 0, weight = 0;
        for (auto px : art->pixels) {
            float pb = (px & 255) / 255.f, pg = ((px >> 8) & 255) / 255.f, pr = ((px >> 16) & 255) / 255.f;
            float hi = std::max({pr, pg, pb}), lo = std::min({pr, pg, pb}), sat = hi > 0 ? (hi - lo) / hi : 0;
            float w8 = 0.04f + sat * sat * hi; r += pr * w8; g += pg * w8; b += pb * w8; weight += w8;
        }
        float c[3] = {float(r / weight), float(g / weight), float(b / weight)};
        float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
        if (hi - lo < 0.06f) { c[0] = 0.62f; c[1] = 0.66f; c[2] = 1.f; }  // grey cover: keep the default periwinkle
        else {
            float sat = std::max((hi - lo) / hi, 0.55f), value = std::clamp(hi, 0.82f, 1.f);
            for (auto& v : c) { float t = (v - lo) / (hi - lo); v = value * (1 - sat + sat * t); }
        }
        std::copy(c, c + 3, art->accent);
        return art;
    }
    static std::uint64_t hash(const Art& a) { std::uint64_t h = 1469598103934665603ull; for (std::size_t i = 0; i < a.pixels.size(); i += 7) { h ^= a.pixels[i]; h *= 1099511628211ull; } return h; }

    void run_command(abi::ISession* session, Command command, double value) {
        abi::Ref<abi::IAsyncOperation> op; HRESULT hr = E_FAIL;
        switch (command) {
        case Command::Toggle: hr = session->TryTogglePlayPauseAsync(op.put()); break;
        case Command::Next: hr = session->TrySkipNextAsync(op.put()); break;
        case Command::Previous: hr = session->TrySkipPreviousAsync(op.put()); break;
        case Command::Seek: hr = session->TryChangePlaybackPositionAsync(INT64(value * 1e7), op.put()); break;
        }
        if (SUCCEEDED(hr)) abi::wait(op.p, nullptr, stop_, 1500);
    }

    void poll() {
        if (!manager_) {
            abi::Ref<abi::IManagerStatics> statics; HSTRING name = nullptr;
            static const wchar_t cls[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
            if (FAILED(rt_.create(cls, UINT32(std::size(cls) - 1), &name))) return;
            auto hr = rt_.factory(name, abi::IID_ManagerStatics, reinterpret_cast<void**>(statics.put())); rt_.remove(name);
            abi::Ref<abi::IAsyncOperation> op;
            if (FAILED(hr) || FAILED(statics->RequestAsync(op.put())) || !abi::wait(op.p, manager_.put(), stop_)) { manager_.reset(); return; }
        }
        abi::Ref<abi::ISession> session;
        if (FAILED(manager_->GetCurrentSession(session.put())) || !session) {
            std::lock_guard g(lock_); if (snapshot_.session) { snapshot_ = Snapshot{}; snapshot_.track = ++track_counter; key_.clear(); } return;
        }
        std::vector<std::pair<Command, double>> queued;
        { std::lock_guard g(lock_); queued.assign(commands_.begin(), commands_.end()); commands_.clear(); }
        for (auto& [c, v] : queued) run_command(session.p, c, v);

        const double now = clock();
        int status = 0; boolean prev = 0, next = 0, toggle = 0, seek = 0;
        { abi::Ref<abi::IPlaybackInfo> info; abi::Ref<abi::IControls> controls;
          if (SUCCEEDED(session->GetPlaybackInfo(info.put())) && info) { info->get_PlaybackStatus(&status);
              if (SUCCEEDED(info->get_Controls(controls.put())) && controls) { controls->get_IsPreviousEnabled(&prev); controls->get_IsNextEnabled(&next); controls->get_IsPlayPauseToggleEnabled(&toggle); controls->get_IsPlaybackPositionEnabled(&seek); } } }
        double position = 0, duration = 0;
        { abi::Ref<abi::ITimeline> timeline; abi::TimeSpan start{}, end{}, pos{}; abi::DateTime updated{};
          if (SUCCEEDED(session->GetTimelineProperties(timeline.put())) && timeline) {
              timeline->get_StartTime(&start); timeline->get_EndTime(&end); timeline->get_Position(&pos); timeline->get_LastUpdatedTime(&updated);
              duration = std::max(0.0, double(end.Duration - start.Duration) / 1e7); position = std::max(0.0, double(pos.Duration - start.Duration) / 1e7);
              if (status == 4 && updated.UniversalTime > 0) {
                  FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
                  double elapsed = double(INT64((ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - updated.UniversalTime) / 1e7;
                  if (elapsed > 0 && elapsed < 6 * 3600) position += elapsed;
              } } }

        std::wstring title, artist; std::shared_ptr<Art> art; bool art_tried = false;
        const bool refresh_properties = now - properties_checked_ > 0.6 || key_.empty();
        if (refresh_properties) {
            properties_checked_ = now;
            abi::Ref<abi::IAsyncOperation> op; abi::Ref<abi::IMediaProperties> props;
            if (SUCCEEDED(session->TryGetMediaPropertiesAsync(op.put())) && abi::wait(op.p, props.put(), stop_) && props) {
                HSTRING s = nullptr;
                if (SUCCEEDED(props->get_Title(&s))) title = rt_.text(s);
                s = nullptr; if (SUCCEEDED(props->get_Artist(&s))) artist = rt_.text(s);
                if (artist.empty()) { s = nullptr; if (SUCCEEDED(props->get_AlbumArtist(&s))) artist = rt_.text(s); }
                const bool changed = title + L'\x1f' + artist != key_;
                if (changed) { track_since_ = now; art_checked_ = 0; art_hash_ = 0; }
                // Players often publish the cover a moment after the title; keep re-reading it briefly.
                if (changed || (now - track_since_ < 6 && now - art_checked_ > 0.9)) {
                    art_checked_ = now; art_tried = true;
                    abi::Ref<abi::IStreamReference> thumb;
                    if (SUCCEEDED(props->get_Thumbnail(thumb.put())) && thumb) art = decode(thumb.p);
                }
            } else title = L"\x1f";  // properties unavailable this round: keep the previous text
        }

        std::lock_guard g(lock_);
        auto& s = snapshot_;
        if (!s.session) s.track = ++track_counter;
        s.session = true; s.can_prev = prev; s.can_next = next; s.can_toggle = toggle; s.can_seek = seek && duration > 0;
        if (now >= hold_play_until_) s.playing = status == 4;
        if (now >= hold_seek_until_ || std::abs(position - s.at(now)) < 1.5) { s.position = position; s.stamp = now; }
        s.duration = duration;
        if (refresh_properties && title != L"\x1f") {
            auto key = title + L'\x1f' + artist;
            if (key != key_) { key_ = key; s.title = title; s.artist = artist; s.track = ++track_counter; s.art.reset(); ++s.art_version; }
            if (art_tried && art) { auto h = hash(*art); if (h != art_hash_) { art_hash_ = h; s.art = std::move(art); ++s.art_version; } }
        }
    }

    void run() {
        if (!rt_.load()) return;
        rt_.initialize(1);  // multithreaded apartment
        CoCreateInstance(abi::CLSID_Wic, nullptr, CLSCTX_INPROC_SERVER, abi::IID_Wic, reinterpret_cast<void**>(wic_.put()));
        while (!stop_) {
            if (active_) { try { poll(); } catch (...) {} }
            std::unique_lock g(lock_);
            wake_.wait_for(g, std::chrono::milliseconds(active_ ? 250 : 1000), [this] { return stop_ || !commands_.empty(); });
        }
        manager_.reset(); wic_.reset();
    }

public:
    unsigned track_counter = 0;
    ~Bridge() { stop_ = true; wake_.notify_all(); if (thread_.joinable()) thread_.join(); }
    void activate(bool on) {
        active_ = on;
        if (on && !thread_.joinable()) thread_ = std::thread([this] { run(); });
        if (on) wake_.notify_all();
    }
    bool connected() { std::lock_guard g(lock_); return bool(manager_); }
    Snapshot snapshot() { std::lock_guard g(lock_); return snapshot_; }
    // Commands update the snapshot optimistically so the UI responds before the player confirms.
    void send(Command command, double value = 0) {
        {
            std::lock_guard g(lock_); const double now = clock();
            if (command == Command::Toggle) { snapshot_.position = snapshot_.at(now); snapshot_.stamp = now; snapshot_.playing = !snapshot_.playing; hold_play_until_ = now + 1.5; }
            if (command == Command::Seek) { snapshot_.position = value; snapshot_.stamp = now; hold_seek_until_ = now + 2.0; }
            commands_.emplace_back(command, value);
        }
        wake_.notify_all();
    }
};
}
