#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hyprspace {

    struct SEmptyWorkspaceCandidate {
        int64_t id = 0, monitor = 0;
        bool    empty = false, special = false, active = false, prepared = false, allowed = true;
    };

    // Monitor matching is performed by the native monitor selector, outside
    // this host-testable helper. A nonempty binding to an absent output does
    // not match the target and reserves its workspace numbers.
    struct SEmptyWorkspaceRule {
        std::string                selector;
        bool                       monitorBinding = false, targetMonitor = false, enabled = true;
        std::optional<std::string> defaultName = std::nullopt;
    };

    struct SEmptyWorkspaceChoice {
        int64_t id       = 0;
        bool    existing = false;
    };

    // Native numeric workspace dispatchers parse with std::stoi even though
    // the stored workspace identity is wider.
    inline constexpr int64_t MAX_EMPTY_WORKSPACE_ID = std::numeric_limits<int32_t>::max();

    namespace emptyWorkspaceDetail {
        struct SInterval {
            int64_t first = 1, last = std::numeric_limits<int64_t>::max();
        };

        inline std::string_view trim(std::string_view value) {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
        }

        inline std::optional<int64_t> positiveNumber(std::string_view text) {
            if (text.empty())
                return std::nullopt;
            int64_t value           = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size() && value > 0 ? std::optional{value} : std::nullopt;
        }

        // Only identity-based selectors can be evaluated without constructing
        // a workspace. Native construction has events and on-created hooks.
        inline std::optional<SInterval> interval(std::string_view selector) {
            selector = trim(selector);
            if (selector.empty())
                return SInterval{};
            if (const auto id = positiveNumber(selector))
                return SInterval{*id, *id};
            if (!selector.starts_with("r[") || !selector.ends_with("]"))
                return std::nullopt;
            const auto range = selector.substr(2, selector.size() - 3);
            const auto dash  = range.find('-');
            if (dash == std::string_view::npos)
                return std::nullopt;
            const auto first = positiveNumber(range.substr(0, dash)), last = positiveNumber(range.substr(dash + 1));
            return first && last && *first <= *last ? std::optional{SInterval{*first, *last}} : std::nullopt;
        }

        inline std::optional<SInterval> initialNameInterval(std::string_view selector) {
            if (const auto range = interval(selector))
                return range;
            selector = trim(selector);
            if (selector.starts_with("name:"))
                if (const auto id = positiveNumber(selector.substr(5)))
                    return SInterval{*id, *id};
            return std::nullopt;
        }
    } // namespace emptyWorkspaceDetail

    // Reusing a local empty is independent of fresh-ID reservations. Every
    // existing positive ID is occupied, including an empty on another output.
    inline std::expected<SEmptyWorkspaceChoice, std::string> chooseEmptyWorkspace(std::span<const SEmptyWorkspaceCandidate> workspaces, int64_t monitor,
                                                                                  std::span<const SEmptyWorkspaceRule> rules) {
        using namespace emptyWorkspaceDetail;
        const SEmptyWorkspaceCandidate* chosen   = nullptr;
        const auto                      priority = [](const auto& ws) { return ws.prepared ? 0 : ws.active ? 1 : 2; };
        for (const auto& ws : workspaces) {
            if (ws.id == 0 || ws.monitor != monitor || !ws.empty || ws.special || !ws.allowed)
                continue;
            if (!chosen || priority(ws) < priority(*chosen) || (priority(ws) == priority(*chosen) && ws.id < chosen->id))
                chosen = &ws;
        }
        if (chosen)
            return SEmptyWorkspaceChoice{chosen->id, true};

        std::vector<SInterval> reserved;
        for (const auto& ws : workspaces)
            if (ws.id > 0)
                reserved.push_back({ws.id, ws.id});
        for (const auto& rule : rules) {
            if (!rule.enabled || !rule.monitorBinding)
                continue;
            const auto selector = trim(rule.selector);
            if (const auto range = interval(selector)) {
                if (!rule.targetMonitor)
                    reserved.push_back(*range);
            } else if (selector.starts_with("name:")) {
                if (rule.targetMonitor)
                    continue;
                const auto name = selector.substr(5);
                // A positive workspace's initial name is its number; native
                // default_name can subsequently make other IDs match name:.
                if (const auto id = positiveNumber(name))
                    reserved.push_back({*id, *id});
                for (const auto& naming : rules) {
                    if (!naming.enabled || !naming.defaultName || *naming.defaultName != name)
                        continue;
                    if (const auto range = initialNameInterval(naming.selector))
                        reserved.push_back(*range);
                    else if (!trim(naming.selector).starts_with("special") && !trim(naming.selector).starts_with("name:"))
                        return std::unexpected("hyprspace: conditional default_name cannot be allocated safely: " + naming.selector);
                }
            } else if (!selector.starts_with("special"))
                return std::unexpected("hyprspace: conditional workspace monitor binding cannot be allocated safely: " + rule.selector);
        }

        // Jump over intervals, rather than iterating potentially enormous
        // configured ranges. Conflicting bindings are conservatively reserved.
        std::ranges::sort(reserved, {}, &SInterval::first);
        int64_t candidate = 1;
        for (const auto& range : reserved) {
            if (range.first > candidate)
                break;
            if (range.last < candidate)
                continue;
            if (range.last >= MAX_EMPTY_WORKSPACE_ID)
                return std::unexpected("hyprspace: no available workspace number on this monitor");
            candidate = range.last + 1;
        }
        return SEmptyWorkspaceChoice{candidate, false};
    }

} // namespace hyprspace
