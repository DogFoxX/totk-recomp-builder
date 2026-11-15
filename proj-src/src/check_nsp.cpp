#include "check_nsp.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr const char* TOTK_BASE_TITLE_ID = "0100f2c0115b6000";
    constexpr const char* TOTK_UPDATE_TITLE_ID = "0100f2c0115b6800";

    bool is_stopped(const std::atomic<bool>* stop_flag)
    {
        return stop_flag != nullptr &&
               stop_flag->load(std::memory_order_relaxed);
    }

    void throw_if_stopped(const std::atomic<bool>* stop_flag)
    {
        if (is_stopped(stop_flag))
        {
            throw std::runtime_error("NSP check interrupted.");
        }
    }

    class TempDirGuard
    {
    public:
        explicit TempDirGuard(fs::path path)
            : path_(std::move(path))
        {
            std::error_code ec;
            fs::remove_all(path_, ec);
            fs::create_directories(path_, ec);
        }

        ~TempDirGuard()
        {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }

        const fs::path& path() const noexcept
        {
            return path_;
        }

        TempDirGuard(const TempDirGuard&) = delete;
        TempDirGuard& operator=(const TempDirGuard&) = delete;

    private:
        fs::path path_;
    };

    std::string to_lower(std::string value)
    {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::string trim(const std::string& value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    bool ends_with(const std::string& str, const std::string& suffix)
    {
        return str.size() >= suffix.size() &&
               str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool is_hex_string(const std::string& str)
    {
        return !str.empty() &&
               std::all_of(
                   str.begin(),
                   str.end(),
                   [](unsigned char c) { return std::isxdigit(c) != 0; });
    }

    std::string bytes_to_hex(const uint8_t* data, std::size_t len)
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (std::size_t i = 0; i < len; ++i)
        {
            oss << std::setw(2) << static_cast<unsigned int>(data[i]);
        }
        return oss.str();
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

#if defined(_WIN32)
    std::wstring utf8_to_wide(const std::string& utf8)
    {
        if (utf8.empty())
            return L"";
        const int needed = MultiByteToWideChar(
            CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (needed <= 0)
            return std::wstring(utf8.begin(), utf8.end());
        std::wstring out(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
        return out;
    }
#endif

    fs::path normalize_input_folder(const fs::path& raw_folder)
    {
        std::string utf8 = trim(path_to_utf8(raw_folder));

        while (utf8.size() >= 2 &&
               ((utf8.front() == '"' && utf8.back() == '"') ||
                (utf8.front() == '\'' && utf8.back() == '\'')))
        {
            utf8 = trim(utf8.substr(1, utf8.size() - 2));
        }

        if (!utf8.empty() && utf8.back() == '"')
        {
            utf8.pop_back();
        }

#if defined(_WIN32)
        fs::path cleaned(utf8_to_wide(utf8));
#else
        fs::path cleaned(utf8);
#endif
        return cleaned.lexically_normal();
    }

    std::string get_folder_display_name(const fs::path& folder_path)
    {
        std::error_code ec;
        fs::path clean = fs::weakly_canonical(folder_path, ec);
        if (ec || clean.empty())
            clean = folder_path;

        while (!clean.empty() && clean.filename().empty() && clean != clean.parent_path())
            clean = clean.parent_path();

        const std::string name = path_to_utf8(clean.filename());
        return name.empty() ? path_to_utf8(clean) : name;
    }

    int get_accepted_update_rank(const std::string& version)
    {
        if (version == "1.4.3")
            return 3;
        if (version == "1.2.1")
            return 2;
        if (version == "1.1.0")
            return 1;
        return 0;
    }

    void create_sanitized_keys_file(
        const fs::path& source_keys,
        const fs::path& dest_keys)
    {
        std::ifstream in(source_keys);
        if (!in)
            throw std::runtime_error("Could not open prod.keys: " + path_to_utf8(source_keys));

        std::ofstream out(dest_keys, std::ios::binary);
        if (!out)
            throw std::runtime_error("Could not create temporary sanitized prod.keys.");

        std::string line;
        bool first_line = true;
        while (std::getline(in, line))
        {
            if (first_line)
            {
                first_line = false;
                if (line.size() >= 3 &&
                    static_cast<unsigned char>(line[0]) == 0xEF &&
                    static_cast<unsigned char>(line[1]) == 0xBB &&
                    static_cast<unsigned char>(line[2]) == 0xBF)
                {
                    line.erase(0, 3);
                }
            }

            const std::string cleaned = trim(line);
            if (cleaned.empty() || cleaned[0] == '#' || cleaned[0] == ';')
                continue;

            const std::size_t eq = cleaned.find('=');
            if (eq == std::string::npos)
                continue;

            const std::string key_name = trim(cleaned.substr(0, eq));
            const std::string hex_val = to_lower(trim(cleaned.substr(eq + 1)));

            if (!key_name.empty() &&
                is_hex_string(hex_val) &&
                (hex_val.size() == 32 || hex_val.size() == 64))
            {
                out << key_name << " = " << hex_val << '\n';
            }
        }
    }

    void remove_key_value_from_file(
        const fs::path& keys_file,
        const std::string& bad_hex_value)
    {
        const std::string target = to_lower(trim(bad_hex_value));
        if (target.empty())
            return;

        std::ifstream in(keys_file);
        if (!in)
            return;

        std::vector<std::string> kept_lines;
        std::string line;
        while (std::getline(in, line))
        {
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos)
            {
                const std::string val = to_lower(trim(line.substr(eq + 1)));
                if (val == target)
                    continue;
            }
            kept_lines.push_back(line);
        }
        in.close();

        std::ofstream out(keys_file, std::ios::binary | std::ios::trunc);
        for (const auto& kept : kept_lines)
        {
            out << kept << '\n';
        }
    }

    void copy_stream_slice(
        std::ifstream& in,
        uint64_t offset,
        uint64_t size,
        const fs::path& dest_file,
        const std::atomic<bool>* stop_flag)
    {
        throw_if_stopped(stop_flag);

        in.clear();
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in.good())
            throw std::runtime_error("Failed seeking inside package file.");

        std::ofstream out(dest_file, std::ios::binary);
        if (!out)
            throw std::runtime_error("Failed creating temp file: " + path_to_utf8(dest_file));

        std::vector<char> buffer(64 * 1024);
        uint64_t remaining = size;
        while (remaining > 0)
        {
            if (is_stopped(stop_flag))
            {
                out.close();
                std::error_code ec;
                fs::remove(dest_file, ec);
                throw std::runtime_error("NSP check interrupted.");
            }

            const std::size_t to_read = static_cast<std::size_t>(
                std::min<uint64_t>(remaining, buffer.size()));
            in.read(buffer.data(), static_cast<std::streamsize>(to_read));
            const std::streamsize bytes_read = in.gcount();
            if (bytes_read <= 0)
                throw std::runtime_error("Unexpected EOF while extracting metadata NCA.");

            out.write(buffer.data(), bytes_read);
            remaining -= static_cast<uint64_t>(bytes_read);
        }
    }

    struct ExtractedPackageFiles
    {
        std::vector<fs::path> ncas;
        std::unordered_map<std::string, std::string> titlekeys;
    };

    void parse_ticket_titlekey(
        const fs::path& tik_path,
        std::unordered_map<std::string, std::string>& titlekeys)
    {
        std::ifstream in(tik_path, std::ios::binary);
        if (!in)
            return;

        std::array<uint8_t, 0x2C0> buf{};
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        if (in.gcount() < 0x2B0)
            return;

        // Offset 0x281: titlekey_type (0 = Common, 1 = Personalized)
        if (buf[0x281] != 0)
            return;

        const std::string titlekey_hex = bytes_to_hex(&buf[0x180], 16);
        const std::string rights_id_hex = bytes_to_hex(&buf[0x2A0], 16);

        if (rights_id_hex != "00000000000000000000000000000000")
        {
            titlekeys[rights_id_hex] = titlekey_hex;
        }
    }

    ExtractedPackageFiles extract_pfs0_metadata(
        const fs::path& nsp_path,
        const fs::path& out_dir,
        const std::atomic<bool>* stop_flag)
    {
        throw_if_stopped(stop_flag);

        std::ifstream in(nsp_path, std::ios::binary);
        if (!in)
            throw std::runtime_error("Could not open NSP file: " + path_to_utf8(nsp_path));

        char magic[4]{};
        uint32_t file_count = 0;
        uint32_t string_table_size = 0;
        uint32_t reserved = 0;

        in.read(magic, 4);
        in.read(reinterpret_cast<char*>(&file_count), sizeof(file_count));
        in.read(reinterpret_cast<char*>(&string_table_size), sizeof(string_table_size));
        in.read(reinterpret_cast<char*>(&reserved), sizeof(reserved));

        if (!in.good() || std::memcmp(magic, "PFS0", 4) != 0)
            throw std::runtime_error("Invalid PFS0 header in NSP: " + path_to_utf8(nsp_path.filename()));

        if (file_count == 0 || file_count > 4096 || string_table_size > 1024 * 1024)
            throw std::runtime_error("Corrupted PFS0 table in NSP: " + path_to_utf8(nsp_path.filename()));

#pragma pack(push, 1)
        struct Pfs0Entry
        {
            uint64_t offset;
            uint64_t size;
            uint32_t string_offset;
            uint32_t reserved;
        };
#pragma pack(pop)

        std::vector<Pfs0Entry> entries(file_count);
        in.read(
            reinterpret_cast<char*>(entries.data()),
            static_cast<std::streamsize>(file_count * sizeof(Pfs0Entry)));

        std::vector<char> string_table(string_table_size);
        in.read(string_table.data(), static_cast<std::streamsize>(string_table_size));

        if (!in.good())
            throw std::runtime_error("Failed reading PFS0 entries in: " + path_to_utf8(nsp_path.filename()));

        const uint64_t data_base =
            16ULL +
            static_cast<uint64_t>(file_count) * sizeof(Pfs0Entry) +
            static_cast<uint64_t>(string_table_size);

        constexpr uint64_t kMaxMetadataNcaSize = 64ULL * 1024 * 1024;
        ExtractedPackageFiles result;

        for (const auto& entry : entries)
        {
            throw_if_stopped(stop_flag);

            if (entry.string_offset >= string_table_size)
                continue;

            const char* raw_name = &string_table[entry.string_offset];
            const std::size_t max_len = string_table_size - entry.string_offset;
            const std::size_t name_len = strnlen(raw_name, max_len);
            const std::string name(raw_name, name_len);
            const std::string lower_name = to_lower(name);

            const bool is_cnmt_nca = ends_with(lower_name, ".cnmt.nca");
            const bool is_small_nca =
                ends_with(lower_name, ".nca") &&
                entry.size > 0 &&
                entry.size <= kMaxMetadataNcaSize;
            const bool is_tik =
                ends_with(lower_name, ".tik") &&
                entry.size > 0 &&
                entry.size <= 64 * 1024;

            if (is_cnmt_nca || is_small_nca)
            {
                const fs::path dest = out_dir / fs::path(name).filename();
                copy_stream_slice(in, data_base + entry.offset, entry.size, dest, stop_flag);
                result.ncas.push_back(dest);
            }
            else if (is_tik)
            {
                const fs::path dest = out_dir / fs::path(name).filename();
                copy_stream_slice(in, data_base + entry.offset, entry.size, dest, stop_flag);
                parse_ticket_titlekey(dest, result.titlekeys);
            }
        }

        return result;
    }

    ExtractedPackageFiles extract_xci_metadata(
        const fs::path& xci_path,
        const fs::path& out_dir,
        const std::atomic<bool>* stop_flag)
    {
        throw_if_stopped(stop_flag);

        std::ifstream in(xci_path, std::ios::binary);
        if (!in)
            throw std::runtime_error("Could not open XCI file: " + path_to_utf8(xci_path));

        uint64_t cart_base = 0;
        char head_magic[4]{};

        in.seekg(0x100, std::ios::beg);
        in.read(head_magic, 4);
        if (in.good() && std::memcmp(head_magic, "HEAD", 4) == 0)
        {
            cart_base = 0x0;
        }
        else
        {
            in.clear();
            in.seekg(0x300, std::ios::beg);
            in.read(head_magic, 4);
            if (in.good() && std::memcmp(head_magic, "HEAD", 4) == 0)
            {
                cart_base = 0x200;
            }
            else
            {
                throw std::runtime_error("Invalid XCI header in: " + path_to_utf8(xci_path.filename()));
            }
        }

        in.seekg(static_cast<std::streamoff>(cart_base + 0x130), std::ios::beg);
        uint64_t raw_root_hfs0_offset = 0;
        in.read(reinterpret_cast<char*>(&raw_root_hfs0_offset), sizeof(raw_root_hfs0_offset));
        if (!in.good())
            throw std::runtime_error("Failed reading XCI HFS0 offset in: " + path_to_utf8(xci_path.filename()));

        const uint64_t root_hfs0_offset = cart_base + raw_root_hfs0_offset;

#pragma pack(push, 1)
        struct Hfs0Entry
        {
            uint64_t offset;
            uint64_t size;
            uint32_t string_offset;
            uint32_t hashed_region_size;
            uint64_t reserved;
            uint8_t hash[32];
        };
#pragma pack(pop)

        auto read_hfs0 = [&](uint64_t hfs0_offset,
                             std::vector<Hfs0Entry>& entries,
                             std::vector<char>& string_table,
                             uint64_t& data_base)
        {
            throw_if_stopped(stop_flag);

            in.clear();
            in.seekg(static_cast<std::streamoff>(hfs0_offset), std::ios::beg);

            char magic[4]{};
            uint32_t file_count = 0;
            uint32_t string_table_size = 0;
            uint32_t reserved = 0;

            in.read(magic, 4);
            in.read(reinterpret_cast<char*>(&file_count), sizeof(file_count));
            in.read(reinterpret_cast<char*>(&string_table_size), sizeof(string_table_size));
            in.read(reinterpret_cast<char*>(&reserved), sizeof(reserved));

            if (!in.good() || std::memcmp(magic, "HFS0", 4) != 0)
                throw std::runtime_error("Invalid HFS0 partition in XCI: " + path_to_utf8(xci_path.filename()));

            if (file_count == 0 || file_count > 4096 || string_table_size > 1024 * 1024)
                throw std::runtime_error("Corrupted HFS0 table in XCI: " + path_to_utf8(xci_path.filename()));

            entries.resize(file_count);
            in.read(
                reinterpret_cast<char*>(entries.data()),
                static_cast<std::streamsize>(file_count * sizeof(Hfs0Entry)));

            string_table.resize(string_table_size);
            in.read(string_table.data(), static_cast<std::streamsize>(string_table_size));

            if (!in.good())
                throw std::runtime_error("Failed reading HFS0 entries in XCI: " + path_to_utf8(xci_path.filename()));

            data_base =
                hfs0_offset +
                16ULL +
                static_cast<uint64_t>(file_count) * sizeof(Hfs0Entry) +
                static_cast<uint64_t>(string_table_size);
        };

        std::vector<Hfs0Entry> root_entries;
        std::vector<char> root_strings;
        uint64_t root_data_base = 0;
        read_hfs0(root_hfs0_offset, root_entries, root_strings, root_data_base);

        uint64_t secure_offset = 0;
        bool found_secure = false;

        for (const auto& entry : root_entries)
        {
            if (entry.string_offset >= root_strings.size())
                continue;

            const char* raw_name = &root_strings[entry.string_offset];
            const std::size_t max_len = root_strings.size() - entry.string_offset;
            const std::string name(raw_name, strnlen(raw_name, max_len));

            if (to_lower(name) == "secure")
            {
                secure_offset = root_data_base + entry.offset;
                found_secure = true;
                break;
            }
        }

        if (!found_secure)
            throw std::runtime_error("Missing secure partition in XCI: " + path_to_utf8(xci_path.filename()));

        std::vector<Hfs0Entry> secure_entries;
        std::vector<char> secure_strings;
        uint64_t secure_data_base = 0;
        read_hfs0(secure_offset, secure_entries, secure_strings, secure_data_base);

        constexpr uint64_t kMaxMetadataNcaSize = 64ULL * 1024 * 1024;
        ExtractedPackageFiles result;

        for (const auto& entry : secure_entries)
        {
            throw_if_stopped(stop_flag);

            if (entry.string_offset >= secure_strings.size())
                continue;

            const char* raw_name = &secure_strings[entry.string_offset];
            const std::size_t max_len = secure_strings.size() - entry.string_offset;
            const std::string name(raw_name, strnlen(raw_name, max_len));
            const std::string lower_name = to_lower(name);

            const bool is_cnmt_nca = ends_with(lower_name, ".cnmt.nca");
            const bool is_small_nca =
                ends_with(lower_name, ".nca") &&
                entry.size > 0 &&
                entry.size <= kMaxMetadataNcaSize;

            if (is_cnmt_nca || is_small_nca)
            {
                const fs::path dest = out_dir / fs::path(name).filename();
                copy_stream_slice(in, secure_data_base + entry.offset, entry.size, dest, stop_flag);
                result.ncas.push_back(dest);
            }
        }

        return result;
    }

    std::string run_hactool_once(
        const fs::path& hactool_bin,
        const fs::path& keys_path,
        const std::vector<fs::path>& path_flags,
        const std::vector<std::string>& string_flags,
        const fs::path& target_file,
        const std::atomic<bool>* stop_flag)
    {
        throw_if_stopped(stop_flag);

#if defined(_WIN32)
        auto quote_win_arg = [](const std::wstring& arg) -> std::wstring
        {
            std::wstring out = L"\"";
            for (auto it = arg.begin();; ++it)
            {
                unsigned int num_backslashes = 0;
                while (it != arg.end() && *it == L'\\')
                {
                    ++it;
                    ++num_backslashes;
                }

                if (it == arg.end())
                {
                    out.append(num_backslashes * 2, L'\\');
                    break;
                }
                else if (*it == L'"')
                {
                    out.append(num_backslashes * 2 + 1, L'\\');
                    out.push_back(L'"');
                }
                else
                {
                    out.append(num_backslashes, L'\\');
                    out.push_back(*it);
                }
            }
            out.push_back(L'"');
            return out;
        };

        std::wstring cmd_line = quote_win_arg(hactool_bin.native());
        cmd_line += L" -t nca -k " + quote_win_arg(keys_path.native());

        for (const auto& flag : string_flags)
        {
            const std::wstring wflag(flag.begin(), flag.end());
            cmd_line += L" " + quote_win_arg(wflag);
        }

        for (const auto& pflag : path_flags)
        {
            cmd_line += L" " + quote_win_arg(pflag.native());
        }

        cmd_line += L" " + quote_win_arg(target_file.native());

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;
        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
            throw std::runtime_error("Failed creating pipe for hactool.");

        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe;
        si.hStdError = write_pipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        const BOOL created = CreateProcessW(
            nullptr,
            cmd_line.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);

        CloseHandle(write_pipe);

        if (!created)
        {
            const DWORD err = GetLastError();
            CloseHandle(read_pipe);
            throw std::runtime_error(
                "Failed launching hactool.exe (Windows error " + std::to_string(err) + ").");
        }

        std::array<char, 512> buffer{};
        std::string result;

        while (true)
        {
            if (is_stopped(stop_flag))
            {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 500);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                CloseHandle(read_pipe);
                throw std::runtime_error("NSP check interrupted.");
            }

            DWORD available = 0;
            if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr))
                break;

            if (available > 0)
            {
                DWORD bytes_read = 0;
                if (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) &&
                    bytes_read > 0)
                {
                    result.append(buffer.data(), bytes_read);
                }
                else
                {
                    break;
                }
            }
            else
            {
                const DWORD wait_status = WaitForSingleObject(pi.hProcess, 50);
                if (wait_status == WAIT_OBJECT_0)
                {
                    DWORD remaining = 0;
                    while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &remaining, nullptr) && remaining > 0)
                    {
                        DWORD bytes_read = 0;
                        if (!ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) ||
                            bytes_read == 0)
                        {
                            break;
                        }
                        result.append(buffer.data(), bytes_read);
                    }
                    break;
                }
            }
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(read_pipe);

        throw_if_stopped(stop_flag);
        return result;
