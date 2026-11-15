#include "download.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    using json = nlohmann::json;

    constexpr const char* RELEASE_API_URL =
        "https://api.github.com/repos/valentderah/totk-pc-recomp/releases/latest";

#if defined(_WIN32)
    constexpr const char* ASSET_SUFFIX = "windows.zip";
    constexpr const char* MISSING_ASSET_ERROR =
        "Could not get a release asset: No Windows Asset Found";
#else
    constexpr const char* ASSET_SUFFIX = ".appimage";
    constexpr const char* MISSING_ASSET_ERROR =
        "Could not get a release asset: No Linux AppImage Asset Found";
#endif

    constexpr int PROGRESS_WIDTH = 50;

    bool is_stopped(const std::atomic<bool>* stop_flag)
    {
        return stop_flag != nullptr &&
               stop_flag->load(std::memory_order_relaxed);
    }

#if !defined(_WIN32)
    std::string detect_linux_ca_bundle()
    {
        for (const char* env_var : {"CURL_CA_BUNDLE", "SSL_CERT_FILE"})
        {
            if (const char* env_path = std::getenv(env_var);
                env_path != nullptr && env_path[0] != '\0')
            {
                std::error_code ec;
                if (std::filesystem::is_regular_file(env_path, ec))
                    return env_path;
            }
        }

        constexpr std::array<const char*, 5> CANDIDATE_BUNDLES = {
            "/etc/ssl/certs/ca-certificates.crt",                  // Debian, Ubuntu, Arch, Alpine
            "/etc/pki/tls/certs/ca-bundle.crt",                    // Fedora, RHEL, Rocky, Alma
            "/etc/ssl/ca-bundle.pem",                              // openSUSE, SLES
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",   // RHEL/Fedora p11-kit fallback
            "/etc/ssl/cert.pem"                                    // Generic Unix fallback
        };

        for (const char* candidate : CANDIDATE_BUNDLES)
        {
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec))
                return candidate;
        }

        return "";
    }

    std::string detect_linux_ca_dir()
    {
        if (const char* env_dir = std::getenv("SSL_CERT_DIR");
            env_dir != nullptr && env_dir[0] != '\0')
        {
            std::error_code ec;
            if (std::filesystem::is_directory(env_dir, ec))
                return env_dir;
        }

        constexpr std::array<const char*, 2> CANDIDATE_DIRS = {
            "/etc/ssl/certs",
            "/etc/pki/tls/certs"
        };

        for (const char* candidate : CANDIDATE_DIRS)
        {
            std::error_code ec;
            if (std::filesystem::is_directory(candidate, ec))
                return candidate;
        }

        return "";
    }
#endif

    void configure_ca_bundle(CURL* curl)
    {
#if !defined(_WIN32)
        static const std::string ca_bundle = detect_linux_ca_bundle();
        static const std::string ca_dir = detect_linux_ca_dir();

        if (!ca_bundle.empty())
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());

        if (!ca_dir.empty())
            curl_easy_setopt(curl, CURLOPT_CAPATH, ca_dir.c_str());
#else
        (void)curl;
