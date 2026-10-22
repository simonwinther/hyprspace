#include "SpaceStore.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hyprspace::spaces {
    namespace {
        constexpr size_t MAX_STATE_BYTES = 16 * 1024 * 1024;

        bool failure(std::string* error, std::string message) {
            if (error)
                *error = std::move(message);
            return false;
        }

        std::optional<std::string> readState(const std::filesystem::path& file, std::string* error) {
            std::error_code code;
            const auto      size = std::filesystem::file_size(file, code);
            if (code || size > MAX_STATE_BYTES) {
                failure(error, code ? code.message() : "space state exceeds the size limit");
                return std::nullopt;
            }
            std::ifstream input(file, std::ios::binary);
            if (!input) {
                failure(error, "cannot open space state");
                return std::nullopt;
            }
            std::string contents(static_cast<size_t>(size), '\0');
            input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!input || input.peek() != std::char_traits<char>::eof()) {
                failure(error, "space state changed or could not be read");
                return std::nullopt;
            }
            return contents;
        }

        std::optional<CSpaceModel> decode(const std::string& contents, std::string* error) {
            try {
                return deserialize(nlohmann::json::parse(contents), error);
            } catch (const std::exception& exception) {
                failure(error, exception.what());
                return std::nullopt;
            }
        }

        bool atomicWrite(const std::filesystem::path& file, const std::string& contents, std::string* error) {
            auto temporary  = file.string() + ".XXXXXX";
            int  descriptor = mkstemp(temporary.data());
            if (descriptor < 0)
                return failure(error, std::strerror(errno));
            const auto cleanup = [&] {
                if (descriptor >= 0)
                    close(descriptor);
                unlink(temporary.c_str());
            };
            size_t offset = 0;
            while (offset < contents.size()) {
                const auto count = write(descriptor, contents.data() + offset, contents.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0) {
                    const auto message = std::string(std::strerror(errno));
                    cleanup();
                    return failure(error, message);
                }
                offset += static_cast<size_t>(count);
            }
            if (fsync(descriptor) != 0) {
                const auto message = std::string(std::strerror(errno));
                cleanup();
                return failure(error, message);
            }
            const auto closed = close(descriptor);
            descriptor        = -1;
            if (closed != 0 || rename(temporary.c_str(), file.c_str()) != 0) {
                const auto message = std::string(std::strerror(errno));
                cleanup();
                return failure(error, message);
            }
            const auto directory = open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (directory < 0)
                return failure(error, std::strerror(errno));
            const auto synced = fsync(directory);
            const auto code   = errno;
            close(directory);
            return synced == 0 || failure(error, std::strerror(code));
        }

        int64_t integer(const nlohmann::json& value) {
            if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
                throw std::runtime_error("space state requires a signed 64-bit integer");
            return value.get<int64_t>();
        }
    } // namespace

    std::filesystem::path spaceStatePath(const char* stateHome, const char* userHome) {
        if (stateHome && std::filesystem::path(stateHome).is_absolute())
            return std::filesystem::path(stateHome) / "hyprspace/spaces.json";
        if (userHome && std::filesystem::path(userHome).is_absolute())
            return std::filesystem::path(userHome) / ".local/state/hyprspace/spaces.json";
        return {};
    }

    nlohmann::json serialize(const CSpaceModel& model) {
        std::string error;
        if (!model.validate(&error))
            throw std::runtime_error(error);
        nlohmann::json value{{"version", 1},
                             {"active", model.active},
                             {"default", model.defaultSpace},
                             {"spaces", nlohmann::json::array()},
                             {"workspaces", nlohmann::json::array()},
                             {"pin_intents", model.runtimePins}};
        for (const auto& owner : model.spaces)
            value["spaces"].push_back({{"id", owner.id}, {"name", owner.name}, {"selections", owner.selections}, {"icon", owner.icon}, {"color", owner.color}});
        for (const auto& ws : model.workspaces)
            value["workspaces"].push_back({{"id", ws.id},
                                           {"space", ws.space},
                                           {"number", ws.number},
                                           {"name", ws.name},
                                           {"special", ws.special},
                                           {"native_id", ws.nativeId},
                                           {"native_name", ws.nativeName},
                                           {"preferred_monitor", ws.preferredMonitor}});
        return value;
    }

    std::optional<CSpaceModel> deserialize(const nlohmann::json& value, std::string* error) {
        try {
            if (!value.is_object() || integer(value.at("version")) != 1)
                throw std::runtime_error("unsupported space state version");
            CSpaceModel model;
            model.spaces.clear();
            model.workspaces.clear();
            model.active       = value.at("active").get<std::string>();
            model.defaultSpace = value.at("default").get<std::string>();
            if (value.contains("pin_intents")) {
                if (!value.at("pin_intents").is_object())
                    throw std::runtime_error("invalid pin intents");
                model.runtimePins = value.at("pin_intents").get<std::map<std::string, bool>>();
            }
            if (!value.at("spaces").is_array() || !value.at("workspaces").is_array())
                throw std::runtime_error("invalid space state collections");
            for (const auto& owner : value.at("spaces")) {
                if (!owner.at("selections").is_object())
                    throw std::runtime_error("invalid monitor selections");
                SSpace space{owner.at("id").get<std::string>(), owner.at("name").get<std::string>(), owner.at("selections").get<std::map<std::string, std::string>>()};
                // Appearance is optional in version 1. Unknown or malformed
                // decoration must never discard workspace ownership/selection.
                if (const auto icon = owner.find("icon"); icon != owner.end() && icon->is_string() && validSpaceIcon(icon->get_ref<const std::string&>()))
                    space.icon = icon->get<std::string>();
                if (const auto color = owner.find("color"); color != owner.end() && color->is_string())
                    if (const auto normalized = normalizeSpaceColor(color->get_ref<const std::string&>()))
                        space.color = *normalized;
                model.spaces.push_back(std::move(space));
            }
            for (const auto& ws : value.at("workspaces"))
                model.workspaces.push_back({ws.at("id").get<std::string>(), ws.at("space").get<std::string>(), integer(ws.at("number")), ws.at("name").get<std::string>(),
                                            ws.at("special").get<bool>(), integer(ws.at("native_id")), ws.at("native_name").get<std::string>(),
                                            ws.at("preferred_monitor").get<std::string>()});
            if (!model.validate(error))
                return std::nullopt;
            return model;
        } catch (const std::exception& exception) {
            failure(error, exception.what());
            return std::nullopt;
        }
    }

    SLoadResult load(const std::filesystem::path& file) {
        SLoadResult result;
        if (!file.is_absolute()) {
            result.error = "space state path must be absolute";
            return result;
        }
        std::error_code code;
        const bool      primaryExists = std::filesystem::exists(file, code);
        if (code)
            result.error = code.message();
        else if (primaryExists) {
            const auto contents = readState(file, &result.error);
            if (contents)
                result.model = decode(*contents, &result.error);
            if (result.model)
                return result;
        }
        const auto backup = std::filesystem::path(file.string() + ".bak");
        if (std::filesystem::exists(backup, code)) {
            std::string backupError;
            if (const auto contents = readState(backup, &backupError))
                result.model = decode(*contents, &backupError);
            if (result.model)
                result.recovered = true;
            else
                result.error += (result.error.empty() ? "" : "; backup: ") + backupError;
        }
        return result;
    }

    bool save(const std::filesystem::path& file, const CSpaceModel& model, std::string* error) {
        if (!file.is_absolute())
            return failure(error, "space state path must be absolute");
        try {
            const auto contents = serialize(model).dump(2) + '\n';
            if (contents.size() > MAX_STATE_BYTES)
                return failure(error, "space state exceeds the size limit");
            std::error_code code;
            const bool      created = std::filesystem::create_directories(file.parent_path(), code);
            if (code)
                return failure(error, code.message());
            if (created) {
                std::filesystem::permissions(file.parent_path(), std::filesystem::perms::owner_all, code);
                if (code)
                    return failure(error, code.message());
            }
            const auto  backup = std::filesystem::path(file.string() + ".bak");
            std::string ignored;
            const auto  previous = readState(file, &ignored);
            if (previous && decode(*previous, nullptr)) {
                if (!atomicWrite(backup, *previous, error))
                    return false;
            } else {
                const auto previousBackup = readState(backup, &ignored);
                if ((!previousBackup || !decode(*previousBackup, nullptr)) && !atomicWrite(backup, contents, error))
                    return false;
            }
            return atomicWrite(file, contents, error);
        } catch (const std::exception& exception) {
            return failure(error, exception.what());
        }
    }

    CSpaceStore::CSpaceStore(std::filesystem::path file) : m_file(std::move(file)), m_worker([this] { run(); }) {}

    CSpaceStore::~CSpaceStore() {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_one();
        m_worker.join();
    }

    void CSpaceStore::submit(const CSpaceModel& model) {
        {
            std::lock_guard lock(m_mutex);
            m_pending = model;
            ++m_requested;
        }
        m_wake.notify_one();
    }

    bool CSpaceStore::flush() {
        std::unique_lock lock(m_mutex);
        const auto       requested = m_requested;
        m_flushRequested           = requested;
        m_wake.notify_one();
        m_flushed.wait(lock, [&] { return m_completed >= requested; });
        return m_error.empty();
    }

    std::string CSpaceStore::error() const {
        std::lock_guard lock(m_mutex);
        return m_error;
    }

    void CSpaceStore::run() {
        for (;;) {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stopping || m_pending.has_value(); });
            if (!m_pending)
                return;
            // Save after a short quiet period instead of issuing durable disk
            // writes for every focus/workspace event. Flush/unload bypass it.
            while (!m_stopping && m_flushRequested <= m_completed) {
                const auto generation = m_requested;
                if (!m_wake.wait_for(lock, std::chrono::milliseconds(150), [&] { return m_stopping || m_flushRequested > m_completed || m_requested != generation; }))
                    break;
            }
            auto model = std::move(*m_pending);
            m_pending.reset();
            const auto generation = m_requested;
            lock.unlock();
            std::string error;
            save(m_file, model, &error);
            lock.lock();
            m_completed = generation;
            m_error     = std::move(error);
            lock.unlock();
            m_flushed.notify_all();
        }
    }
} // namespace hyprspace::spaces
