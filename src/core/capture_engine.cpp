// Windows must come before WinRT
#include <Windows.h>
#include <inspectable.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

// C++/WinRT projection headers
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// IDirect3DDxgiInterfaceAccess is sometimes not exposed by SDK headers
// depending on include order / version. Declare it explicitly.
#ifndef __IDirect3DDxgiInterfaceAccess_FWD_DEFINED__
MIDL_INTERFACE("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")
IDirect3DDxgiInterfaceAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid,
                                                    void** p) = 0;
};
#endif

// Project headers
#include <memoir/capture_engine.h>
#include "keyboard.h"
#include "window_finder.h"

// Standard library
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace memoir {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace dx  = winrt::Windows::Graphics::DirectX;
namespace d3d = winrt::Windows::Graphics::DirectX::Direct3D11;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int64_t GetQPC() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

static int64_t GetQPCFrequency() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
}

// "step: HRESULT 0x887A0005: <system message>" for a WinRT error.
// winrt::hresult_error does NOT derive from std::exception, so every boundary
// that only catches std::exception lets it escape — out of an N-API callback
// that means std::terminate() and a fail-fast process exit with no minidump.
static std::string DescribeHresult(const char* step,
                                   const winrt::hresult_error& e) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: HRESULT 0x%08X: ", step,
             static_cast<uint32_t>(e.code().value));
    return std::string(buf) + winrt::to_string(e.message());
}

// Run one step of Start(); rethrow any WinRT error as std::runtime_error that
// names the step, so callers (and logs) see which call failed and why.
template <typename F>
static auto Step(const char* step, F&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const winrt::hresult_error& e) {
        throw std::runtime_error(DescribeHresult(step, e));
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string(step) + ": " + e.what());
    }
}

// ---------------------------------------------------------------------------
// CaptureEngine::Impl
// ---------------------------------------------------------------------------

struct CaptureEngine::Impl {
    EngineConfig config;
    std::atomic<EngineState> state{EngineState::Created};

    // D3D11
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext>  context;
    std::mutex contextMutex;

    // WGC
    d3d::IDirect3DDevice   winrtDevice{nullptr};
    wgc::GraphicsCaptureItem            captureItem{nullptr};
    wgc::Direct3D11CaptureFramePool     framePool{nullptr};
    wgc::GraphicsCaptureSession         session{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrivedRevoker;
    wgc::GraphicsCaptureItem::Closed_revoker              closedRevoker;
    HWND targetHwnd{nullptr};  // non-null when capturing a window

    // Staging texture (single, recreated on size change)
    winrt::com_ptr<ID3D11Texture2D> stagingTex;
    uint32_t stagingW = 0;
    uint32_t stagingH = 0;

    // Current FramePool size (for ContentSize-change detection)
    uint32_t poolW = 0;
    uint32_t poolH = 0;
    std::atomic<uint64_t> poolRecreates{0};

    // FrameArrived runs on WinRT thread-pool threads and, with a free-threaded
    // pool, can run concurrently. frameMutex serialises the handler body (pool
    // Recreate + TryGetNextFrame must not race); callbacksInFlight lets Stop()
    // wait for running handlers before it tears down the device they use.
    std::mutex frameMutex;
    std::atomic<int> callbacksInFlight{0};

    // D3D adapter the capture device was created on (for cross-GPU issues).
    std::string adapterDescription;

    // Per-step error counts from OnFrameArrived, e.g. {"Recreate": 296}.
    // Guarded by errorMutex, like lastPoolChange.
    std::map<std::string, uint64_t> errorsByStep;
    std::string lastPoolChange;

    // Frame queue
    std::queue<std::shared_ptr<FramePacket>> frameQueue;
    mutable std::mutex queueMutex;
    std::condition_variable queueCV;

    // Counters
    std::atomic<uint64_t> nextFrameId{0};
    std::atomic<uint64_t> framesSeen{0};
    std::atomic<uint64_t> framesAccepted{0};
    std::atomic<uint64_t> framesDroppedQueueFull{0};
    std::atomic<uint64_t> framesDroppedInternalError{0};
    std::atomic<uint64_t> framesRecorded{0};

    // FPS limiting
    int64_t lastAcceptQPC   = 0;
    int64_t minIntervalQPC  = 0;
    int64_t qpcFrequency    = 0;

    // Error
    std::optional<std::string> lastError;
    mutable std::mutex errorMutex;

    // Recording
    std::unique_ptr<RecordingSession> recordingSession;
    mutable std::mutex recordingMutex;

    // COM ownership
    bool comOwned = false;

    // -----------------------------------------------------------------------
    void InitD3D11() {
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                       D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL achieved;
        winrt::check_hresult(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 2, D3D11_SDK_VERSION,
            device.put(), &achieved, context.put()));

        auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> insp;
        winrt::check_hresult(
            CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
        winrtDevice = insp.as<d3d::IDirect3DDevice>();

        // Record which adapter we got: on hybrid-GPU machines the default
        // adapter can differ from the one the game renders on.
        adapterDescription.clear();
        winrt::com_ptr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(dxgi->GetAdapter(adapter.put())) &&
            SUCCEEDED(adapter->GetDesc(&ad))) {
            char buf[96];
            snprintf(buf, sizeof(buf),
                     " (vendor=0x%04X device=0x%04X luid=%08X:%08X fl=0x%X)",
                     ad.VendorId, ad.DeviceId,
                     static_cast<uint32_t>(ad.AdapterLuid.HighPart),
                     ad.AdapterLuid.LowPart, static_cast<uint32_t>(achieved));
            adapterDescription = winrt::to_string(ad.Description) + buf;
        }
    }

