#include "extraction.h"

#include <archive.h>
#include <archive_entry.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr int PROGRESS_WIDTH = 50;

    bool is_stopped(const std::atomic<bool>* stop_flag) noexcept
    {
        return stop_flag != nullptr &&
               stop_flag->load(std::memory_order_relaxed);
    }

    [[noreturn]] void throw_extraction_error(const std::string& message)
    {
        std::cerr << '\n' << message << '\n';
        throw extraction::ExtractionError(message);
    }

    std::string to_lower_ascii(std::string_view input)
    {
        std::string out(input);
        std::transform(
            out.begin(),
            out.end(),
            out.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        return out;
    }

    std::string path_to_utf8(const fs::path& p)
    {
#if defined(_WIN32)
        const std::wstring& ws = p.native();
        if (ws.empty())
            return "";

        const int needed = WideCharToMultiByte(
            CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return p.string();

        std::string out(static_cast<std::size_t>(needed), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), needed, nullptr, nullptr);
        return out;
#else
        return p.string();
#endif
    }

    void print_progress_bar(int filled, std::string_view status)
    {
        const int clamped = std::clamp(filled, 0, PROGRESS_WIDTH);
        std::cout << "\r["
                  << std::string(clamped, '=')
                  << std::string(PROGRESS_WIDTH - clamped, ' ')
                  << "][" << status << "]"
                  << std::flush;
    }

    void finish_progress_bar(int filled, std::string_view status)
    {
        print_progress_bar(filled, status);
        std::cout << '\n' << std::flush;
    }

    std::uint16_t read_le_u16(const unsigned char* p) noexcept
    {
        return static_cast<std::uint16_t>(p[0]) |
               (static_cast<std::uint16_t>(p[1]) << 8);
    }

    std::uint32_t read_le_u32(const unsigned char* p) noexcept
    {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    }

    std::uint64_t read_le_u64(const unsigned char* p) noexcept
    {
        return static_cast<std::uint64_t>(read_le_u32(p)) |
               (static_cast<std::uint64_t>(read_le_u32(p + 4)) << 32);
    }

    bool is_valid_squashfs_superblock(
        const unsigned char* sb,
        std::uint64_t offset,
        std::uint64_t file_size) noexcept
    {
        // SquashFS 4.0 superblock is 96 bytes starting with "hsqs" (0x73717368).
        if (read_le_u32(sb + 0) != 0x73717368U)
            return false;

        const std::uint32_t inode_count = read_le_u32(sb + 4);
        const std::uint32_t block_size = read_le_u32(sb + 12);
        const std::uint16_t compression = read_le_u16(sb + 20);
        const std::uint16_t s_major = read_le_u16(sb + 28);
        const std::uint16_t s_minor = read_le_u16(sb + 30);
        const std::uint64_t bytes_used = read_le_u64(sb + 40);

        if (inode_count == 0)
            return false;

        if (s_major != 4 || s_minor != 0)
            return false;

        if (block_size < 4096U || block_size > 1048576U ||
            (block_size & (block_size - 1U)) != 0U)
            return false;

        if (compression < 1U || compression > 6U)
            return false;

        if (bytes_used <= 96ULL || offset + bytes_used > file_size)
            return false;

        return true;
    }

    std::uint64_t find_squashfs_offset(
        std::ifstream& file,
        std::uint64_t file_size)
    {
        if (file_size < 160ULL)
            throw_extraction_error("Extraction failed: AppImage file is too small.");

        file.clear();
        file.seekg(0, std::ios::beg);

        std::array<unsigned char, 64> elf_hdr{};
        file.read(reinterpret_cast<char*>(elf_hdr.data()), elf_hdr.size());
        if (file.gcount() != 64)
            throw_extraction_error("Extraction failed: Could not read AppImage header.");

        std::uint64_t scan_start = 0;

        // Parse ELF64 section header table end to skip past the AppImage runtime binary.
        if (elf_hdr[0] == 0x7F && elf_hdr[1] == 'E' &&
            elf_hdr[2] == 'L' && elf_hdr[3] == 'F' &&
            elf_hdr[4] == 2 && elf_hdr[5] == 1)
        {
            const std::uint64_t e_shoff = read_le_u64(elf_hdr.data() + 40);
            const std::uint16_t e_shentsize = read_le_u16(elf_hdr.data() + 58);
            const std::uint16_t e_shnum = read_le_u16(elf_hdr.data() + 60);

            if (e_shoff > 0 && e_shentsize >= 64 && e_shnum > 0)
            {
                const std::uint64_t elf_end =
                    e_shoff + static_cast<std::uint64_t>(e_shentsize) * e_shnum;
                if (elf_end < file_size)
                {
                    scan_start = elf_end;
                }
            }
        }

        // Fast check directly at elf_end first.
        if (scan_start > 0 && scan_start + 96ULL <= file_size)
        {
            std::array<unsigned char, 96> sb{};
            file.clear();
            file.seekg(static_cast<std::streamoff>(scan_start), std::ios::beg);
            file.read(reinterpret_cast<char*>(sb.data()), sb.size());
            if (file.gcount() == 96 &&
                is_valid_squashfs_superblock(sb.data(), scan_start, file_size))
            {
                return scan_start;
            }
        }

        // Fallback scan up to 32 MiB from scan_start for padded/stripped AppImage runtimes.
        const std::uint64_t max_scan =
            std::min<std::uint64_t>(file_size, scan_start + 32ULL * 1024ULL * 1024ULL);

        constexpr std::size_t CHUNK_SIZE = 65536;
        constexpr std::size_t OVERLAP = 96;
        std::vector<unsigned char> buf(CHUNK_SIZE + OVERLAP);

        std::uint64_t pos = scan_start;
        while (pos + 96ULL <= max_scan)
        {
            const std::size_t to_read = static_cast<std::size_t>(
                std::min<std::uint64_t>(buf.size(), max_scan - pos));

            file.clear();
            file.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
            file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(to_read));

            const std::size_t got = static_cast<std::size_t>(file.gcount());
            if (got < 96)
                break;

            const std::size_t limit = got - 96;
            for (std::size_t i = 0; i <= limit; ++i)
            {
                if (buf[i] == 'h' && buf[i + 1] == 's' &&
                    buf[i + 2] == 'q' && buf[i + 3] == 's')
                {
                    const std::uint64_t candidate = pos + i;
                    if (is_valid_squashfs_superblock(buf.data() + i, candidate, file_size))
                    {
                        return candidate;
                    }
                }
            }

            if (got <= OVERLAP)
                break;

            pos += (got - OVERLAP);
        }

        throw_extraction_error(
            "Extraction failed: Could not locate SquashFS payload inside AppImage.");
    }

    struct StreamReader
    {
        std::ifstream file;
        std::uint64_t base_offset = 0;
        std::uint64_t stream_size = 0;
        std::uint64_t logical_pos = 0;
        std::array<char, 65536> buffer{};
    };

    la_ssize_t stream_read_cb(
        struct archive*,
        void* client_data,
        const void** out_buffer)
    {
        auto* reader = static_cast<StreamReader*>(client_data);
        if (reader->logical_pos >= reader->stream_size)
            return 0;

        const std::uint64_t remaining = reader->stream_size - reader->logical_pos;
        const std::size_t to_read = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, reader->buffer.size()));

        reader->file.read(
            reader->buffer.data(),
            static_cast<std::streamsize>(to_read));

        const std::streamsize got = reader->file.gcount();
        if (got <= 0)
        {
            return reader->file.eof() ? 0 : ARCHIVE_FATAL;
        }

        reader->logical_pos += static_cast<std::uint64_t>(got);
        *out_buffer = reader->buffer.data();
        return static_cast<la_ssize_t>(got);
    }

    la_int64_t stream_skip_cb(
        struct archive*,
        void* client_data,
        la_int64_t request)
    {
        auto* reader = static_cast<StreamReader*>(client_data);
        if (request <= 0)
            return 0;

        const std::uint64_t remaining = reader->stream_size - reader->logical_pos;
        const std::uint64_t skip = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(request),
            remaining);

        reader->file.clear();
        reader->file.seekg(
            static_cast<std::streamoff>(reader->base_offset + reader->logical_pos + skip),
            std::ios::beg);

        if (!reader->file)
            return 0;

        reader->logical_pos += skip;
        return static_cast<la_int64_t>(skip);
    }

    la_int64_t stream_seek_cb(
        struct archive*,
        void* client_data,
        la_int64_t offset,
        int whence)
    {
        auto* reader = static_cast<StreamReader*>(client_data);
        la_int64_t target = 0;

        switch (whence)
        {
            case SEEK_SET:
                target = offset;
                break;
            case SEEK_CUR:
                target = static_cast<la_int64_t>(reader->logical_pos) + offset;
                break;
            case SEEK_END:
                target = static_cast<la_int64_t>(reader->stream_size) + offset;
                break;
            default:
                return ARCHIVE_FATAL;
        }

        if (target < 0 || static_cast<std::uint64_t>(target) > reader->stream_size)
            return ARCHIVE_FATAL;

        reader->file.clear();
        reader->file.seekg(
            static_cast<std::streamoff>(reader->base_offset + static_cast<std::uint64_t>(target)),
            std::ios::beg);

        if (!reader->file)
            return ARCHIVE_FATAL;

        reader->logical_pos = static_cast<std::uint64_t>(target);
        return static_cast<la_int64_t>(reader->logical_pos);
    }

    int stream_close_cb(struct archive*, void* client_data)
    {
        auto* reader = static_cast<StreamReader*>(client_data);
        if (reader->file.is_open())
            reader->file.close();
        return ARCHIVE_OK;
    }

    bool is_path_within_base(
        const fs::path& base_norm,
        const fs::path& target_norm)
    {
        auto base_it = base_norm.begin();
        auto target_it = target_norm.begin();

        for (; base_it != base_norm.end(); ++base_it, ++target_it)
        {
            if (target_it == target_norm.end() || *base_it != *target_it)
                return false;
        }

        return true;
    }

    fs::path extract_entry_relative_path(struct archive_entry* entry)
    {
#if defined(_WIN32)
        const wchar_t* wpath = archive_entry_pathname_w(entry);
        if (wpath != nullptr && wpath[0] != L'\0')
            return fs::path(wpath);
#endif
        const char* utf8_path = archive_entry_pathname_utf8(entry);
        if (utf8_path != nullptr && utf8_path[0] != '\0')
            return fs::path(utf8_path);

        const char* raw_path = archive_entry_pathname(entry);
        if (raw_path != nullptr && raw_path[0] != '\0')
            return fs::path(raw_path);

        return {};
    }

    fs::path resolve_safe_entry_path(
        const fs::path& staging_norm,
        const fs::path& entry_rel)
    {
        if (entry_rel.empty())
            return {};

        if (entry_rel.is_absolute() || entry_rel.has_root_name() || entry_rel.has_root_directory())
        {
            throw_extraction_error(
                "Extraction failed: Archive contains unsafe absolute path '" +
                path_to_utf8(entry_rel) + "'.");
        }

        const std::string raw_utf8 = path_to_utf8(entry_rel);
        if (!raw_utf8.empty() &&
            (raw_utf8.front() == '/' || raw_utf8.front() == '\\' ||
             raw_utf8.find(':') != std::string::npos))
        {
            throw_extraction_error(
                "Extraction failed: Archive contains unsafe path '" + raw_utf8 + "'.");
        }

        for (const auto& part : entry_rel)
        {
            if (part == "..")
            {
                throw_extraction_error(
                    "Extraction failed: Archive contains path traversal ('..') in '" +
                    raw_utf8 + "'.");
            }
        }

        const fs::path combined = (staging_norm / entry_rel).lexically_normal();
        if (combined == staging_norm)
            return {};

        if (!is_path_within_base(staging_norm, combined))
        {
            throw_extraction_error(
                "Extraction failed: Entry escapes destination directory ('" +
                raw_utf8 + "').");
        }

        return combined;
    }

    class StagingDirGuard
    {
    public:
        explicit StagingDirGuard(fs::path dir)
            : dir_(std::move(dir))
        {
        }

        ~StagingDirGuard()
        {
            if (!released_ && !dir_.empty())
            {
                std::error_code ec;
                fs::remove_all(dir_, ec);
            }
        }

        void release() noexcept
        {
            released_ = true;
        }

        const fs::path& path() const noexcept
        {
            return dir_;
        }

        StagingDirGuard(const StagingDirGuard&) = delete;
        StagingDirGuard& operator=(const StagingDirGuard&) = delete;

    private:
        fs::path dir_;
        bool released_ = false;
    };

    void ensure_linux_executables(const fs::path& destination)
    {
#if !defined(_WIN32)
        std::error_code ec;

        const fs::path hactool = destination / "hactool";
        if (fs::is_regular_file(hactool, ec))
        {
            fs::permissions(
                hactool,
                fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                fs::perm_options::add,
                ec);
        }

        const fs::path python_bin_dir = destination / "python" / "bin";
        if (fs::is_directory(python_bin_dir, ec))
        {
            for (const auto& entry : fs::directory_iterator(python_bin_dir, ec))
            {
                if (entry.is_regular_file(ec))
                {
                    fs::permissions(
                        entry.path(),
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add,
                        ec);
                }
            }
        }
#else
        (void)destination;
#endif
    }

    void promote_staging_contents(
        const fs::path& staging_dir,
        const fs::path& destination)
    {
        std::error_code ec;
        std::vector<fs::directory_entry> top_entries;
        for (const auto& entry : fs::directory_iterator(staging_dir, ec))
        {
            top_entries.push_back(entry);
        }

        if (ec)
        {
            throw_extraction_error(
                "Extraction failed: Could not read staged extraction directory.");
        }

        // Unwrap single top-level wrapper folder (e.g. squashfs-root or release folder).
        fs::path source_root = staging_dir;
        if (top_entries.size() == 1 && top_entries.front().is_directory(ec))
        {
            source_root = top_entries.front().path();
        }

        for (const auto& entry : fs::directory_iterator(source_root, ec))
        {
            const fs::path target = destination / entry.path().filename();
            if (fs::exists(target, ec) || fs::is_symlink(target, ec))
            {
                fs::remove_all(target, ec);
                if (ec)
                {
                    throw_extraction_error(
                        "Extraction failed: Could not replace existing path '" +
                        path_to_utf8(target) + "'.");
                }
            }

            fs::rename(entry.path(), target, ec);
            if (ec)
            {
                throw_extraction_error(
                    "Extraction failed: Could not move extracted item '" +
                    path_to_utf8(entry.path().filename()) + "' into destination.");
            }
        }
    }
}