#else
        std::vector<std::string> arg_storage;
        arg_storage.push_back(hactool_bin.string());
        arg_storage.push_back("-t");
        arg_storage.push_back("nca");
        arg_storage.push_back("-k");
        arg_storage.push_back(keys_path.string());

        for (const auto& flag : string_flags)
            arg_storage.push_back(flag);

        for (const auto& pflag : path_flags)
            arg_storage.push_back(pflag.string());

        arg_storage.push_back(target_file.string());

        std::vector<char*> argv_exec;
        argv_exec.reserve(arg_storage.size() + 1);
        for (auto& s : arg_storage)
            argv_exec.push_back(s.data());
        argv_exec.push_back(nullptr);

        int pipe_fds[2]{-1, -1};
        if (pipe(pipe_fds) != 0)
            throw std::runtime_error("Failed creating pipe for hactool.");

        const pid_t pid = fork();
        if (pid < 0)
        {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            throw std::runtime_error("Failed forking process for hactool.");
        }

        if (pid == 0)
        {
            close(pipe_fds[0]);
            dup2(pipe_fds[1], STDOUT_FILENO);
            dup2(pipe_fds[1], STDERR_FILENO);
            close(pipe_fds[1]);

            execv(arg_storage[0].c_str(), argv_exec.data());
            _exit(127);
        }

        close(pipe_fds[1]);

        std::array<char, 512> buffer{};
        std::string result;
        bool pipe_open = true;

        while (pipe_open)
        {
            if (is_stopped(stop_flag))
            {
                kill(pid, SIGTERM);
                for (int i = 0; i < 10; ++i)
                {
                    int wstatus = 0;
                    if (waitpid(pid, &wstatus, WNOHANG) == pid)
                        break;
                    usleep(20000);
                }
                kill(pid, SIGKILL);
                int wstatus = 0;
                waitpid(pid, &wstatus, 0);
                close(pipe_fds[0]);
                throw std::runtime_error("NSP check interrupted.");
            }

            struct pollfd pfd{};
            pfd.fd = pipe_fds[0];
            pfd.events = POLLIN | POLLHUP | POLLERR;

            const int poll_res = poll(&pfd, 1, 50);
            if (poll_res > 0)
            {
                const ssize_t bytes_read = read(pipe_fds[0], buffer.data(), buffer.size());
                if (bytes_read > 0)
                {
                    result.append(buffer.data(), static_cast<std::size_t>(bytes_read));
                }
                else if (bytes_read == 0 || (bytes_read < 0 && errno != EINTR))
                {
                    pipe_open = false;
                }
            }
        }

        close(pipe_fds[0]);

        int wait_status = 0;
        while (waitpid(pid, &wait_status, 0) < 0)
        {
            if (errno != EINTR)
                break;
        }

        throw_if_stopped(stop_flag);
        return result;
