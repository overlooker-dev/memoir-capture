#include <napi.h>

#include <Windows.h>
#include <cstring>
#include <memoir/version.h>
#include <memoir/capture_engine.h>
#include <memoir/frame_packet.h>
#include <memoir/types.h>

using namespace memoir;

// Rethrow the in-flight C++ exception as a JS error. Must be called from a
// catch block. node-addon-api is built without NAPI_CPP_EXCEPTIONS here, so
// nothing wraps our methods: any C++ exception that escapes one unwinds into
// V8 and terminates the process (fail-fast, no minidump). Every method that
// calls into the engine therefore ends with `catch (...) { ThrowAsJs(env); }`.
static void ThrowAsJs(Napi::Env env) {
    std::string msg;
    try {
        throw;
    } catch (const std::exception& e) {
        msg = e.what();
    } catch (...) {
        // e.g. a winrt::hresult_error the core failed to translate.
        msg = "unknown native exception";
    }
    Napi::Error::New(env, msg).ThrowAsJavaScriptException();
}

// ─── JsFramePacket ──────────────────────────────────────────────

class JsFramePacket : public Napi::ObjectWrap<JsFramePacket> {
    std::shared_ptr<FramePacket> pkt_;

    static Napi::FunctionReference constructor;

public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "FramePacket", {
            InstanceAccessor<&JsFramePacket::GetFrameId>("frameId"),
            InstanceAccessor<&JsFramePacket::GetWidth>("width"),
            InstanceAccessor<&JsFramePacket::GetHeight>("height"),
            InstanceAccessor<&JsFramePacket::GetStride>("stride"),
            InstanceAccessor<&JsFramePacket::GetCaptureQpc>("captureQpc"),
            InstanceAccessor<&JsFramePacket::GetHostAcceptQpc>("hostAcceptQpc"),
            InstanceAccessor<&JsFramePacket::GetKeyboardMask>("keyboardMask"),
            InstanceAccessor<&JsFramePacket::GetData>("data"),
            InstanceAccessor<&JsFramePacket::GetReleased>("released"),
            InstanceMethod<&JsFramePacket::Release>("release"),
        });
        constructor = Napi::Persistent(func);
        constructor.SuppressDestruct();
        exports.Set("FramePacket", func);
        return exports;
    }

    static Napi::Object NewInstance(Napi::Env env,
                                     std::shared_ptr<FramePacket> pkt) {
        Napi::Object obj = constructor.New({});
        auto* wrapper = Napi::ObjectWrap<JsFramePacket>::Unwrap(obj);
        wrapper->pkt_ = std::move(pkt);
        return obj;
    }

    JsFramePacket(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<JsFramePacket>(info) {}

    // ── Property getters ────────────────────────────────────────

    Napi::Value GetFrameId(const Napi::CallbackInfo& info) {
        if (!pkt_) return info.Env().Undefined();
        return Napi::Number::New(info.Env(), static_cast<double>(pkt_->frame_id));
    }

    Napi::Value GetWidth(const Napi::CallbackInfo& info) {
        if (!pkt_) return info.Env().Undefined();
        return Napi::Number::New(info.Env(), pkt_->width);
    }

    Napi::Value GetHeight(const Napi::CallbackInfo& info) {
        if (!pkt_) return info.Env().Undefined();
        return Napi::Number::New(info.Env(), pkt_->height);
    }

    Napi::Value GetStride(const Napi::CallbackInfo& info) {
        if (!pkt_) return info.Env().Undefined();
        return Napi::Number::New(info.Env(), pkt_->stride);
    }

    Napi::Value GetCaptureQpc(const Napi::CallbackInfo& info) {
        if (!pkt_) return info.Env().Undefined();
        return Napi::BigInt::New(info.Env(), static_cast<int64_t>(pkt_->capture_qpc));
    }

    Napi::Value GetHostAcceptQpc(const Napi::CallbackInfo& info) {
        if (!pkt_) return info.Env().Undefined();
        return Napi::BigInt::New(info.Env(), static_cast<int64_t>(pkt_->host_accept_qpc));
    }

    Napi::Value GetKeyboardMask(const Napi::CallbackInfo& info) {
        if (!pkt_) return info.Env().Undefined();
        return Napi::BigInt::New(info.Env(), static_cast<uint64_t>(pkt_->keyboard_mask));
    }

    Napi::Value GetData(const Napi::CallbackInfo& info) {
        if (!pkt_ || pkt_->IsReleased()) {
            Napi::TypeError::New(info.Env(), "Packet already released")
                .ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        // Copy pixel data into a JS-owned buffer.
        // (Electron forbids external buffers backed by native memory.)
        return Napi::Buffer<uint8_t>::Copy(
            info.Env(),
            pkt_->pixel_data.data(),
            pkt_->pixel_data.size()
        );
    }

    Napi::Value GetReleased(const Napi::CallbackInfo& info) {
        if (!pkt_) return Napi::Boolean::New(info.Env(), true);
        return Napi::Boolean::New(info.Env(), pkt_->IsReleased());
    }

    void Release(const Napi::CallbackInfo&) {
        if (pkt_) pkt_->Release();
    }
};

Napi::FunctionReference JsFramePacket::constructor;

// ─── JsCaptureEngine ────────────────────────────────────────────

class JsCaptureEngine : public Napi::ObjectWrap<JsCaptureEngine> {
    std::unique_ptr<CaptureEngine> engine_;

public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "CaptureEngine", {
            InstanceMethod<&JsCaptureEngine::Start>("start"),
            InstanceMethod<&JsCaptureEngine::Stop>("stop"),
            InstanceMethod<&JsCaptureEngine::GetNextFrame>("getNextFrame"),
            InstanceMethod<&JsCaptureEngine::StartRecording>("startRecording"),
            InstanceMethod<&JsCaptureEngine::StopRecording>("stopRecording"),
            InstanceMethod<&JsCaptureEngine::IsRecording>("isRecording"),
            InstanceMethod<&JsCaptureEngine::Stats>("stats"),
            InstanceMethod<&JsCaptureEngine::LastError>("lastError"),
        });
        exports.Set("CaptureEngine", func);
        return exports;
    }

    JsCaptureEngine(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<JsCaptureEngine>(info) {

        Napi::Env env = info.Env();

        if (info.Length() < 1 || !info[0].IsObject()) {
            Napi::TypeError::New(env, "Expected options object")
                .ThrowAsJavaScriptException();
            return;
        }

        Napi::Object opts = info[0].As<Napi::Object>();
        EngineConfig cfg;

        // Parse target
        if (!opts.Has("target") || !opts.Get("target").IsObject()) {
            Napi::TypeError::New(env, "options.target is required")
                .ThrowAsJavaScriptException();
            return;
        }

        Napi::Object target = opts.Get("target").As<Napi::Object>();
        std::string type = target.Get("type").As<Napi::String>().Utf8Value();

        if (type == "monitor") {
            cfg.target.type = CaptureTargetType::MonitorIndex;
            cfg.target.monitor_index = target.Get("index").As<Napi::Number>().Int32Value();
        } else if (type == "windowTitle") {
            cfg.target.type = CaptureTargetType::WindowTitleRegex;
            std::string s = target.Get("pattern").As<Napi::String>().Utf8Value();
            cfg.target.value_wstr = std::wstring(s.begin(), s.end());
        } else if (type == "windowExe") {
            cfg.target.type = CaptureTargetType::WindowExeRegex;
            std::string s = target.Get("pattern").As<Napi::String>().Utf8Value();
            cfg.target.value_wstr = std::wstring(s.begin(), s.end());
        } else {
            Napi::TypeError::New(env, "Unknown target type: " + type)
                .ThrowAsJavaScriptException();
            return;
        }

        // Parse optional fields
        if (opts.Has("maxFps") && !opts.Get("maxFps").IsUndefined())
            cfg.max_fps = opts.Get("maxFps").As<Napi::Number>().DoubleValue();
        if (opts.Has("queueCapacity") && !opts.Get("queueCapacity").IsUndefined())
            cfg.analysis_queue_capacity = opts.Get("queueCapacity").As<Napi::Number>().Uint32Value();
        if (opts.Has("captureCursor") && !opts.Get("captureCursor").IsUndefined())
            cfg.capture_cursor = opts.Get("captureCursor").As<Napi::Boolean>().Value();
        if (opts.Has("recordWidth") && !opts.Get("recordWidth").IsUndefined())
            cfg.record_width = opts.Get("recordWidth").As<Napi::Number>().Uint32Value();
        if (opts.Has("recordHeight") && !opts.Get("recordHeight").IsUndefined())
            cfg.record_height = opts.Get("recordHeight").As<Napi::Number>().Uint32Value();
        if (opts.Has("recordGop") && !opts.Get("recordGop").IsUndefined())
            cfg.record_gop = opts.Get("recordGop").As<Napi::Number>().Uint32Value();

        // Parse key map
        if (opts.Has("keys") && !opts.Get("keys").IsUndefined()) {
            Napi::Array keys = opts.Get("keys").As<Napi::Array>();
            for (uint32_t i = 0; i < keys.Length(); i++) {
                Napi::Object k = keys.Get(i).As<Napi::Object>();
                KeySpec ks{};
                ks.bit_index = k.Get("bit").As<Napi::Number>().Uint32Value();
                ks.virtual_key = k.Get("vk").As<Napi::Number>().Uint32Value();
                std::string name = k.Get("name").As<Napi::String>().Utf8Value();
                strncpy_s(ks.name, sizeof(ks.name), name.c_str(), _TRUNCATE);
                cfg.key_map.push_back(ks);
            }
        }

        engine_ = std::make_unique<CaptureEngine>(cfg);
    }

    // ── Lifecycle ───────────────────────────────────────────────

    void Start(const Napi::CallbackInfo& info) {
        try {
            engine_->Start();
        } catch (...) {
            ThrowAsJs(info.Env());
        }
    }

    void Stop(const Napi::CallbackInfo& info) {
        try {
            engine_->Stop();
        } catch (...) {
            ThrowAsJs(info.Env());
        }
    }

    // ── Frame delivery ──────────────────────────────────────────

    Napi::Value GetNextFrame(const Napi::CallbackInfo& info) {
        int timeout_ms = -1;
        if (info.Length() > 0 && !info[0].IsUndefined())
            timeout_ms = info[0].As<Napi::Number>().Int32Value();

        try {
            // Synchronous blocking call — intended for Worker threads.
            // No GIL in N-API, so other threads are unaffected.
            auto pkt = engine_->GetNextFrame(timeout_ms);
            if (!pkt) return info.Env().Null();
            return JsFramePacket::NewInstance(info.Env(), pkt);
        } catch (...) {
            ThrowAsJs(info.Env());
            return info.Env().Undefined();
        }
    }

    // ── Recording ───────────────────────────────────────────────

    Napi::Value StartRecording(const Napi::CallbackInfo& info) {
        try {
            RecordingInfo ri;

            if (info[0].IsString()) {
                // startRecording(basePath, encoder?)
                std::string basePath = info[0].As<Napi::String>().Utf8Value();
                std::string encoder;
                if (info.Length() > 1 && info[1].IsString())
                    encoder = info[1].As<Napi::String>().Utf8Value();
                ri = engine_->StartRecording(basePath, encoder);
            } else {
                // startRecording({ path, videoName, metaName, encoder? })
                Napi::Object opts = info[0].As<Napi::Object>();
                std::string path = opts.Get("path").As<Napi::String>().Utf8Value();
                std::string videoName = opts.Get("videoName").As<Napi::String>().Utf8Value();
                std::string metaName = opts.Get("metaName").As<Napi::String>().Utf8Value();
                std::string encoder;
                if (opts.Has("encoder") && !opts.Get("encoder").IsUndefined())
                    encoder = opts.Get("encoder").As<Napi::String>().Utf8Value();

                // Strip redundant extensions (matching Python behavior)
                if (videoName.size() > 4 && videoName.substr(videoName.size() - 4) == ".mp4")
                    videoName = videoName.substr(0, videoName.size() - 4);
                if (metaName.size() > 5 && metaName.substr(metaName.size() - 5) == ".meta")
                    metaName = metaName.substr(0, metaName.size() - 5);

                std::string videoPath = path + "/" + videoName + ".mp4";
                std::string metaPath = path + "/" + metaName + ".meta";
                ri = engine_->StartRecording(path, videoPath, metaPath, encoder);
            }

            Napi::Object result = Napi::Object::New(info.Env());
            result.Set("basePath", ri.base_path);
            result.Set("videoPath", ri.video_path);
            result.Set("metaPath", ri.meta_path);
            result.Set("codec", ri.codec);
            result.Set("width", static_cast<double>(ri.width));
            result.Set("height", static_cast<double>(ri.height));
            return result;
        } catch (...) {
            ThrowAsJs(info.Env());
            return info.Env().Undefined();
        }
    }

    void StopRecording(const Napi::CallbackInfo& info) {
        try {
            engine_->StopRecording();
        } catch (...) {
            ThrowAsJs(info.Env());
        }
    }

    Napi::Value IsRecording(const Napi::CallbackInfo& info) {
        return Napi::Boolean::New(info.Env(), engine_->IsRecording());
    }

    // ── Diagnostics ─────────────────────────────────────────────

    Napi::Value Stats(const Napi::CallbackInfo& info) {
        auto s = engine_->GetStats();
        Napi::Object obj = Napi::Object::New(info.Env());
        obj.Set("framesSeen", Napi::Number::New(info.Env(), static_cast<double>(s.frames_seen)));
        obj.Set("framesAccepted", Napi::Number::New(info.Env(), static_cast<double>(s.frames_accepted)));
        obj.Set("framesDroppedQueueFull", Napi::Number::New(info.Env(), static_cast<double>(s.frames_dropped_queue_full)));
        obj.Set("framesDroppedError", Napi::Number::New(info.Env(), static_cast<double>(s.frames_dropped_internal_error)));
        obj.Set("framesRecorded", Napi::Number::New(info.Env(), static_cast<double>(s.frames_recorded)));
        obj.Set("queueDepth", s.python_queue_depth);
        obj.Set("recording", s.recording_active);
        obj.Set("poolRecreates", Napi::Number::New(info.Env(), static_cast<double>(s.pool_recreates)));
        obj.Set("poolWidth", s.pool_width);
        obj.Set("poolHeight", s.pool_height);
        obj.Set("lastPoolChange", s.last_pool_change);
        obj.Set("adapter", s.adapter);
        obj.Set("errorsByStep", s.errors_by_step);
        return obj;
    }

    Napi::Value LastError(const Napi::CallbackInfo& info) {
        auto e = engine_->GetLastError();
        if (e) return Napi::String::New(info.Env(), *e);
        return info.Env().Null();
    }
};

