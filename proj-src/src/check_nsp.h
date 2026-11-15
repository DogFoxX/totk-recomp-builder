#pragma once

#include <atomic>
#include <filesystem>

namespace check_nsp
{
    struct GameFiles
    {
        std::filesystem::path base;
        std::filesystem::path update;
        std::filesystem::path keys;
        std::filesystem::path firmware;
    };

    GameFiles validate_folder(
        const std::filesystem::path& folder,
        const std::filesystem::path& setup_dir,
        const std::atomic<bool>* stop_flag = nullptr);
}