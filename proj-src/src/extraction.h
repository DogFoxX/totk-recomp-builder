#pragma once

#include <filesystem>
#include <string>
#include <atomic>
#include <stdexcept>

namespace extraction
{
    class ExtractionError : public std::runtime_error
    {
    public:
        explicit ExtractionError(const std::string& message)
            : std::runtime_error(message) {}
    };

    void extract_archive(
        const std::filesystem::path& archive_path,
        const std::filesystem::path& destination,
        const std::string& label,
        const std::atomic<bool>* stop_flag = nullptr
    );
}