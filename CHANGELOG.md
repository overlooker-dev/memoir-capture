# Changelog

## 0.3.2

### Fixed
- **Process crash when capture failed to start.** `start()` (and `stop()`)
  could fail with a WinRT `hresult_error`, which does not derive from
  `std::exception`. The Node binding only caught `std::exception`, and it is
  built without `NAPI_CPP_EXCEPTIONS`, so the error escaped into V8 and
  terminated the host process via a fail-fast (no minidump). Every engine
  step now reports WinRT failures as a regular error naming the step and the
  HRESULT, e.g. `CreateForWindow: HRESULT 0x80070057: The parameter is
  incorrect.`, and every binding method has a catch-all.
- **Use-after-free on `stop()`.** `stop()` revoked the `FrameArrived` handler
  and then released the frame pool and D3D device without waiting for
  handlers already running on WinRT thread-pool threads. It now waits
  (bounded, 2s) for in-flight handlers first. The handler body is also
  serialised, since a free-threaded pool can invoke it concurrently.
- A failed `start()` releases what it created and leaves the engine startable.
- `start()` rejects a capture target with an empty size instead of creating a
  zero-sized frame pool.

### Added
- Frame errors are no longer silent: `lastError()` now reports the failing
  step and HRESULT (with the removal reason for `DXGI_ERROR_DEVICE_REMOVED`),
  and `stats()` gains `errorsByStep`, `poolRecreates`, `poolWidth`,
  `poolHeight`, `lastPoolChange` and `adapter` (the D3D adapter used for
  capture).

## 0.3.1

### Fixed
- `CaptureEngine` now recreates the WGC `FramePool` when the target
  window's `ContentSize` changes. Previously the pool was sized once at
  `start()` and frames were clamped to that initial size — so a window
  that was tiny when capture began (e.g. a game still on its launch
  splash reporting 160×28) produced frames stuck at that resolution even
  after the window grew. Matches Microsoft's canonical
  `ScreenCaptureforHWND` pattern.

## 0.3.0

### Removed
- **Breaking:** `libx265` software encoder support. Bundling x265 (GPL)
  would force the whole project — and any consumer — to be GPL-licensed.
  Recording now requires a hardware HEVC encoder (`hevc_nvenc` or
  `hevc_amf`). Calls passing `encoder="libx265"` will raise
  `RuntimeError: Encoder not found: libx265`.

## 0.2.0

### Added
- **Node.js N-API bindings** — new `memoir-node` npm package with synchronous
  blocking API designed for Worker thread tick loops.
- `CaptureEngine` class for Node.js with `getNextFrame()`, `startRecording()`,
  `stopRecording()`, `stats()`, `lastError()`, and split-path recording.
- `FramePacket` with zero-copy Buffer access, BigInt for 64-bit fields.
- Pure TypeScript `.meta` reader/writer (`readMeta`, `writeMeta`) with helpers
  (`isPressed`, `pressedKeys`, `synthesizeKeyEvents`).
- `TestWindow` native helper for integration tests (Win32 window creation).
- Full Node.js test suite (37 tests) with parity to Python tests, including
  color cycle end-to-end verification.
- `README-node.md` with Node.js API documentation, auto-swapped at publish.
- CI job for building, testing, and publishing Node.js package to npm via
  trusted publishing (OIDC).

### Changed
- Refactored `CMakeLists.txt` to compile C++ core as a static library
  (`memoir_core`) shared between Python and Node targets.
- Added `MEMOIR_BUILD_PYTHON` and `MEMOIR_BUILD_NODE` CMake options.
- Added `node` CMake preset to `CMakePresets.json`.

## 0.1.5

### Added
- Encoder fallback chain: automatically selects `hevc_nvenc` → `hevc_amf` →
  `libx265` based on available hardware. Recording no longer requires an
  NVIDIA GPU.
- `encoder` parameter on `start_recording()` to force a specific FFmpeg
  encoder (e.g. `encoder="libx265"` for software encoding).
- `libx265` software encoder bundled via vcpkg/FFmpeg for machines without
  hardware encoding support.
- Test for recording with `libx265` (`test_record_5s_libx265`).

## 0.1.4

### Fixed
- Detect window closure via `IsWindow()` check in `GetNextFrame()` when the
  WGC `Closed` event does not fire. The engine now reliably transitions to
  `Faulted` with error `"Capture target closed"` when the captured window
  is destroyed.

### Added
- Test for window-close detection (`test_window_close.py`).

## 0.1.3

### Added
- `start_recording(path=, video_name=, meta_name=)` overload for placing
  video and metadata files under a directory with separate names.
  Redundant `.mp4`/`.meta` extensions are stripped automatically.
- Syntax sugar for `MetaFile`, `FramePacket`, and engine (sequence protocol,
  key lookup helpers, `save_png`, `on_frame` callback, `grab()` convenience).

## 0.1.2

### Added
- Custom `key_map` parameter for `CaptureEngine` to select which keys are
  tracked in the keyboard bitmask.
- Engine and metadata documentation (`docs/engine.md`).

### Fixed
- Support `(?i)` case-insensitive flag in window regex patterns.

## 0.1.1

### Fixed
- Finalize recording on destructor when engine is faulted.
- CI: skip `cv2` import on headless runners, cache vcpkg on failure.
- CI: fix cache key and `save-always` deprecation issues.

### Changed
- Renamed Python import from `memoir` to `memoir_capture`.
- Renamed PyPI package to `memoir-capture`.

## 0.1.0

### Added
- Initial implementation of Memoir capture/replay module.
- Windows Graphics Capture (WGC) backend with monitor index, window title
  regex, and window exe regex targets.
- Hardware-accelerated HEVC recording via NVENC with `.mp4` + `.meta` output.
- Python bindings via pybind11 with `CaptureEngine`, `FramePacket`, and
  `RecordingInfo` types.
- Typed Python wrapper package with dataclasses for all return values.
- `.meta` binary format with per-frame keyboard bitmask, QPC timestamps,
  and resolution info.
- CI/CD with GitHub Actions, PyPI trusted publishing, and GitHub Releases.
- pytest test suite with `--headless` marker for CI runners.