namespace extraction
{
    void extract_archive(
        const fs::path& archive_path,
        const fs::path& destination,
        const std::string& label,
        const std::atomic<bool>* stop_flag)
    {
        if (is_stopped(stop_flag))
            throw std::runtime_error("Extraction interrupted.");

        std::error_code ec;
        if (!fs::exists(archive_path, ec) || !fs::is_regular_file(archive_path, ec))
        {
            throw_extraction_error(
                "Extraction failed: Archive file not found '" +
                path_to_utf8(archive_path) + "'.");
        }

        const std::uint64_t total_file_size = fs::file_size(archive_path, ec);
        if (ec || total_file_size == 0)
        {
            throw_extraction_error(
                "Extraction failed: Archive file is empty or unreadable.");
        }

        fs::create_directories(destination, ec);
        if (ec)
        {
            throw_extraction_error(
                "Extraction failed: Could not create destination directory '" +
                path_to_utf8(destination) + "'.");
        }

        const fs::path staging_dir = (destination / ".extract_staging").lexically_normal();
        fs::remove_all(staging_dir, ec);
        fs::create_directories(staging_dir, ec);
        if (ec)
        {
            throw_extraction_error(
                "Extraction failed: Could not create temporary staging directory.");
        }

        StagingDirGuard staging_guard(staging_dir);

        StreamReader reader;
        reader.file.open(archive_path, std::ios::binary);
        if (!reader.file)
        {
            throw_extraction_error(
                "Extraction failed: Could not open '" + path_to_utf8(archive_path) + "'.");
        }

        const std::string lower_name =
            to_lower_ascii(path_to_utf8(archive_path.filename()));

        if (lower_name.ends_with(".appimage"))
        {
            reader.base_offset = find_squashfs_offset(reader.file, total_file_size);
        }
        else
        {
            reader.base_offset = 0;
        }

        reader.stream_size = total_file_size - reader.base_offset;
        reader.logical_pos = 0;
        reader.file.clear();
        reader.file.seekg(static_cast<std::streamoff>(reader.base_offset), std::ios::beg);

        using ArchivePtr = std::unique_ptr<struct archive, decltype(&archive_read_free)>;
        ArchivePtr arc(archive_read_new(), &archive_read_free);
        if (!arc)
        {
            throw_extraction_error("Extraction failed: Could not initialize libarchive.");
        }

        archive_read_support_filter_all(arc.get());
        archive_read_support_format_all(arc.get());

        archive_read_set_read_callback(arc.get(), &stream_read_cb);
        archive_read_set_skip_callback(arc.get(), &stream_skip_cb);
        archive_read_set_seek_callback(arc.get(), &stream_seek_cb);
        archive_read_set_close_callback(arc.get(), &stream_close_cb);
        archive_read_set_callback_data(arc.get(), &reader);

        if (archive_read_open1(arc.get()) != ARCHIVE_OK)
        {
            const char* err = archive_error_string(arc.get());
            throw_extraction_error(
                std::string("Extraction failed: ") + (err != nullptr ? err : "Invalid archive."));
        }

        std::cout << "\nExtracting " << label << "...\n";

        int last_filled = 0;
        print_progress_bar(0, "0%");

        struct archive_entry* entry = nullptr;
        while (true)
        {
            if (is_stopped(stop_flag))
            {
                finish_progress_bar(last_filled, "Interrupted");
                throw std::runtime_error("Extraction interrupted.");
            }

            const int hdr_rc = archive_read_next_header(arc.get(), &entry);
            if (hdr_rc == ARCHIVE_EOF)
                break;

            if (hdr_rc != ARCHIVE_OK && hdr_rc != ARCHIVE_WARN)
            {
                finish_progress_bar(last_filled, "Failed");
                const char* err = archive_error_string(arc.get());
                throw_extraction_error(
                    std::string("Extraction failed: ") +
                    (err != nullptr ? err : "Corrupt archive header."));
            }

            const fs::path rel_path = extract_entry_relative_path(entry);
            const fs::path target_path = resolve_safe_entry_path(staging_dir, rel_path);
            if (target_path.empty())
            {
                archive_read_data_skip(arc.get());
                continue;
            }

            const auto file_type = archive_entry_filetype(entry);

            if (file_type == AE_IFDIR)
            {
                fs::create_directories(target_path, ec);
                if (ec)
                {
                    finish_progress_bar(last_filled, "Failed");
                    throw_extraction_error(
                        "Extraction failed: Could not create directory '" +
                        path_to_utf8(target_path) + "'.");
                }
            }
            else if (file_type == AE_IFLNK)
            {
#if !defined(_WIN32)
                const char* link_target_raw = archive_entry_symlink_utf8(entry);
                if (link_target_raw == nullptr)
                    link_target_raw = archive_entry_symlink(entry);

                if (link_target_raw != nullptr && link_target_raw[0] != '\0')
                {
                    const fs::path link_target(link_target_raw);
                    if (link_target.is_absolute())
                    {
                        finish_progress_bar(last_filled, "Failed");
                        throw_extraction_error(
                            "Extraction failed: Archive contains unsafe absolute symlink.");
                    }

                    const fs::path resolved_link =
                        (target_path.parent_path() / link_target).lexically_normal();
                    if (!is_path_within_base(staging_dir, resolved_link))
                    {
                        finish_progress_bar(last_filled, "Failed");
                        throw_extraction_error(
                            "Extraction failed: Archive symlink escapes destination directory.");
                    }

                    fs::create_directories(target_path.parent_path(), ec);
                    fs::remove(target_path, ec);
                    fs::create_symlink(link_target, target_path, ec);
                    if (ec)
                    {
                        finish_progress_bar(last_filled, "Failed");
                        throw_extraction_error(
                            "Extraction failed: Could not create symlink '" +
                            path_to_utf8(target_path) + "'.");
                    }
                }
#endif
            }
            else if (file_type == AE_IFREG)
            {
                fs::create_directories(target_path.parent_path(), ec);
                if (ec)
                {
                    finish_progress_bar(last_filled, "Failed");
                    throw_extraction_error(
                        "Extraction failed: Could not create parent directory for '" +
                        path_to_utf8(target_path) + "'.");
                }

                std::ofstream out_file(target_path, std::ios::binary | std::ios::trunc);
                if (!out_file)
                {
                    finish_progress_bar(last_filled, "Failed");
                    throw_extraction_error(
                        "Extraction failed: Could not write file '" +
                        path_to_utf8(target_path.filename()) + "'.");
                }

                const void* data_block = nullptr;
                std::size_t block_size = 0;
                la_int64_t block_offset = 0;

                while (true)
                {
                    if (is_stopped(stop_flag))
                    {
                        out_file.close();
                        finish_progress_bar(last_filled, "Interrupted");
                        throw std::runtime_error("Extraction interrupted.");
                    }

                    const int data_rc = archive_read_data_block(
                        arc.get(),
                        &data_block,
                        &block_size,
                        &block_offset);

                    if (data_rc == ARCHIVE_EOF)
                        break;

                    if (data_rc != ARCHIVE_OK && data_rc != ARCHIVE_WARN)
                    {
                        out_file.close();
                        finish_progress_bar(last_filled, "Failed");
                        const char* err = archive_error_string(arc.get());
                        throw_extraction_error(
                            std::string("Extraction failed: ") +
                            (err != nullptr ? err : "Error reading archive data."));
                    }

                    if (block_size > 0 && data_block != nullptr)
                    {
                        out_file.write(
                            static_cast<const char*>(data_block),
                            static_cast<std::streamsize>(block_size));

                        if (!out_file)
                        {
                            out_file.close();
                            finish_progress_bar(last_filled, "Failed");
                            throw_extraction_error(
                                "Extraction failed: Disk write error on '" +
                                path_to_utf8(target_path.filename()) + "'.");
                        }
                    }

                    if (reader.stream_size > 0)
                    {
                        const double frac = std::clamp(
                            static_cast<double>(reader.logical_pos) /
                                static_cast<double>(reader.stream_size),
                            0.0,
                            1.0);
                        const int filled = static_cast<int>(frac * PROGRESS_WIDTH);
                        if (filled != last_filled)
                        {
                            last_filled = filled;
                            const int pct = static_cast<int>(frac * 100.0);
                            print_progress_bar(filled, std::to_string(pct) + "%");
                        }
                    }
                }

                out_file.close();

#if !defined(_WIN32)
                const mode_t perm = archive_entry_perm(entry) & 0777;
                if (perm != 0)
                {
                    chmod(target_path.c_str(), perm);
                }
#endif
            }
            else
            {
                archive_read_data_skip(arc.get());
            }
        }

        arc.reset();

        if (is_stopped(stop_flag))
        {
            finish_progress_bar(last_filled, "Interrupted");
            throw std::runtime_error("Extraction interrupted.");
        }

        promote_staging_contents(staging_dir, destination);
        ensure_linux_executables(destination);
        staging_guard.release();
        fs::remove_all(staging_dir, ec);

        finish_progress_bar(PROGRESS_WIDTH, "Completed");
    }
}