    // -----------------------------------------------------------------------
    wgc::GraphicsCaptureItem CreateCaptureItem() {
        auto interop = winrt::get_activation_factory<
            wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

        wgc::GraphicsCaptureItem item{nullptr};

        switch (config.target.type) {
        case CaptureTargetType::WindowTitleRegex: {
            HWND h = FindWindowByTitleRegex(config.target.value_wstr);
            if (!h) throw std::runtime_error("No window matching title pattern");
            targetHwnd = h;
            winrt::check_hresult(interop->CreateForWindow(
                h, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                winrt::put_abi(item)));
            break;
        }
        case CaptureTargetType::WindowExeRegex: {
            HWND h = FindWindowByExeRegex(config.target.value_wstr);
            if (!h) throw std::runtime_error("No window matching exe pattern");
            targetHwnd = h;
            winrt::check_hresult(interop->CreateForWindow(
                h, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                winrt::put_abi(item)));
            break;
        }
        case CaptureTargetType::MonitorIndex: {
            HMONITOR hm = GetMonitorByIndex(config.target.monitor_index);
            if (!hm)
                throw std::runtime_error(
                    "No monitor at index " +
                    std::to_string(config.target.monitor_index));
            winrt::check_hresult(interop->CreateForMonitor(
                hm, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                winrt::put_abi(item)));
            break;
        }
        }
        return item;
    }

    // -----------------------------------------------------------------------
    void EnsureStaging(uint32_t w, uint32_t h) {
        if (stagingTex && stagingW == w && stagingH == h) return;
        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_STAGING;
        d.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        stagingTex = nullptr;
        winrt::check_hresult(device->CreateTexture2D(&d, nullptr,
                                                      stagingTex.put()));
        stagingW = w;
        stagingH = h;
    }

