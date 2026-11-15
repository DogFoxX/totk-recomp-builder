#pragma once

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace download
{
    class DownloadError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct ReleaseAsset
    {
        std::string name;
        std::string url;
        std::string tag;
    };

    bool is_version_current(
        const std::filesystem::path& version_file,
        const std::string& release_tag
    );

    ReleaseAsset get_latest_release_asset(
        const std::atomic<bool>* stop_flag = nullptr
    );

    void download_file(
        const std::string& url,
        const std::filesystem::path& destination,
        const std::string& label,
        const std::atomic<bool>* stop_flag = nullptr
    );
}