#endif
    }

    std::string run_hactool(
        const fs::path& hactool_bin,
        const fs::path& sanitized_keys_path,
        const std::vector<fs::path>& path_flags,
        const std::vector<std::string>& string_flags,
        const fs::path& target_file,
        const std::atomic<bool>* stop_flag)
    {
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            throw_if_stopped(stop_flag);

            const std::string output = run_hactool_once(
                hactool_bin,
                sanitized_keys_path,
                path_flags,
                string_flags,
                target_file,
                stop_flag);

            const std::string marker = "Key (";
            const std::size_t key_pos = output.find(marker);
            if (key_pos != std::string::npos)
            {
                const std::size_t close_pos = output.find(") must be ", key_pos + marker.size());
                if (close_pos != std::string::npos)
                {
                    const std::string bad_val = output.substr(
                        key_pos + marker.size(),
                        close_pos - (key_pos + marker.size()));
                    remove_key_value_from_file(sanitized_keys_path, bad_val);
                    continue;
                }
            }

            return output;
        }

        return run_hactool_once(
            hactool_bin,
            sanitized_keys_path,
            path_flags,
            string_flags,
            target_file,
            stop_flag);
    }

    std::string extract_field_value(const std::string& text, const std::string& key)
    {
        const std::string lower_key = to_lower(key);
        std::size_t pos = 0;
        while (pos < text.size())
        {
            const std::size_t line_end = text.find('\n', pos);
            const std::string line = (line_end == std::string::npos)
                ? text.substr(pos)
                : text.substr(pos, line_end - pos);

            const std::size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                const std::string found_key = to_lower(trim(line.substr(0, colon + 1)));
                if (found_key == lower_key)
                {
                    return trim(line.substr(colon + 1));
                }
            }

            if (line_end == std::string::npos)
                break;
            pos = line_end + 1;
        }
        return "";
    }

    struct PackageMetadata
    {
        bool is_base = false;
        bool is_update = false;
        std::string base_title_id;
        std::string update_title_id;
        std::string display_version;
    };

    PackageMetadata inspect_package_with_hactool(
        const fs::path& package_path,
        const fs::path& sanitized_keys_path,
        const fs::path& hactool_bin,
        const fs::path& work_dir_path,
        const std::atomic<bool>* stop_flag)
    {
        throw_if_stopped(stop_flag);

        TempDirGuard work_dir(work_dir_path);

        const std::string ext = to_lower(path_to_utf8(package_path.extension()));
        const ExtractedPackageFiles pkg_files = (ext == ".xci")
            ? extract_xci_metadata(package_path, work_dir.path(), stop_flag)
            : extract_pfs0_metadata(package_path, work_dir.path(), stop_flag);

        if (pkg_files.ncas.empty())
        {
            throw std::runtime_error(
                "No metadata NCAs found inside: " + path_to_utf8(package_path.filename()));
        }

        PackageMetadata meta;

        for (std::size_t nca_idx = 0; nca_idx < pkg_files.ncas.size(); ++nca_idx)
        {
            throw_if_stopped(stop_flag);

            const auto& nca = pkg_files.ncas[nca_idx];
            const std::string info = run_hactool(
                hactool_bin,
                sanitized_keys_path,
                {},
                {},
                nca,
                stop_flag);

            const std::string content_type = to_lower(extract_field_value(info, "Content Type:"));
            const std::string rights_id = to_lower(extract_field_value(info, "Rights ID:"));

            std::vector<std::string> key_flags;
            if (!rights_id.empty())
            {
                const auto tk_it = pkg_files.titlekeys.find(rights_id);
                if (tk_it != pkg_files.titlekeys.end())
                {
                    key_flags.push_back("--titlekey=" + tk_it->second);
                }
            }

            if (content_type == "meta")
            {
                const std::string tid = to_lower(extract_field_value(info, "Title ID:"));
                bool found_base_in_nca = false;
                bool found_update_in_nca = false;

                const fs::path sec0_dir = work_dir.path() / ("sec0_" + std::to_string(nca_idx));
                fs::create_directories(sec0_dir);

                fs::path sec0_arg = "--section0dir=";
                sec0_arg += sec0_dir;

                run_hactool(
                    hactool_bin,
                    sanitized_keys_path,
                    {sec0_arg},
                    key_flags,
                    nca,
                    stop_flag);

                for (const auto& entry : fs::directory_iterator(sec0_dir))
                {
                    throw_if_stopped(stop_flag);

                    if (entry.is_regular_file() &&
                        to_lower(path_to_utf8(entry.path().extension())) == ".cnmt")
                    {
                        std::ifstream cnmt_in(entry.path(), std::ios::binary);
                        if (cnmt_in)
                        {
                            uint64_t cnmt_title_id = 0;
                            uint32_t cnmt_version = 0;
                            uint8_t title_type = 0;

                            cnmt_in.read(reinterpret_cast<char*>(&cnmt_title_id), sizeof(cnmt_title_id));
                            cnmt_in.read(reinterpret_cast<char*>(&cnmt_version), sizeof(cnmt_version));
                            cnmt_in.read(reinterpret_cast<char*>(&title_type), sizeof(title_type));

                            if (cnmt_in.good())
                            {
                                if (title_type == 0x80)
                                    found_base_in_nca = true;
                                else if (title_type == 0x81)
                                    found_update_in_nca = true;
                            }
                        }
                    }
                }

                // Fallback using Title ID suffix if .cnmt section could not be decrypted
                if (!found_base_in_nca && !found_update_in_nca && tid.size() == 16)
                {
                    if (ends_with(tid, "000"))
                        found_base_in_nca = true;
                    else if (ends_with(tid, "800"))
                        found_update_in_nca = true;
                }

                if (found_base_in_nca)
                {
                    meta.is_base = true;
                    if (!tid.empty())
                        meta.base_title_id = tid;
                }
                else if (found_update_in_nca)
                {
                    meta.is_update = true;
                    if (!tid.empty())
                        meta.update_title_id = tid;
                }
            }
            else if (content_type == "control")
            {
                const fs::path romfs_dir = work_dir.path() / ("romfs_" + std::to_string(nca_idx));
                fs::create_directories(romfs_dir);

                fs::path romfs_arg = "--romfsdir=";
                romfs_arg += romfs_dir;

                run_hactool(
                    hactool_bin,
                    sanitized_keys_path,
                    {romfs_arg},
                    key_flags,
                    nca,
                    stop_flag);

                const fs::path nacp_path = romfs_dir / "control.nacp";
                if (fs::exists(nacp_path))
                {
                    std::ifstream nacp_in(nacp_path, std::ios::binary);
                    if (nacp_in)
                    {
                        nacp_in.seekg(0x3060, std::ios::beg);
                        char version_buf[16]{};
                        nacp_in.read(version_buf, sizeof(version_buf));
                        if (nacp_in.good())
                        {
                            const std::size_t len = strnlen(version_buf, sizeof(version_buf));
                            meta.display_version = trim(std::string(version_buf, len));
                        }
                    }
                }
            }
        }

        return meta;
    }
}

