#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace hyprspace::diagnostics {
    using Clock                          = std::chrono::steady_clock;
    inline constexpr size_t SAMPLE_LIMIT = 64;
    struct SFrame {
        int64_t monitor;
        double  timeMs, intervalMs, prepareMs, renderMs, captureMs, inputMs;
        size_t  captures, pixels;
    };
    inline bool                                           enabled = false;
    inline uint64_t                                       frames  = 0;
    inline std::deque<SFrame>                             samples;
    inline std::unordered_map<int64_t, Clock::time_point> lastFrames, inputs;
    struct SPrepared {
        Clock::time_point start;
        double            prepareMs, captureMs;
        size_t            captures, pixels;
    };
    inline std::unordered_map<int64_t, SPrepared> prepared;
    inline int64_t                                renderMonitor = -1;
    inline double                                 captureMs     = 0;
    inline size_t                                 captures = 0, pixels = 0;

    inline void enable(bool value) {
        if (value == enabled)
            return;
        enabled = value;
        samples.clear();
        lastFrames.clear();
        inputs.clear();
        prepared.clear();
        renderMonitor = -1;
        frames        = 0;
    }
    inline void input(int64_t monitor) {
        if (enabled) {
            if (inputs.size() >= SAMPLE_LIMIT && !inputs.contains(monitor))
                inputs.erase(inputs.begin());
            inputs.try_emplace(monitor, Clock::now());
        }
    }
    inline void capture(double duration, size_t allocatedPixels) {
        if (!enabled)
            return;
        captureMs += duration;
        ++captures;
        pixels += allocatedPixels;
    }
    inline nlohmann::json snapshot() {
        nlohmann::json result{{"enabled", enabled}, {"frames", frames}, {"sample_limit", SAMPLE_LIMIT}, {"samples", nlohmann::json::array()}};
        for (const auto& frame : samples)
            result["samples"].push_back({{"monitor", frame.monitor},
                                         {"time_ms", frame.timeMs},
                                         {"interval_ms", frame.intervalMs},
                                         {"prepare_ms", frame.prepareMs},
                                         {"render_ms", frame.renderMs},
                                         {"capture_ms", frame.captureMs},
                                         {"input_ms", frame.inputMs},
                                         {"captures", frame.captures},
                                         {"pixels", frame.pixels}});
        return result;
    }
    inline double milliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }
    // RENDER_POST is after the native pass executes, before output commit. This
    // measures CPU render submission; GPU completion and presentation are external.
    inline void finish() {
        if (!enabled)
            return;
        const auto frame = prepared.find(renderMonitor);
        if (frame == prepared.end())
            return;
        const auto  end   = Clock::now();
        const auto  prior = lastFrames.find(renderMonitor);
        const auto  input = inputs.find(renderMonitor);
        const auto& data  = frame->second;
        samples.push_back({renderMonitor, milliseconds(end.time_since_epoch()), prior == lastFrames.end() ? 0 : milliseconds(end - prior->second), data.prepareMs,
                           milliseconds(end - data.start), data.captureMs, input == inputs.end() ? -1 : milliseconds(end - input->second), data.captures, data.pixels});
        if (samples.size() > SAMPLE_LIMIT)
            samples.pop_front();
        if (lastFrames.size() >= SAMPLE_LIMIT && !lastFrames.contains(renderMonitor))
            lastFrames.erase(lastFrames.begin());
        lastFrames[renderMonitor] = end;
        if (input != inputs.end())
            inputs.erase(input);
        prepared.erase(frame);
        ++frames;
    }
    class CFrame {
      public:
        explicit CFrame(int64_t monitor) : m_monitor(monitor), m_start(enabled && monitor >= 0 ? std::optional{Clock::now()} : std::nullopt) {
            if (m_start) {
                captureMs = 0;
                captures = pixels = 0;
            }
        }
        ~CFrame() {
            if (!m_start || !enabled)
                return;
            if (prepared.size() >= SAMPLE_LIMIT && !prepared.contains(m_monitor))
                prepared.erase(prepared.begin());
            prepared.insert_or_assign(m_monitor, SPrepared{*m_start, milliseconds(Clock::now() - *m_start), captureMs, captures, pixels});
        }

      private:
        int64_t                          m_monitor;
        std::optional<Clock::time_point> m_start;
    };
} // namespace hyprspace::diagnostics