// ─── TestWindow (for integration tests) ─────────────────────────

static LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_ERASEBKGND) {
        auto* brush = reinterpret_cast<HBRUSH>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (brush) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, brush);
            return 1;
        }
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

class JsTestWindow : public Napi::ObjectWrap<JsTestWindow> {
    HWND hwnd_ = nullptr;
    HBRUSH brush_ = nullptr;
    static bool classRegistered_;

public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "TestWindow", {
            InstanceMethod<&JsTestWindow::Fill>("fill"),
            InstanceMethod<&JsTestWindow::Destroy>("destroy"),
            InstanceAccessor<&JsTestWindow::GetTitle>("title"),
        });
        exports.Set("TestWindow", func);
        return exports;
    }

    JsTestWindow(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<JsTestWindow>(info) {

        Napi::Env env = info.Env();
        std::string title = info[0].As<Napi::String>().Utf8Value();
        int w = info.Length() > 1 ? info[1].As<Napi::Number>().Int32Value() : 640;
        int h = info.Length() > 2 ? info[2].As<Napi::Number>().Int32Value() : 480;

        std::wstring wtitle(title.begin(), title.end());

        if (!classRegistered_) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = TestWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"MemoirTestWindow";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassExW(&wc);
            classRegistered_ = true;
        }

        // Adjust window rect so client area is exactly w×h
        RECT rc = {0, 0, w, h};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        hwnd_ = CreateWindowExW(
            0, L"MemoirTestWindow", wtitle.c_str(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        if (!hwnd_) {
            Napi::Error::New(env, "Failed to create test window")
                .ThrowAsJavaScriptException();
            return;
        }

        brush_ = CreateSolidBrush(RGB(128, 128, 128));
        SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(brush_));
        InvalidateRect(hwnd_, nullptr, TRUE);
        PumpMessages();
    }

    ~JsTestWindow() {
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
        if (brush_) { DeleteObject(brush_); brush_ = nullptr; }
    }

    void Fill(const Napi::CallbackInfo& info) {
        uint8_t b = info[0].As<Napi::Number>().Uint32Value();
        uint8_t g = info[1].As<Napi::Number>().Uint32Value();
        uint8_t r = info[2].As<Napi::Number>().Uint32Value();

        HBRUSH old = brush_;
        brush_ = CreateSolidBrush(RGB(r, g, b));
        SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(brush_));
        InvalidateRect(hwnd_, nullptr, TRUE);
        PumpMessages();
        if (old) DeleteObject(old);
    }

    void Destroy(const Napi::CallbackInfo&) {
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
        if (brush_) { DeleteObject(brush_); brush_ = nullptr; }
        PumpMessages();
    }

    Napi::Value GetTitle(const Napi::CallbackInfo& info) {
        if (!hwnd_) return info.Env().Null();
        wchar_t buf[256];
        GetWindowTextW(hwnd_, buf, 256);
        std::wstring ws(buf);
        return Napi::String::New(info.Env(), std::string(ws.begin(), ws.end()));
    }

private:
    static void PumpMessages() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
};

bool JsTestWindow::classRegistered_ = false;

// ─── Module init ────────────────────────────────────────────────

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    JsFramePacket::Init(env, exports);
    JsCaptureEngine::Init(env, exports);
    JsTestWindow::Init(env, exports);

    exports.Set("version", Napi::String::New(env, MEMOIR_VERSION_STRING));
    exports.Set("ping", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(),
            std::string("memoir-node ") + MEMOIR_VERSION_STRING + " loaded OK");
    }));

    return exports;
}

NODE_API_MODULE(memoir_node, Init)
