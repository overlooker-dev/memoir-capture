#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace memoir {

enum class EngineState {
    Created,
    Running,
    Stopping,
    Stopped,
    Faulted
};

enum class CaptureTargetType {
    WindowTitleRegex,
    WindowExeRegex,
    MonitorIndex
};

enum class DropPolicy { DropNew };
enum class AnalysisPixelFormat { BGRA8 };
enum class RecordCodec { HEVC };

struct KeySpec {
    uint32_t bit_index;
    uint32_t virtual_key;
    char name[32];
};

struct CaptureTarget {
    CaptureTargetType type = CaptureTargetType::MonitorIndex;
    std::wstring value_wstr;
    int32_t monitor_index = -1;
};

struct EngineConfig {
    CaptureTarget target;
    double max_fps = 10.0;
    uint32_t analysis_queue_capacity = 1;
    AnalysisPixelFormat analysis_format = AnalysisPixelFormat::BGRA8;
    DropPolicy drop_policy = DropPolicy::DropNew;
    bool capture_cursor = false;

    uint32_t record_width = 1920;
    uint32_t record_height = 1080;
    uint32_t record_gop = 1;
    RecordCodec record_codec = RecordCodec::HEVC;

    std::vector<KeySpec> key_map;
};

struct EngineStats {
    uint64_t frames_seen = 0;
    uint64_t frames_accepted = 0;
    uint64_t frames_dropped_queue_full = 0;
    uint64_t frames_dropped_internal_error = 0;
    uint64_t frames_recorded = 0;
    uint32_t python_queue_depth = 0;
    bool recording_active = false;

    // Diagnostics
    uint64_t pool_recreates = 0;      // frame pool resized to a new ContentSize
    uint32_t pool_width = 0;          // current frame pool size
    uint32_t pool_height = 0;
    std::string last_pool_change;     // e.g. "1920x1080 -> 1904x1041"
    std::string adapter;              // D3D adapter the capture device uses
    std::string errors_by_step;       // e.g. "Recreate=296, Map=2"
};

std::vector<KeySpec> GetDefaultKeyMap();

} // namespace memoir