    // -----------------------------------------------------------------------
    void OnFrameArrived() {
        // Counted before the state check: Stop() flips the state first and
        // then waits for this to drop to zero, so no handler can still be
        // running when it releases the device and pool.
        callbacksInFlight++;
        struct InFlight {
            std::atomic<int>& n;
            ~InFlight() { n--; }
        } inFlight{callbacksInFlight};

        // Which call is running, for the error report below.
        const char* step = "state check";
        try {
            if (state.load() != EngineState::Running) return;

            step = "frameMutex";
            std::lock_guard frameLock(frameMutex);
            if (state.load() != EngineState::Running) return;

            step = "TryGetNextFrame";
            auto frame = framePool.TryGetNextFrame();
            if (!frame) return;

            framesSeen++;

            // Track ContentSize changes — WGC clamps frames to the pool size,
            // so if the target window resized (incl. growing from a tiny
            // launch-time size), the pool must be recreated to match.
            {
                step = "ContentSize";
                auto cs = frame.ContentSize();
                uint32_t cw = static_cast<uint32_t>(cs.Width);
                uint32_t ch = static_cast<uint32_t>(cs.Height);
                if (cw > 0 && ch > 0 && (cw != poolW || ch != poolH)) {
                    step = "Recreate";
                    framePool.Recreate(
                        winrtDevice,
                        dx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                        2, {static_cast<int32_t>(cw),
                            static_cast<int32_t>(ch)});
                    {
                        char buf[96];
                        snprintf(buf, sizeof(buf), "%ux%u -> %ux%u", poolW,
                                 poolH, cw, ch);
                        std::lock_guard lk(errorMutex);
                        lastPoolChange = buf;
                    }
                    poolW = cw;
                    poolH = ch;
                    poolRecreates++;
                    // Continue processing this frame at its old (texture) size;
                    // subsequent FrameArrived events will deliver at new size.
                }
            }

            // FPS limiting
            int64_t now = GetQPC();
            if (lastAcceptQPC > 0 &&
                (now - lastAcceptQPC) < minIntervalQPC) {
                frame.Close();
                return;
            }

            // Queue-full check (drop-new policy)
            {
                std::lock_guard lk(queueMutex);
                if (frameQueue.size() >= config.analysis_queue_capacity) {
                    framesDroppedQueueFull++;
                    frame.Close();
                    return;
                }
            }

            // --- Accept the frame ---
            lastAcceptQPC = now;

            uint64_t fid         = nextFrameId++;
            step = "SystemRelativeTime";
            int64_t  captureQpc  = frame.SystemRelativeTime().count();
            int64_t  hostQpc     = now;
            uint64_t kbMask      = SnapshotKeyboard(config.key_map);

            // Get captured texture
            step = "Surface";
            auto surface = frame.Surface();
            auto access  = surface.as<IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> srcTex;
            step = "GetInterface(texture)";
            winrt::check_hresult(
                access->GetInterface(IID_PPV_ARGS(srcTex.put())));

            D3D11_TEXTURE2D_DESC td;
            srcTex->GetDesc(&td);

            // Build packet
            auto pkt          = std::make_shared<FramePacket>();
            pkt->frame_id     = fid;
            pkt->capture_qpc  = captureQpc;
            pkt->host_accept_qpc = hostQpc;
            pkt->keyboard_mask = kbMask;
            pkt->width         = td.Width;
            pkt->height        = td.Height;
            pkt->channels      = 4;

            // GPU copy → staging → CPU readback
            {
                std::lock_guard lk(contextMutex);
                step = "EnsureStaging";
                EnsureStaging(td.Width, td.Height);
                step = "CopyResource";
                context->CopyResource(stagingTex.get(), srcTex.get());

                step = "Map";
                D3D11_MAPPED_SUBRESOURCE mapped{};
                HRESULT hr = context->Map(stagingTex.get(), 0,
                                          D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    pkt->stride = mapped.RowPitch;
                    size_t bytes =
                        static_cast<size_t>(mapped.RowPitch) * td.Height;
                    pkt->pixel_data.resize(bytes);
                    std::memcpy(pkt->pixel_data.data(), mapped.pData, bytes);
                    context->Unmap(stagingTex.get(), 0);
                } else {
                    // Throw so it's reported (with the HRESULT and, for a
                    // removed device, the reason) like every other step.
                    winrt::throw_hresult(hr);
                }
            }

            step = "Close";
            frame.Close();

            // Recording (uses CPU pixel data — resize + NVENC encode)
            {
                std::lock_guard lk(recordingMutex);
                if (recordingSession) {
                    try {
                        recordingSession->RecordFrame(
                            pkt->pixel_data.data(), pkt->stride,
                            pkt->width, pkt->height,
                            pkt->frame_id, pkt->capture_qpc,
                            pkt->host_accept_qpc, pkt->keyboard_mask);
                        framesRecorded++;
                    } catch (const std::exception& e) {
                        SetError(std::string("Recording error: ") +
                                 e.what());
                        try { recordingSession->Finalize(); } catch (...) {}
                        recordingSession.reset();
                    }
                }
            }

            // Enqueue for Python
            {
                std::lock_guard lk(queueMutex);
                frameQueue.push(std::move(pkt));
            }
            queueCV.notify_one();
            framesAccepted++;

        } catch (const winrt::hresult_error& e) {
            std::string msg = DescribeHresult(step, e);
            if (e.code() == DXGI_ERROR_DEVICE_REMOVED && device) {
                char buf[48];
                snprintf(buf, sizeof(buf), " (removed reason 0x%08X)",
                         static_cast<uint32_t>(
                             device->GetDeviceRemovedReason()));
                msg += buf;
            }
            RecordFrameError(step, "OnFrameArrived " + msg);
        } catch (const std::exception& e) {
            RecordFrameError(step, std::string("OnFrameArrived ") + step +
                                       ": " + e.what());
        } catch (...) {
            RecordFrameError(step, std::string("OnFrameArrived ") + step +
                                       ": unknown native exception");
        }
    }