namespace check_nsp
{
    GameFiles validate_folder(
        const fs::path& folder_path,
        const fs::path& setup_dir,
        const std::atomic<bool>* stop_flag)
    {
        throw_if_stopped(stop_flag);

        const fs::path clean_folder = normalize_input_folder(folder_path);

        if (!fs::exists(clean_folder) || !fs::is_directory(clean_folder))
        {
            throw std::runtime_error(
                "Provided path is not a valid directory: " + path_to_utf8(clean_folder));
        }

        const std::string folder_name = get_folder_display_name(clean_folder);

        std::cout << "\nChecking game files...\n";

        GameFiles result;
        std::vector<fs::path> candidate_packages;

        for (const auto& entry : fs::directory_iterator(clean_folder))
        {
            throw_if_stopped(stop_flag);

            const std::string raw_filename = path_to_utf8(entry.path().filename());
            if (raw_filename.starts_with("._"))
                continue;

            if (entry.is_regular_file())
            {
                const std::string filename_lower = to_lower(raw_filename);
                const std::string ext_lower = to_lower(path_to_utf8(entry.path().extension()));

                if (filename_lower == "prod.keys")
                {
                    result.keys = entry.path();
                }
                else if (ext_lower == ".nsp" || ext_lower == ".xci")
                {
                    candidate_packages.push_back(entry.path());
                }
            }
            else if (entry.is_directory())
            {
                if (to_lower(raw_filename) == "firmware")
                {
                    result.firmware = entry.path();
                }
            }
        }

        if (candidate_packages.empty())
        {
            throw std::runtime_error(
                "No valid files found. Ensure a Base NSP/XCI and Update NSP exist in '" +
                folder_name + "'");
        }

        if (result.keys.empty())
        {
            throw std::runtime_error(
                "prod.keys file could not be found. Ensure the file exists in '" +
                folder_name + "'");
        }

#if defined(_WIN32)
        const fs::path hactool_bin = setup_dir / "bin" / "hactool.exe";
#else
        const fs::path hactool_bin = setup_dir / "bin" / "hactool";
#endif
        if (!fs::exists(hactool_bin))
        {
            throw std::runtime_error(
                path_to_utf8(hactool_bin.filename()) +
                " not found at: " +
                path_to_utf8(hactool_bin));
        }

#if !defined(_WIN32)
        {
            std::error_code ec;
            fs::permissions(
                hactool_bin,
                fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                fs::perm_options::add,
                ec);
        }
#endif

        std::sort(candidate_packages.begin(), candidate_packages.end());

        TempDirGuard temp_root(setup_dir / "_tmp_inspect");
        const fs::path sanitized_keys_path = temp_root.path() / "prod.keys";
        create_sanitized_keys_file(result.keys, sanitized_keys_path);

        bool saw_any_base = false;
        bool saw_any_update = false;
        bool saw_matching_title_update = false;
        std::string invalid_matching_update_version;
        std::string selected_update_version;
        int selected_update_rank = 0;

        for (std::size_t i = 0; i < candidate_packages.size(); ++i)
        {
            throw_if_stopped(stop_flag);

            const fs::path& pkg = candidate_packages[i];
            const std::string ext_lower = to_lower(path_to_utf8(pkg.extension()));

            try
            {
                const PackageMetadata meta = inspect_package_with_hactool(
                    pkg,
                    sanitized_keys_path,
                    hactool_bin,
                    temp_root.path() / ("pkg_" + std::to_string(i)),
                    stop_flag);

                if (meta.is_base)
                {
                    saw_any_base = true;
                    if (result.base.empty() && meta.base_title_id == TOTK_BASE_TITLE_ID)
                    {
                        result.base = pkg;
                    }
                }
                else if (meta.is_update && ext_lower == ".nsp")
                {
                    saw_any_update = true;
                    if (meta.update_title_id == TOTK_UPDATE_TITLE_ID)
                    {
                        saw_matching_title_update = true;
                        const int rank = get_accepted_update_rank(meta.display_version);
                        if (rank > selected_update_rank)
                        {
                            selected_update_rank = rank;
                            selected_update_version = meta.display_version;
                            result.update = pkg;
                        }
                        else if (rank == 0 && invalid_matching_update_version.empty())
                        {
                            invalid_matching_update_version = meta.display_version;
                        }
                    }
                }
            }
            catch (const std::exception&)
            {
                if (is_stopped(stop_flag))
                    throw;
                // Unrecognized or corrupted package; missing/mismatch status is handled below.
            }
        }

        if (result.base.empty())
        {
            if (!saw_any_base)
            {
                throw std::runtime_error(
                    "No Base NSP/XCI found in '" + folder_name + "'");
            }

            throw std::runtime_error(
                "No matching Base NSP/XCI found in '" + folder_name + "'");
        }

        std::cout
            << "Valid Base NSP found: "
            << path_to_utf8(result.base.filename())
            << '\n';

        if (result.update.empty())
        {
            if (!saw_any_update)
            {
                throw std::runtime_error(
                    "No Update NSP file found in '" + folder_name + "'");
            }

            if (saw_matching_title_update)
            {
                const std::string version_str = invalid_matching_update_version.empty()
                    ? "unknown"
                    : invalid_matching_update_version;

                throw std::runtime_error(
                    "Invalid Update NSP version '" + version_str +
                    "'. Accepted versions: 1.1.0, 1.2.1 and 1.4.3");
            }

            throw std::runtime_error(
                "No matching Update NSP file found in '" + folder_name + "'");
        }

        std::cout
            << "Valid Update NSP found: "
            << path_to_utf8(result.update.filename())
            << " | Version: "
            << selected_update_version
            << '\n';

        return result;
    }
}