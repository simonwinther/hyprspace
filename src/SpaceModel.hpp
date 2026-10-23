#pragma once

#include "SpaceAppearance.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/random.h>

namespace hyprspace::spaces {

    inline std::string newId() {
        std::array<unsigned char, 16> bytes{};
        size_t                        offset = 0;
        while (offset < bytes.size()) {
            const auto count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error("cannot allocate a space identity");
            offset += static_cast<size_t>(count);
        }
        bytes[6]                = (bytes[6] & 0x0f) | 0x40;
        bytes[8]                = (bytes[8] & 0x3f) | 0x80;
        constexpr char digits[] = "0123456789abcdef";
        std::string    result;
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                result += '-';
            result += digits[bytes[i] >> 4];
            result += digits[bytes[i] & 15];
        }
        return result;
    }

    inline bool validId(std::string_view id) {
        if (id.size() != 36)
            return false;
        for (size_t i = 0; i < id.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (id[i] != '-')
                    return false;
            } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
                return false;
        }
        return true;
    }

    inline std::string trimName(std::string_view name) {
        constexpr std::string_view whitespace = " \t\r\n\f\v";
        const auto                 first      = name.find_first_not_of(whitespace);
        return first == std::string_view::npos ? std::string{} : std::string(name.substr(first, name.find_last_not_of(whitespace) - first + 1));
    }

    inline bool validUtf8(std::string_view value) {
        for (size_t i = 0; i < value.size();) {
            const auto first = static_cast<unsigned char>(value[i++]);
            if (first < 0x80)
                continue;
            const size_t count = first >= 0xc2 && first <= 0xdf ? 1 : first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
            if (!count || i + count > value.size())
                return false;
            const auto second = static_cast<unsigned char>(value[i]);
            if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) || (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90))
                return false;
            for (size_t end = i + count; i < end; ++i)
                if ((static_cast<unsigned char>(value[i]) & 0xc0) != 0x80)
                    return false;
        }
        return true;
    }

    inline bool validName(std::string_view name) {
        return !name.empty() && name.size() <= 256 && validUtf8(name) && std::ranges::none_of(name, [](unsigned char c) { return c < 0x20 || c == 0x7f; }) &&
               trimName(name) == name;
    }

    inline std::string suffixedWorkspaceName(std::string_view base, size_t suffix) {
        const auto tail   = " (" + std::to_string(suffix) + ")";
        size_t     length = std::min(base.size(), 256 - tail.size());
        while (length < base.size() && length && (static_cast<unsigned char>(base[length]) & 0xc0) == 0x80)
            --length;
        return std::string(base.substr(0, length)) + tail;
    }

    inline std::string managedWorkspaceName(std::string_view workspaceId) {
        return "hyprspace:" + std::string(workspaceId);
    }

    struct SSpace {
        std::string                        id, name;
        std::map<std::string, std::string> selections;
        std::string                        icon{DEFAULT_SPACE_ICON}, color{DEFAULT_SPACE_COLOR};
        bool                               operator==(const SSpace&) const = default;
    };

    struct SSpaceWorkspace {
        std::string id, space;
        int64_t     number = 0; // Zero denotes a named or special workspace.
        std::string name;
        bool        special  = false;
        int64_t     nativeId = 0; // Zero means not yet materialized in this compositor.
        std::string nativeName, preferredMonitor;
        bool        operator==(const SSpaceWorkspace&) const = default;
    };

    class CSpaceModel {
      public:
        std::vector<SSpace>          spaces;
        std::vector<SSpaceWorkspace> workspaces;
        std::string                  active, defaultSpace;
        // Runtime reconciliation discards intents from other compositor instances.
        std::map<std::string, bool> runtimePins;

        CSpaceModel() {
            ensureDefault();
        }

        void ensureDefault() {
            if (spaces.empty()) {
                spaces.push_back({.id = newId(), .name = "Default", .selections = {}});
                defaultSpace = spaces.front().id;
            }
            if (!space(defaultSpace))
                defaultSpace = spaces.front().id;
            if (!space(active))
                active = defaultSpace;
        }

        SSpace* space(std::string_view id) {
            return const_cast<SSpace*>(std::as_const(*this).space(id));
        }
        const SSpace* space(std::string_view id) const {
            const auto found = std::ranges::find(spaces, id, &SSpace::id);
            return found == spaces.end() ? nullptr : &*found;
        }
        SSpace* spaceNamed(std::string_view name) {
            return const_cast<SSpace*>(std::as_const(*this).spaceNamed(name));
        }
        const SSpace* spaceNamed(std::string_view name) const {
            const auto found = std::ranges::find(spaces, name, &SSpace::name);
            return found == spaces.end() ? nullptr : &*found;
        }
        SSpaceWorkspace* workspace(std::string_view id) {
            return const_cast<SSpaceWorkspace*>(std::as_const(*this).workspace(id));
        }
        const SSpaceWorkspace* workspace(std::string_view id) const {
            const auto found = std::ranges::find(workspaces, id, &SSpaceWorkspace::id);
            return found == workspaces.end() ? nullptr : &*found;
        }
        SSpaceWorkspace* byNative(int64_t id) {
            return const_cast<SSpaceWorkspace*>(std::as_const(*this).byNative(id));
        }
        const SSpaceWorkspace* byNative(int64_t id) const {
            if (!id)
                return nullptr;
            const auto found = std::ranges::find(workspaces, id, &SSpaceWorkspace::nativeId);
            return found == workspaces.end() ? nullptr : &*found;
        }
        SSpaceWorkspace* local(std::string_view owner, int64_t number) {
            return const_cast<SSpaceWorkspace*>(std::as_const(*this).local(owner, number));
        }
        const SSpaceWorkspace* local(std::string_view owner, int64_t number) const {
            if (number <= 0)
                return nullptr;
            const auto found = std::ranges::find_if(workspaces, [&](const auto& ws) { return ws.space == owner && ws.number == number && !ws.special; });
            return found == workspaces.end() ? nullptr : &*found;
        }
        SSpaceWorkspace* named(std::string_view owner, std::string_view name, bool special = false) {
            return const_cast<SSpaceWorkspace*>(std::as_const(*this).named(owner, name, special));
        }
        const SSpaceWorkspace* named(std::string_view owner, std::string_view name, bool special = false) const {
            const auto found = std::ranges::find_if(workspaces, [&](const auto& ws) { return ws.space == owner && ws.name == name && ws.special == special; });
            return found == workspaces.end() ? nullptr : &*found;
        }

        std::optional<std::string> create(std::string name, std::string* error = nullptr, std::string icon = std::string(DEFAULT_SPACE_ICON),
                                          std::string color = std::string(DEFAULT_SPACE_COLOR)) {
            name = trimName(name);
            if (!validName(name))
                return failOptional(error, "space names must contain 1–256 bytes without control characters");
            if (spaceNamed(name))
                return failOptional(error, "a space with that name already exists");
            if (!validSpaceIcon(icon))
                return failOptional(error, "unknown space icon");
            const auto normalized = normalizeSpaceColor(color);
            if (!normalized)
                return failOptional(error, "space color must be #RRGGBB");
            const auto id = newId();
            spaces.push_back({.id = id, .name = std::move(name), .selections = {}, .icon = std::move(icon), .color = *normalized});
            return id;
        }

        bool rename(std::string_view id, std::string name, std::string* error = nullptr) {
            return update(id, std::move(name), std::nullopt, std::nullopt, error);
        }

        // Move relative to a stable identity, so filtered choosers can reorder
        // neighboring matches without changing the relative order of others.
        bool reorder(std::string_view id, std::string_view target, bool after, std::string* error = nullptr) {
            const auto source = std::ranges::find(spaces, id, &SSpace::id);
            const auto anchor = std::ranges::find(spaces, target, &SSpace::id);
            if (source == spaces.end() || anchor == spaces.end())
                return fail(error, "space does not exist");
            if (source == anchor)
                return true;
            const auto from = source - spaces.begin();
            const auto to   = anchor - spaces.begin() + (after ? 1 : 0) - (source < anchor ? 1 : 0);
            if (from < to)
                std::rotate(spaces.begin() + from, spaces.begin() + from + 1, spaces.begin() + to + 1);
            else if (from > to)
                std::rotate(spaces.begin() + to, spaces.begin() + from, spaces.begin() + from + 1);
            return true;
        }

        bool update(std::string_view id, std::optional<std::string> name = std::nullopt, std::optional<std::string> icon = std::nullopt,
                    std::optional<std::string> color = std::nullopt, std::string* error = nullptr) {
            auto* target = space(id);
            if (!target)
                return fail(error, "space does not exist");
            auto nextName = name ? trimName(*name) : target->name;
            if (!validName(nextName))
                return fail(error, "space names must contain 1–256 bytes without control characters");
            if (const auto* other = spaceNamed(nextName); other && other != target)
                return fail(error, "a space with that name already exists");
            auto nextIcon = icon.value_or(target->icon);
            if (!validSpaceIcon(nextIcon))
                return fail(error, "unknown space icon");
            const auto nextColor = normalizeSpaceColor(color.value_or(target->color));
            if (!nextColor)
                return fail(error, "space color must be #RRGGBB");
            target->name  = std::move(nextName);
            target->icon  = std::move(nextIcon);
            target->color = *nextColor;
            return true;
        }

        int64_t lowestNumber(std::string_view owner) const {
            std::set<int64_t> used;
            for (const auto& ws : workspaces)
                if (ws.space == owner && ws.number > 0)
                    used.insert(ws.number);
            return firstFree(used);
        }

        std::optional<std::string> addWorkspace(std::string_view owner, int64_t number, std::string name, bool special, int64_t nativeId = 0, std::string nativeName = {},
                                                std::string monitor = {}, std::string* error = nullptr) {
            if (!space(owner))
                return failOptional(error, "space does not exist");
            if (number < 0 || (special && number != 0))
                return failOptional(error, "invalid local workspace number");
            if (name.empty() && number > 0)
                name = std::to_string(number);
            if (!validName(name))
                return failOptional(error, "invalid workspace name");
            if ((number > 0 && local(owner, number)) || named(owner, name, special))
                return failOptional(error, "workspace identity already exists in this space");
            if (byNative(nativeId) || (!nativeName.empty() && std::ranges::any_of(workspaces, [&](const auto& ws) { return ws.nativeName == nativeName; })))
                return failOptional(error, "native workspace already belongs to a space");
            const auto id = newId();
            workspaces.push_back({id, std::string(owner), number, std::move(name), special, nativeId, std::move(nativeName), std::move(monitor)});
            return id;
        }

        bool forgetWorkspace(std::string_view id) {
            if (!workspace(id))
                return false;
            const std::string identity(id);
            for (auto& owner : spaces)
                std::erase_if(owner.selections, [&](const auto& selection) { return selection.second == identity; });
            std::erase_if(workspaces, [&](const auto& ws) { return ws.id == identity; });
            return true;
        }

        bool select(std::string_view owner, std::string monitor, std::string_view id) {
            auto*       target = space(owner);
            const auto* ws     = workspace(id);
            if (!target || !ws || ws->space != owner || monitor.empty() || ws->special != monitor.starts_with("special:"))
                return false;
            target->selections[std::move(monitor)] = std::string(id);
            return true;
        }

        const SSpaceWorkspace* selection(std::string_view owner, std::string_view monitor) const {
            const auto* target = space(owner);
            if (!target)
                return nullptr;
            const auto found = target->selections.find(std::string(monitor));
            return found == target->selections.end() ? nullptr : workspace(found->second);
        }

        bool moveWorkspace(std::string_view id, std::string_view destination, std::string* error = nullptr) {
            const auto* ws = workspace(id);
            if (!ws || !space(destination))
                return fail(error, "workspace or destination does not exist");
            if (ws->space == destination)
                return true;
            auto copy = *this;
            if (!copy.transfer({std::string(id)}, destination, error))
                return false;
            *this = std::move(copy);
            return true;
        }

        bool erase(std::string_view id, std::string_view destination, std::string* error = nullptr) {
            if (!space(id) || !space(destination) || id == destination || spaces.size() == 1)
                return fail(error, "choose a different existing destination space");
            auto                     copy = *this;
            std::vector<std::string> ids;
            for (const auto& ws : workspaces)
                if (ws.space == id)
                    ids.push_back(ws.id);
            const auto selections = space(id)->selections;
            if (!copy.transfer(ids, destination, error))
                return false;
            for (const auto& [monitor, selected] : selections)
                copy.space(destination)->selections.try_emplace(monitor, selected);
            std::erase_if(copy.spaces, [&](const auto& owner) { return owner.id == id; });
            if (copy.active == id)
                copy.active = destination;
            if (copy.defaultSpace == id)
                copy.defaultSpace = destination;
            *this = std::move(copy);
            return true;
        }

        bool validate(std::string* error = nullptr) const {
            if (spaces.empty() || !space(active) || !space(defaultSpace))
                return fail(error, "state must contain active and default spaces");
            // Build indexes once: saved state may contain many dormant
            // workspaces and output selections. Repeated vector lookups here
            // made validating a complete state graph quadratic.
            std::set<std::string_view>                         spaceIds, names, nativeNames;
            std::map<std::string_view, const SSpaceWorkspace*> workspaceIds;
            std::set<std::string>                              logical;
            std::set<int64_t>                                  nativeIds;
            for (const auto& owner : spaces)
                if (!validId(owner.id) || !validName(owner.name) || !validSpaceIcon(owner.icon) || normalizeSpaceColor(owner.color) != owner.color ||
                    !spaceIds.insert(owner.id).second || !names.insert(owner.name).second)
                    return fail(error, "invalid or duplicate space identity");
            for (const auto& ws : workspaces) {
                if (!validId(ws.id) || spaceIds.contains(ws.id) || !workspaceIds.emplace(ws.id, &ws).second || !spaceIds.contains(ws.space) || !validName(ws.name) ||
                    ws.number < 0 || (ws.special && ws.number != 0))
                    return fail(error, "invalid workspace identity");
                const auto key = ws.space + (ws.special ? "/special/" : "/name/") + ws.name;
                if (!logical.insert(key).second || (ws.number > 0 && !logical.insert(ws.space + "/number/" + std::to_string(ws.number)).second))
                    return fail(error, "duplicate local workspace identity");
                if ((ws.nativeId && !nativeIds.insert(ws.nativeId).second) || (!ws.nativeName.empty() && !nativeNames.insert(ws.nativeName).second))
                    return fail(error, "duplicate native workspace identity");
            }
            for (const auto& owner : spaces)
                for (const auto& [monitor, selected] : owner.selections) {
                    const auto  found = workspaceIds.find(selected);
                    const auto* ws    = found == workspaceIds.end() ? nullptr : found->second;
                    if (monitor.empty() || !ws || ws->space != owner.id || ws->special != monitor.starts_with("special:"))
                        return fail(error, "invalid monitor selection");
                }
            for (const auto& [key, _] : runtimePins)
                if (key.empty() || key.size() > 1024 || !validUtf8(key) || std::ranges::any_of(key, [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
                    return fail(error, "invalid pin intent identity");
            return true;
        }

      private:
        static bool fail(std::string* error, std::string message) {
            if (error)
                *error = std::move(message);
            return false;
        }
        static std::optional<std::string> failOptional(std::string* error, std::string message) {
            fail(error, std::move(message));
            return std::nullopt;
        }
        static int64_t firstFree(const std::set<int64_t>& used) {
            int64_t candidate = 1;
            for (const auto number : used) {
                if (number > candidate)
                    break;
                if (number == candidate) {
                    if (candidate == std::numeric_limits<int64_t>::max())
                        return 0;
                    ++candidate;
                }
            }
            return candidate;
        }
        bool transfer(const std::vector<std::string>& ids, std::string_view destination, std::string* error) {
            std::set<int64_t> used;
            for (const auto& ws : workspaces)
                if (ws.space == destination && ws.number > 0)
                    used.insert(ws.number);
            std::set<std::string> renumber;
            // Reserve all nonconflicting source numbers before allocating collisions.
            for (const auto& id : ids) {
                const auto* ws = workspace(id);
                if (ws->number > 0 && !used.insert(ws->number).second)
                    renumber.insert(id);
            }
            for (const auto& id : ids) {
                auto* ws = workspace(id);
                for (auto& owner : spaces)
                    std::erase_if(owner.selections, [&](const auto& selected) { return selected.second == id; });
                if (renumber.contains(id)) {
                    const auto number = firstFree(used);
                    if (!number)
                        return fail(error, "local workspace numbers exhausted");
                    if (ws->name == std::to_string(ws->number))
                        ws->name = std::to_string(number);
                    ws->number = number;
                    used.insert(number);
                }
            }
            std::set<std::pair<bool, std::string>> names;
            std::set<std::string>                  rename;
            for (const auto& ws : workspaces)
                if (ws.space == destination)
                    names.emplace(ws.special, ws.name);
            for (const auto& id : ids) {
                const auto* ws = workspace(id);
                if (!names.emplace(ws->special, ws->name).second)
                    rename.insert(id);
            }
            for (const auto& id : ids) {
                auto* ws = workspace(id);
                if (rename.contains(id)) {
                    const auto base   = ws->name;
                    size_t     suffix = 2;
                    do {
                        ws->name = suffixedWorkspaceName(base, suffix++);
                    } while (!names.emplace(ws->special, ws->name).second);
                }
                ws->space = destination;
            }
            return true;
        }
    };

} // namespace hyprspace::spaces