#endif
    }

    struct DownloadProgress
    {
        std::chrono::steady_clock::time_point started;
        curl_off_t last_reported = -1;
        int last_filled = 0;
        std::size_t last_suffix_len = 0;
        const std::atomic<bool>* stop_flag = nullptr;
    };

    void print_progress_status(
        int filled,
        std::size_t previous_suffix_len,
        const std::string& status)
    {
        const int clamped = std::clamp(filled, 0, PROGRESS_WIDTH);
        const std::string tag = "[" + status + "]";
        const std::size_t pad = (previous_suffix_len > tag.size())
            ? (previous_suffix_len - tag.size())
            : 0;

        std::cout << "\r["
                  << std::string(clamped, '=')
                  << std::string(PROGRESS_WIDTH - clamped, ' ')
                  << "]"
                  << tag
                  << std::string(pad, ' ')
                  << '\n'
                  << std::flush;
    }

    size_t file_write_callback(
        char* data,
        size_t size,
        size_t count,
        void* userdata)
    {
        auto* file = static_cast<std::ofstream*>(userdata);
        const size_t bytes = size * count;

        file->write(data, static_cast<std::streamsize>(bytes));

        return *file ? bytes : 0;
    }

    size_t string_write_callback(
        char* data,
        size_t size,
        size_t count,
        void* userdata)
    {
        const size_t bytes = size * count;
        auto* response = static_cast<std::string*>(userdata);

        response->append(data, bytes);

        return bytes;
    }

    std::string trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");

        if (first == std::string::npos)
            return "";

        const auto last = value.find_last_not_of(" \t\r\n");

        return value.substr(first, last - first + 1);
    }

    std::string normalize_version(std::string version)
    {
        version = trim(version);

        // Remove Git version prefix.
        if (!version.empty() && version.front() == 'v')
            version.erase(0, 1);

        // Remove build metadata.
        const auto metadata = version.find('+');

        if (metadata != std::string::npos)
            version.erase(metadata);

        return version;
    }

    std::string format_speed(double bytes_per_second)
    {
        constexpr double KB = 1024.0;
        constexpr double MB = 1024.0 * KB;
        constexpr double GB = 1024.0 * MB;

        std::ostringstream stream;
        stream << std::fixed << std::setprecision(1);

        if (bytes_per_second >= GB)
            stream << bytes_per_second / GB << " GB/s";
        else if (bytes_per_second >= MB)
            stream << bytes_per_second / MB << " MB/s";
        else if (bytes_per_second >= KB)
            stream << bytes_per_second / KB << " KB/s";
        else
            stream << bytes_per_second << " B/s";

        return stream.str();
    }

    int abort_only_progress_callback(
        void* userdata,
        curl_off_t,
        curl_off_t,
        curl_off_t,
        curl_off_t)
    {
        const auto* stop_flag = static_cast<const std::atomic<bool>*>(userdata);
        return is_stopped(stop_flag) ? 1 : 0;
    }

    int progress_callback(
        void* userdata,
        curl_off_t total_download,
        curl_off_t downloaded,
        curl_off_t,
        curl_off_t)
    {
        auto* progress = static_cast<DownloadProgress*>(userdata);

        if (is_stopped(progress->stop_flag))
            return 1;

        if (total_download <= 0)
            return 0;

        if (downloaded == progress->last_reported &&
            downloaded != total_download)
            return 0;

        progress->last_reported = downloaded;

        const double fraction = std::clamp(
            static_cast<double>(downloaded) /
                static_cast<double>(total_download),
            0.0,
            1.0);

        const int filled = static_cast<int>(
            fraction * PROGRESS_WIDTH);

        progress->last_filled = filled;

        const double elapsed =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - progress->started
            ).count();

        const double mb =
            static_cast<double>(downloaded) / (1024.0 * 1024.0);

        const double total_mb =
            static_cast<double>(total_download) / (1024.0 * 1024.0);

        const double bytes_per_second =
            elapsed > 0.0
                ? static_cast<double>(downloaded) / elapsed
                : 0.0;

        const std::string suffix =
            "[" +
            std::to_string(static_cast<int>(mb)) +
            " MB / " +
            std::to_string(static_cast<int>(total_mb)) +
            " MB @ " +
            format_speed(bytes_per_second) +
            "]";

        const std::size_t pad = (progress->last_suffix_len > suffix.size())
            ? (progress->last_suffix_len - suffix.size())
            : 0;
        progress->last_suffix_len = suffix.size();

        std::cout << "\r["
                  << std::string(filled, '=')
                  << std::string(PROGRESS_WIDTH - filled, ' ')
                  << "]"
                  << suffix
                  << std::string(pad, ' ')
                  << std::flush;

        return 0;
    }

    std::string http_get_release_json(
        const std::string& url,
        const std::atomic<bool>* stop_flag)
    {
        if (is_stopped(stop_flag))
            throw std::runtime_error("Operation interrupted.");

        CURL* curl = curl_easy_init();

        if (!curl)
            throw std::runtime_error(
                "Could not get a release asset: Internal Curl Error");

        std::string response;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "TOTK-PC-Recomp-Builder/1.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, string_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_only_progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(stop_flag));
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        configure_ca_bundle(curl);

        const CURLcode result = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_easy_cleanup(curl);

        if (is_stopped(stop_flag) || result == CURLE_ABORTED_BY_CALLBACK)
        {
            throw std::runtime_error("Operation interrupted.");
        }

        if (result != CURLE_OK)
        {
            if (result == CURLE_HTTP_RETURNED_ERROR && http_code > 0)
            {
                throw std::runtime_error(
                    "Could not get a release asset: HTTP Error " +
                    std::to_string(http_code));
            }

            throw std::runtime_error(
                "Could not get a release asset: Network Error");
        }

        return response;
    }
}