    // Tear down the WGC session/pool and D3D objects. Callers set state to
    // Stopping first. Never throws: Close() on WinRT objects can fail with an
    // hresult_error (e.g. RO_E_CLOSED), and an exception out of Stop() or the
    // destructor would terminate the process.
    void ReleaseCaptureResources() {
        try { frameArrivedRevoker.revoke(); } catch (...) {}
        try { closedRevoker.revoke(); } catch (...) {}

        // revoke() stops new FrameArrived calls but doesn't wait for ones
        // already running on thread-pool threads, which may be using the pool
        // and device we're about to release. Wait for them (they bail out
        // quickly once they see state != Running). Bounded so a wedged
        // handler can't hang shutdown.
        for (int i = 0; i < 2000 && callbacksInFlight.load() > 0; i++) {
            Sleep(1);
        }

        if (session) {
            try { session.Close(); } catch (...) {}
            session = nullptr;
        }
        if (framePool) {
            try { framePool.Close(); } catch (...) {}
            framePool = nullptr;
        }
        captureItem = nullptr;

        {
            std::lock_guard lk(contextMutex);
            stagingTex = nullptr;
            stagingW = 0;
            stagingH = 0;
        }
        poolW = 0;
        poolH = 0;
        context     = nullptr;
        device      = nullptr;
        winrtDevice = nullptr;
    }

    void RecordFrameError(const char* step, const std::string& msg) {
        framesDroppedInternalError++;
        std::lock_guard lk(errorMutex);
        lastError = msg;
        errorsByStep[step]++;
    }

    // -----------------------------------------------------------------------
    void SetError(const std::string& msg) {
        std::lock_guard lk(errorMutex);
        lastError = msg;
    }
};

// ---------------------------------------------------------------------------
// CaptureEngine public API
// ---------------------------------------------------------------------------

CaptureEngine::CaptureEngine(const EngineConfig& cfg)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = cfg;
    if (impl_->config.key_map.empty())
        impl_->config.key_map = GetDefaultKeyMap();
}

CaptureEngine::~CaptureEngine() {
    auto st = impl_->state.load();
    if (st == EngineState::Running || st == EngineState::Faulted) {
        try { Stop(); } catch (...) {}
    }
    // NOTE: We intentionally do NOT call CoUninitialize here.
    // WinRT thread-pool threads from WGC may still reference COM state.
    // COM stays initialized for the process lifetime — this is safe.
}

