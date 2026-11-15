#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace embedded_script
{
    std::filesystem::path extract(
        const std::filesystem::path& destination);

    int run(
        const std::filesystem::path& python,
        const std::filesystem::path& script,
        const std::filesystem::path& working_directory,
        const std::vector<std::filesystem::path>& arguments);

    int run_module(
        const std::filesystem::path& python,
        const std::filesystem::path& working_directory,
        const std::vector<std::string>& arguments);

    void remove(
        const std::filesystem::path& script);
}
