#pragma once

#include "SpaceModel.hpp"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>

namespace hyprspace::spaces {

    std::filesystem::path      spaceStatePath(const char* stateHome, const char* userHome);
    nlohmann::json             serialize(const CSpaceModel& model);
    std::optional<CSpaceModel> deserialize(const nlohmann::json& value, std::string* error = nullptr);

    struct SLoadResult {
        std::optional<CSpaceModel> model;
        bool                       recovered = false;
        std::string                error;
    };

    SLoadResult load(const std::filesystem::path& file);
    bool        save(const std::filesystem::path& file, const CSpaceModel& model, std::string* error = nullptr);

    // The worker only owns immutable model copies. Compositor objects never
    // cross this boundary. Pending saves coalesce to the newest generation.
    class CSpaceStore {
      public:
        explicit CSpaceStore(std::filesystem::path file);
        ~CSpaceStore();
        CSpaceStore(const CSpaceStore&)            = delete;
        CSpaceStore& operator=(const CSpaceStore&) = delete;

        void        submit(const CSpaceModel& model);
        bool        flush();
        std::string error() const;

      private:
        void run();

        std::filesystem::path      m_file;
        mutable std::mutex         m_mutex;
        std::condition_variable    m_wake, m_flushed;
        std::optional<CSpaceModel> m_pending;
        uint64_t                   m_requested = 0, m_completed = 0, m_flushRequested = 0;
        std::string                m_error;
        bool                       m_stopping = false;
        std::thread                m_worker;
    };

} // namespace hyprspace::spaces