void CaptureEngine::Start() {
    auto st = impl_->state.load();
    if (st != EngineState::Created && st != EngineState::Stopped)
        throw std::runtime_error("Engine not in a startable state");

    // Ensure COM (MTA) on calling thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    impl_->comOwned = (hr == S_OK);
    // S_FALSE = already MTA (fine), RPC_E_CHANGED_MODE = STA (risky but try)

    // Timing
    impl_->qpcFrequency   = GetQPCFrequency();
    impl_->minIntervalQPC =
        static_cast<int64_t>(impl_->qpcFrequency / impl_->config.max_fps);
    impl_->lastAcceptQPC  = 0;

    // Reset counters
    impl_->nextFrameId.store(0);
    impl_->framesSeen.store(0);
    impl_->framesAccepted.store(0);
    impl_->framesDroppedQueueFull.store(0);
    impl_->framesDroppedInternalError.store(0);
    impl_->framesRecorded.store(0);
    impl_->poolRecreates.store(0);
    {
        std::lock_guard lk(impl_->errorMutex);
        impl_->errorsByStep.clear();
        impl_->lastPoolChange.clear();
    }

    // Every step is wrapped by Step(): a WinRT failure comes out as a
    // std::runtime_error naming the step and HRESULT (e.g.
    // "CreateForWindow: HRESULT 0x80070057: The parameter is incorrect.")
    // instead of an hresult_error that callers can't catch generically.
    try {
        Step("D3D11CreateDevice", [&] { impl_->InitD3D11(); });

        impl_->captureItem =
            Step("CreateForWindow", [&] { return impl_->CreateCaptureItem(); });
        auto size = Step("GraphicsCaptureItem.Size",
                         [&] { return impl_->captureItem.Size(); });
        if (size.Width <= 0 || size.Height <= 0) {
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "Capture target has an empty size (%dx%d)", size.Width,
                     size.Height);
            throw std::runtime_error(buf);
        }

        // Frame pool (free-threaded – no DispatcherQueue required)
        impl_->framePool = Step("CreateFreeThreaded", [&] {
            return wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                impl_->winrtDevice,
                dx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        });
        impl_->poolW = static_cast<uint32_t>(size.Width);
        impl_->poolH = static_cast<uint32_t>(size.Height);

        // Subscribe FrameArrived
        impl_->frameArrivedRevoker = Step("FrameArrived subscribe", [&] {
            return impl_->framePool.FrameArrived(
                winrt::auto_revoke,
                [raw = impl_.get()](auto const&, auto const&) {
                    raw->OnFrameArrived();
                });
        });

        // Subscribe item Closed (target window closed / monitor removed)
        impl_->closedRevoker = Step("Closed subscribe", [&] {
            return impl_->captureItem.Closed(
                winrt::auto_revoke,
                [raw = impl_.get()](auto const&, auto const&) {
                    raw->state.store(EngineState::Faulted);
                    raw->SetError("Capture target closed");
                    raw->queueCV.notify_all();
                });
        });

        // Create & configure session
        impl_->session = Step("CreateCaptureSession", [&] {
            return impl_->framePool.CreateCaptureSession(impl_->captureItem);
        });
        try { impl_->session.IsBorderRequired(false); } catch (...) {}
        try {
            impl_->session.IsCursorCaptureEnabled(
                impl_->config.capture_cursor);
        } catch (...) {}

        impl_->state.store(EngineState::Running);
        Step("StartCapture", [&] { impl_->session.StartCapture(); });
    } catch (...) {
        // Leave the engine startable again and release what was created.
        impl_->state.store(EngineState::Stopping);
        impl_->ReleaseCaptureResources();
        impl_->state.store(EngineState::Created);
        throw;
    }
}

void CaptureEngine::Stop() {
    auto expected = EngineState::Running;
    if (!impl_->state.compare_exchange_strong(expected,
                                               EngineState::Stopping)) {
        if (expected != EngineState::Faulted) return;
    }

    // Stop recording first
    {
        std::lock_guard lk(impl_->recordingMutex);
        if (impl_->recordingSession) {
            try { impl_->recordingSession->Finalize(); } catch (...) {}
            impl_->recordingSession.reset();
        }
    }

    impl_->ReleaseCaptureResources();

    impl_->state.store(EngineState::Stopped);
    impl_->queueCV.notify_all();
}

std::shared_ptr<FramePacket> CaptureEngine::GetNextFrame(int timeout_ms) {
    std::unique_lock lk(impl_->queueMutex);

    auto ready = [this]() {
        return !impl_->frameQueue.empty() ||
               impl_->state.load() != EngineState::Running;
    };

    if (timeout_ms < 0) {
        impl_->queueCV.wait(lk, ready);
    } else if (timeout_ms > 0) {
        impl_->queueCV.wait_for(
            lk, std::chrono::milliseconds(timeout_ms), ready);
    }
    // timeout_ms == 0 → non-blocking poll (no wait)

    if (impl_->frameQueue.empty()) {
        // WGC's Closed event is unreliable for window destruction, so
        // fall back to checking the HWND directly.
        if (impl_->targetHwnd && !::IsWindow(impl_->targetHwnd)) {
            impl_->state.store(EngineState::Faulted);
            impl_->SetError("Capture target closed");
        }
        return nullptr;
    }

    auto pkt = impl_->frameQueue.front();
    impl_->frameQueue.pop();
    return pkt;
}

