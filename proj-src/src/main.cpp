#include "check_nsp.h"
#include "download.h"
#include "embedded_script.h"
#include "extraction.h"

#include <curl/curl.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <conio.h>
#include <io.h>
#include <shellapi.h>
#else
#include <csignal>
#include <termios.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef APP_VERSION
#define APP_VERSION "1.0.0"
#endif

namespace fs = std::filesystem;

namespace
{
    std::atomic<bool> g_interrupted{false};

    class ProcessInterrupted : public std::exception
    {
    public:
        const char* what() const noexcept override
        {
            return "The process was interrupted.";
        }
    };

#if defined(_WIN32)
    BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
    {
        if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT)
        {
            // Allow a second Ctrl+C to force-terminate immediately if a blocking call is stuck.
            if (g_interrupted.exchange(true, std::memory_order_relaxed))
            {
                return FALSE;
            }
            return TRUE;
        }
        return FALSE;
    }
#else
    void posix_signal_handler(int)
    {
        g_interrupted.store(true, std::memory_order_relaxed);
    }
#endif

    void install_interrupt_handler()
    {
#if defined(_WIN32)
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
        struct sigaction sa{};
        sa.sa_handler = posix_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
#endif
    }

    bool is_interrupt_exit_code(int exit_code) noexcept
    {
#if defined(_WIN32)
        // STATUS_CONTROL_C_EXIT (0xC000013A)
        return exit_code == static_cast<int>(0xC000013A);
#else
        return exit_code == 130 || exit_code == -SIGINT;
#endif
    }

    void throw_if_interrupted()
    {
        if (g_interrupted.load(std::memory_order_relaxed))
        {
            throw ProcessInterrupted();
        }
    }

    class CurlGlobal
    {
    public:
        CurlGlobal()
        {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            {
                throw std::runtime_error("Could not initialize libcurl.");
            }
        }

        ~CurlGlobal()
        {
            curl_global_cleanup();
        }

        CurlGlobal(const CurlGlobal&) = delete;
        CurlGlobal& operator=(const CurlGlobal&) = delete;
    };

    class EmbeddedScriptGuard
    {
    public:
        explicit EmbeddedScriptGuard(const fs::path& destination)
            : script_path_(embedded_script::extract(destination))
        {
        }

        ~EmbeddedScriptGuard()
        {
            embedded_script::remove(script_path_);
        }

        const fs::path& path() const noexcept
        {
            return script_path_;
        }

        EmbeddedScriptGuard(const EmbeddedScriptGuard&) = delete;
        EmbeddedScriptGuard& operator=(const EmbeddedScriptGuard&) = delete;

    private:
        fs::path script_path_;
    };

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

    fs::path get_executable_dir(const char* argv0)
    {
#if defined(_WIN32)
        (void)argv0;
        std::wstring buffer(MAX_PATH, L'\0');
        DWORD len = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));

        while (len == buffer.size())
        {
            buffer.resize(buffer.size() * 2);
            len = GetModuleFileNameW(
                nullptr,
                buffer.data(),
                static_cast<DWORD>(buffer.size()));
        }

        if (len == 0)
        {
            throw std::runtime_error("Could not determine executable directory.");
        }

        buffer.resize(len);
        return fs::path(buffer).parent_path();
#else
        std::error_code ec;
        const fs::path proc_exe = fs::canonical("/proc/self/exe", ec);
        if (!ec && !proc_exe.empty())
            return proc_exe.parent_path();

        if (argv0 != nullptr && argv0[0] != '\0')
        {
            const fs::path fallback = fs::weakly_canonical(argv0, ec);
            if (!ec && !fallback.empty())
                return fallback.parent_path();
        }

        return fs::current_path();
#endif
    }

    std::vector<fs::path> get_cli_path_args(int argc, char* argv[])
    {
        std::vector<fs::path> args;
#if defined(_WIN32)
        (void)argc;
        (void)argv;
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv != nullptr)
        {
            for (int i = 0; i < wargc; ++i)
            {
                args.emplace_back(wargv[i]);
            }
            LocalFree(wargv);
            return args;
        }
