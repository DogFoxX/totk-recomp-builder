#include "embedded_script.h"
#include "auto_build_data.h"

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
#include <cstdlib>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
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

    std::wstring quote_win_arg(const std::wstring& arg)
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
    }
#endif

    void validate_python_executable(const fs::path& python_bin)
    {
        std::error_code ec;
        if (!fs::exists(python_bin, ec))
        {
            throw std::runtime_error(
                "Bundled Python interpreter not found at: " + path_to_utf8(python_bin));
        }

#if !defined(_WIN32)
        fs::permissions(
            python_bin,
            fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
            fs::perm_options::add,
            ec);
#endif
    }

    int spawn_process(
        const fs::path& executable,
        const std::vector<std::string>& args,
        const fs::path& working_directory)
    {
        validate_python_executable(executable);

#if defined(_WIN32)
        std::wstring cmd_line = quote_win_arg(executable.native());
        for (const auto& arg : args)
        {
            cmd_line += L" " + quote_win_arg(utf8_to_wide(arg));
        }

        // Prepend working_directory (_setup) to PYTHONPATH for the child process.
        std::wstring prev_pythonpath;
        bool had_pythonpath = false;
        const DWORD existing_len = GetEnvironmentVariableW(L"PYTHONPATH", nullptr, 0);
        if (existing_len > 0)
        {
            prev_pythonpath.resize(existing_len);
            const DWORD copied = GetEnvironmentVariableW(
                L"PYTHONPATH", prev_pythonpath.data(), existing_len);
            prev_pythonpath.resize(copied);
            had_pythonpath = true;
        }

        if (!working_directory.empty())
        {
            std::wstring new_pythonpath = working_directory.native();
            if (had_pythonpath && !prev_pythonpath.empty())
            {
                new_pythonpath += L";" + prev_pythonpath;
            }
            SetEnvironmentVariableW(L"PYTHONPATH", new_pythonpath.c_str());
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);

        PROCESS_INFORMATION pi{};
        const BOOL created = CreateProcessW(
            nullptr,
            cmd_line.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            working_directory.empty() ? nullptr : working_directory.c_str(),
            &si,
            &pi);

        if (!working_directory.empty())
        {
            SetEnvironmentVariableW(
                L"PYTHONPATH",
                had_pythonpath ? prev_pythonpath.c_str() : nullptr);
        }

        if (!created)
        {
            const DWORD err = GetLastError();
            throw std::runtime_error(
                "Failed to start Python process (Windows error " +
                std::to_string(err) + ").");
        }

        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return static_cast<int>(exit_code);
#else
        std::vector<std::string> arg_storage;
        arg_storage.reserve(args.size() + 1);
        arg_storage.push_back(executable.string());
        for (const auto& arg : args)
        {
            arg_storage.push_back(arg);
        }

        std::vector<char*> argv_exec;
        argv_exec.reserve(arg_storage.size() + 1);
        for (auto& s : arg_storage)
        {
            argv_exec.push_back(s.data());
        }
        argv_exec.push_back(nullptr);

        const pid_t pid = fork();
        if (pid < 0)
        {
            throw std::runtime_error("Failed to fork Python process.");
        }

        if (pid == 0)
        {
            if (!working_directory.empty())
            {
                if (chdir(working_directory.c_str()) != 0)
                    _exit(126);

                std::string pythonpath = working_directory.string();
                if (const char* existing = std::getenv("PYTHONPATH");
                    existing != nullptr && existing[0] != '\0')
                {
                    pythonpath += ":";
                    pythonpath += existing;
                }
                setenv("PYTHONPATH", pythonpath.c_str(), 1);
            }

            execv(arg_storage[0].c_str(), argv_exec.data());
            _exit(127);
        }

        int status = 0;
        while (waitpid(pid, &status, 0) < 0)
        {
            if (errno != EINTR)
            {
                throw std::runtime_error("Failed waiting for Python process.");
            }
        }

        if (WIFEXITED(status))
            return WEXITSTATUS(status);

        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);

        return 1;
#endif
    }
}

namespace embedded_script
{
    fs::path extract(
        const fs::path& destination)
    {
        fs::create_directories(destination);
        const fs::path script_path = destination / "_auto_build.py";

        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            throw std::runtime_error(
                "Could not write embedded script to: " + path_to_utf8(script_path));
        }

        out.write(
            reinterpret_cast<const char*>(auto_build_data),
            static_cast<std::streamsize>(auto_build_size));

        if (!out)
        {
            throw std::runtime_error("Failed writing embedded script contents.");
        }

        return script_path;
    }

    int run(
        const fs::path& python,
        const fs::path& script,
        const fs::path& working_directory,
        const std::vector<fs::path>& arguments)
    {
        std::vector<std::string> cli_args;
        cli_args.push_back(path_to_utf8(script));

        if (arguments.size() >= 4)
        {
            cli_args.push_back("--dest");
            cli_args.push_back(path_to_utf8(arguments[0]));
            cli_args.push_back("--base");
            cli_args.push_back(path_to_utf8(arguments[1]));
            cli_args.push_back("--update");
            cli_args.push_back(path_to_utf8(arguments[2]));
            cli_args.push_back("--keys");
            cli_args.push_back(path_to_utf8(arguments[3]));

            if (arguments.size() >= 5 && !arguments[4].empty())
            {
                cli_args.push_back("--firmware");
                cli_args.push_back(path_to_utf8(arguments[4]));
            }
        }
        else
        {
            for (const auto& arg : arguments)
            {
                if (!arg.empty())
                {
                    cli_args.push_back(path_to_utf8(arg));
                }
            }
        }

        return spawn_process(python, cli_args, working_directory);
    }

    int run_module(
        const fs::path& python,
        const fs::path& working_directory,
        const std::vector<std::string>& arguments)
    {
        return spawn_process(python, arguments, working_directory);
    }

    void remove(
        const fs::path& script)
    {
        std::error_code ec;
        fs::remove(script, ec);
    }
}