namespace download
{
    ReleaseAsset get_latest_release_asset(
        const std::atomic<bool>* stop_flag)
    {
        if (is_stopped(stop_flag))
            throw std::runtime_error("Operation interrupted.");

        std::cout
            << "Getting latest valentderah/totk-pc-recomp release..."
            << std::flush;

        const std::string body = http_get_release_json(RELEASE_API_URL, stop_flag);

        json release;
        try
        {
            release = json::parse(body);
        }
        catch (const json::exception&)
        {
            throw std::runtime_error(
                "Could not get a release asset: Invalid JSON Response");
        }

        if (!release.contains("assets") ||
            !release["assets"].is_array())
        {
            throw std::runtime_error(
                "Could not get a release asset: No Assets Found");
        }

        const std::string tag =
            release.value("tag_name", "");

        if (tag.empty())
            throw std::runtime_error(
                "Could not get a release asset: Missing Release Tag");

        for (const auto& asset : release["assets"])
        {
            const std::string name = asset.value("name", "");

            std::string lower_name = name;

            std::transform(
                lower_name.begin(),
                lower_name.end(),
                lower_name.begin(),
                [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });

            if (lower_name.ends_with(ASSET_SUFFIX))
            {
                const std::string url =
                    asset.value("browser_download_url", "");

                if (url.empty())
                    throw std::runtime_error(
                        "Could not get a release asset: Missing Download URL");

                std::cout
                    << " [Found: " << name
                    << " (" << tag << ")]\n";

                return {name, url, tag};
            }
        }

        throw std::runtime_error(MISSING_ASSET_ERROR);
    }

    bool is_version_current(
        const std::filesystem::path& version_file,
        const std::string& release_tag)
    {
        if (!std::filesystem::exists(version_file))
            return false;

        std::ifstream file(version_file);

        if (!file)
            return false;

        std::string local_version;

        std::getline(file, local_version);

        local_version = normalize_version(local_version);

        const std::string latest_version =
            normalize_version(release_tag);

        return !local_version.empty() &&
               !latest_version.empty() &&
               local_version == latest_version;
    }

    void download_file(
        const std::string& url,
        const std::filesystem::path& destination,
        const std::string& label,
        const std::atomic<bool>* stop_flag)
    {
        if (is_stopped(stop_flag))
            throw std::runtime_error("Download interrupted.");

        const std::string file_name = destination.filename().string();

        std::cout << "\nDownloading " << label << "...\n";

        if (!destination.parent_path().empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(
                destination.parent_path(), ec);
        }

        std::ofstream file(
            destination,
            std::ios::binary | std::ios::trunc);

        if (!file)
        {
            print_progress_status(0, 0, "Failed");
            throw DownloadError(
                "Download failed: Could not write file '" + file_name + "'");
        }

        CURL* curl = curl_easy_init();

        if (!curl)
        {
            print_progress_status(0, 0, "Failed");
            throw DownloadError("Download failed: Network Error");
        }

        DownloadProgress progress{
            std::chrono::steady_clock::now(),
            -1,
            0,
            0,
            stop_flag
        };

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "TOTK-PC-Recomp-Builder/1.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        configure_ca_bundle(curl);

        const CURLcode result = curl_easy_perform(curl);

        curl_easy_cleanup(curl);
        file.close();

        if (is_stopped(stop_flag) || result == CURLE_ABORTED_BY_CALLBACK)
        {
            std::error_code ec;
            std::filesystem::remove(destination, ec);
            print_progress_status(progress.last_filled, progress.last_suffix_len, "Interrupted");
            throw std::runtime_error("Download interrupted.");
        }

        if (result != CURLE_OK || !file)
        {
            std::error_code ec;
            std::filesystem::remove(destination, ec);
            print_progress_status(progress.last_filled, progress.last_suffix_len, "Failed");

            if (result == CURLE_WRITE_ERROR || !file)
                throw DownloadError(
                    "Download failed: Could not write file '" + file_name + "'");

            throw DownloadError(
                "Download failed: Network Error");
        }

        print_progress_status(PROGRESS_WIDTH, progress.last_suffix_len, "Completed");
    }
}