#endif
        for (int i = 0; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }
        return args;
    }

    void print_version()
    {
        std::cout << "totk-auto-build v" << APP_VERSION << '\n';
    }

    void print_help()
    {
        std::cout
            << "TOTK PC Recomp Builder Bootstrap\n\n"
            << "Usage:\n"
#if defined(_WIN32)
            << "  totk-auto-build [-h, --help] [-v --version] [<folder>]\n\n"
#else
            << "  ./AutoBuildRun [-h, --help] [-v --version] [<folder>]\n\n"
#endif
            << "Options:\n"
            << "  -h, --help\n"
            << "      Display this help message and exit.\n\n"
            << "  -v, --version\n"
            << "      Display the version number and exit.\n\n"
            << "  <folder>\n"
            << "      Optional\n\n"
            << "      Folder containing the TOTK base NSP/XCI, update NSP\n"
            << "      and prod.keys.\n\n"
            << "      Optionally contains firmware/ directory\n"
            << "      holding firmware NCAs.\n\n"
#if defined(_WIN32)
            << "      Drag and drop the folder onto the executable\n"
#else
            << "      Drag and drop the folder onto totk-auto-build.desktop\n"
            << "      or pass it to ./AutoBuildRun <folder>\n"
#endif
            << "      to start the automatic rebuild.\n";
    }

    void wait_for_any_key()
    {
#if defined(_WIN32)
        if (_isatty(_fileno(stdin)))
        {
            HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
            if (stdin_handle != INVALID_HANDLE_VALUE && stdin_handle != nullptr)
            {
                FlushConsoleInputBuffer(stdin_handle);
            }
            (void)_getch();
            std::cout << '\n';
            return;
        }
#else
        if (isatty(STDIN_FILENO))
        {
            tcflush(STDIN_FILENO, TCIFLUSH);
        }
#endif
        std::cin.clear();
        std::string dummy;
        std::getline(std::cin, dummy);
    }

    void print_failed_and_wait()
    {
        std::cout
            << "\nThe process could not complete. Check the log and try again later.\n"
            << "Press any key to exit..."
            << std::flush;

        wait_for_any_key();
    }

    void print_interrupted_and_wait()
    {
        std::cout
            << "\nThe process was interrupted.\n"
            << "Press any key to exit..."
            << std::flush;

        wait_for_any_key();
    }

    void ensure_latest_release(
        const fs::path& setup_dir,
        const download::ReleaseAsset& asset)
    {
        const fs::path version_file = setup_dir / "VERSION";

        if (download::is_version_current(version_file, asset.tag))
        {
            std::cout
                << "Latest release already available ("
                << asset.tag
                << "). Skipping download.\n";
            return;
        }

        std::cout
            << "\nNew release available: "
            << asset.tag
            << '\n';

        const fs::path archive_path = setup_dir / asset.name;

        download::download_file(
            asset.url,
            archive_path,
            asset.name,
            &g_interrupted);

        throw_if_interrupted();

        try
        {
            extraction::extract_archive(
                archive_path,
                setup_dir,
                asset.name,
                &g_interrupted);
        }
        catch (...)
        {
            std::error_code ec;
            fs::remove(version_file, ec);
            fs::remove(archive_path, ec);
            throw;
        }

        std::error_code ec;
        fs::remove(archive_path, ec);

        throw_if_interrupted();

        std::cout << "\nRelease extraction completed.\n";
    }

    int launch_python_setup(
        const fs::path& root,
        const fs::path& setup_dir,
        const fs::path& dropped_folder)
    {
#if defined(_WIN32)
        const fs::path python = setup_dir / "python" / "python.exe";
#else
        const fs::path python = setup_dir / "python" / "bin" / "python3";
#endif

        if (dropped_folder.empty())
        {
            return embedded_script::run_module(
                python,
                setup_dir,
                {
                    "-m",
                    "installer.gui",
                    "--rebuild",
                    path_to_utf8(root)
                });
        }

        const check_nsp::GameFiles game_files =
            check_nsp::validate_folder(dropped_folder, setup_dir, &g_interrupted);

        throw_if_interrupted();

        std::cout << "\nRunning TOTK Setup. Wait for process to complete...\n";

        EmbeddedScriptGuard script(setup_dir);

        return embedded_script::run(
            python,
            script.path(),
            setup_dir,
            {
                root,
                fs::absolute(game_files.base),
                fs::absolute(game_files.update),
                fs::absolute(game_files.keys),
                game_files.firmware.empty()
                    ? fs::path{}
                    : fs::absolute(game_files.firmware)
            });
    }
}

int main(int argc, char* argv[])
{
    install_interrupt_handler();

    try
    {
        const std::vector<fs::path> args = get_cli_path_args(argc, argv);
        fs::path dropped_folder;

        if (args.size() >= 2)
        {
            const std::string arg1 = path_to_utf8(args[1]);
            if (arg1 == "-h" || arg1 == "--help")
            {
                print_help();
                return 0;
            }

            if (arg1 == "-v" || arg1 == "--version")
            {
                print_version();
                return 0;
            }

            if (args.size() > 2)
            {
                throw std::runtime_error(
                    "Too many arguments. Drag and drop a single folder onto the executable, or run with -h for help.");
            }

            dropped_folder = args[1];
        }

        throw_if_interrupted();

        CurlGlobal curl;

        const fs::path root = get_executable_dir(argc > 0 ? argv[0] : nullptr);
        const fs::path setup = root / "_setup";

        fs::create_directories(setup);
        throw_if_interrupted();

        const download::ReleaseAsset asset =
            download::get_latest_release_asset(&g_interrupted);

        throw_if_interrupted();

        ensure_latest_release(setup, asset);

        const int exit_code = launch_python_setup(root, setup, dropped_folder);

        if (g_interrupted.load(std::memory_order_relaxed) || is_interrupt_exit_code(exit_code))
        {
            throw ProcessInterrupted();
        }

        if (exit_code != 0)
        {
            throw std::runtime_error(
                "Python exited with code " + std::to_string(exit_code));
        }

        std::cout
            << "\nTOTK Recomp Builder completed successfully.\n"
            << "Press any key to exit..."
            << std::flush;

        wait_for_any_key();
        return 0;
    }
    catch (const ProcessInterrupted&)
    {
        print_interrupted_and_wait();
        return 130;
    }
    catch (const download::DownloadError& error)
    {
        std::cerr << '\n' << error.what() << '\n';
        print_failed_and_wait();
        return 1;
    }
    catch (const extraction::ExtractionError&)
    {
        print_failed_and_wait();
        return 1;
    }
    catch (const std::exception& error)
    {
        if (g_interrupted.load(std::memory_order_relaxed))
        {
            print_interrupted_and_wait();
            return 130;
        }

        std::cerr << '\n' << error.what() << '\n';
        print_failed_and_wait();
        return 1;
    }
}