EngineStats CaptureEngine::GetStats() const {
    EngineStats s;
    s.frames_seen                 = impl_->framesSeen.load();
    s.frames_accepted             = impl_->framesAccepted.load();
    s.frames_dropped_queue_full   = impl_->framesDroppedQueueFull.load();
    s.frames_dropped_internal_error = impl_->framesDroppedInternalError.load();
    s.frames_recorded             = impl_->framesRecorded.load();
    {
        std::lock_guard lk(impl_->queueMutex);
        s.python_queue_depth =
            static_cast<uint32_t>(impl_->frameQueue.size());
    }
    {
        std::lock_guard lk(impl_->recordingMutex);
        s.recording_active = (impl_->recordingSession != nullptr);
    }
    s.pool_recreates = impl_->poolRecreates.load();
    s.pool_width     = impl_->poolW;
    s.pool_height    = impl_->poolH;
    s.adapter        = impl_->adapterDescription;
    {
        std::lock_guard lk(impl_->errorMutex);
        s.last_pool_change = impl_->lastPoolChange;
        for (const auto& [step, n] : impl_->errorsByStep) {
            if (!s.errors_by_step.empty()) s.errors_by_step += ", ";
            s.errors_by_step += step + "=" + std::to_string(n);
        }
    }
    return s;
}

EngineState CaptureEngine::GetState() const {
    return impl_->state.load();
}

std::optional<std::string> CaptureEngine::GetLastError() const {
    std::lock_guard lk(impl_->errorMutex);
    return impl_->lastError;
}

RecordingInfo CaptureEngine::StartRecording(const std::string& base_path,
                                             const std::string& encoder) {
    if (impl_->state.load() != EngineState::Running)
        throw std::runtime_error("Engine not running");

    std::lock_guard lk(impl_->recordingMutex);
    if (impl_->recordingSession)
        throw std::runtime_error("Already recording");

    RecordingSession::Config rc;
    rc.base_path     = base_path;
    rc.record_width  = impl_->config.record_width;
    rc.record_height = impl_->config.record_height;
    rc.gop           = impl_->config.record_gop;
    rc.fps           = impl_->config.max_fps;
    rc.encoder       = encoder;
    rc.key_map       = impl_->config.key_map;

    impl_->recordingSession = std::make_unique<RecordingSession>(rc);
    return impl_->recordingSession->GetInfo();
}

RecordingInfo CaptureEngine::StartRecording(const std::string& base_path,
                                             const std::string& video_path,
                                             const std::string& meta_path,
                                             const std::string& encoder) {
    if (impl_->state.load() != EngineState::Running)
        throw std::runtime_error("Engine not running");

    std::lock_guard lk(impl_->recordingMutex);
    if (impl_->recordingSession)
        throw std::runtime_error("Already recording");

    RecordingSession::Config rc;
    rc.base_path     = base_path;
    rc.video_path    = video_path;
    rc.meta_path     = meta_path;
    rc.record_width  = impl_->config.record_width;
    rc.record_height = impl_->config.record_height;
    rc.gop           = impl_->config.record_gop;
    rc.fps           = impl_->config.max_fps;
    rc.encoder       = encoder;
    rc.key_map       = impl_->config.key_map;

    impl_->recordingSession = std::make_unique<RecordingSession>(rc);
    return impl_->recordingSession->GetInfo();
}

void CaptureEngine::StopRecording() {
    std::lock_guard lk(impl_->recordingMutex);
    if (impl_->recordingSession) {
        impl_->recordingSession->Finalize();
        impl_->recordingSession.reset();
    }
}

bool CaptureEngine::IsRecording() const {
    std::lock_guard lk(impl_->recordingMutex);
    return impl_->recordingSession != nullptr;
}

void CaptureEngine::SubmitAnalysisResult(uint64_t, uint32_t,
                                          const std::vector<uint8_t>&) {
    // Stub for v1
}

} // namespace memoir
