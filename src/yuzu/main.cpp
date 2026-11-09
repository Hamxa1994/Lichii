// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <QApplication>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "nx2_switches.h"
#include "common/logging.h"
#include "startup_checks.h"

#if YUZU_ROOM
#include <cstring>
#include "dedicated_room/yuzu_room.h"
#endif
#ifdef __unix__
#include "qt_common/gui_settings.h"
#endif

#ifndef _WIN32
#include <sys/resource.h>
#endif

#if defined(__APPLE__)
#include <climits>
#include <cstdlib>
#include <cstring>
#endif

#include "main_window.h"

#ifdef _WIN32
#include <QScreen>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// This frontend is a GUI program, so a crash prints nowhere, and YUZU_CRASH_DUMPS is off on
// Windows (CMakeLists.txt: "unimplemented on Windows"): crash_dumps stayed empty while
// eden_log.txt simply stopped mid-line, with no faulting address to work from. This filter writes
// the address and stack as module+offset (resolve with the module's PDB) plus a minidump into the
// profile's crash_dumps directory, so a crash at full speed can be read afterwards. Nothing here
// allocates or takes a lock: it runs while the process is already broken.
namespace {

char g_crash_directory[MAX_PATH]{};

void WriteCrashText(HANDLE file, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length > 0) {
        const int limit = static_cast<int>(sizeof(buffer)) - 1;
        DWORD written{};
        WriteFile(file, buffer, static_cast<DWORD>(length < limit ? length : limit), &written,
                  nullptr);
    }
}

void WriteCrashAddress(HANDLE file, const char* label, const void* address) {
    HMODULE module{};
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) != 0) {
        GetModuleFileNameA(module, name, sizeof(name));
    }
    WriteCrashText(file, "  %s %p %s+%#llx\n", label, address, name,
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                    reinterpret_cast<uintptr_t>(module)));
}

LONG WINAPI WriteGuiCrashDump(EXCEPTION_POINTERS* exception) {
    static LONG entered{};
    if (InterlockedExchange(&entered, 1) != 0 || g_crash_directory[0] == '\0') {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The log file is buffered, so the last lines before the fault (often the ones that show what
    // the emulator was doing) are still sitting in it. Keep them before doing anything else.
    Common::Log::FlushFileBackend();
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
    const DWORD pid = GetCurrentProcessId();
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\eden-crash-%lu.txt", g_crash_directory,
                  static_cast<unsigned long>(pid));
    const HANDLE text = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (text != INVALID_HANDLE_VALUE) {
        WriteCrashText(text, "Unhandled exception %#lx on thread %lu, information %#llx %#llx\n",
                       static_cast<unsigned long>(record.ExceptionCode), GetCurrentThreadId(),
                       static_cast<unsigned long long>(
                           record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0),
                       static_cast<unsigned long long>(
                           record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0));
        WriteCrashAddress(text, "at", record.ExceptionAddress);
        void* frames[48];
        const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < count; ++i) {
            WriteCrashAddress(text, "frame", frames[i]);
        }
        CloseHandle(text);
    }
    // The small flag set yuzu_cmd uses: the larger MiniDumpWithIndirectlyReferencedMemory failed
    // on Eden's address space and left an empty file.
    struct ExceptionInformation {
        DWORD thread_id;
        EXCEPTION_POINTERS* exception_pointers;
        BOOL client_pointers;
    };
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int,
                                              const ExceptionInformation*, const void*,
                                              const void*);
    const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    const auto write = dbghelp != nullptr
                           ? reinterpret_cast<MiniDumpWriteDumpFn>(
                                 GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                           : nullptr;
    if (write != nullptr) {
        std::snprintf(path, sizeof(path), "%s\\eden-crash-%lu.dmp", g_crash_directory,
                      static_cast<unsigned long>(pid));
        const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const ExceptionInformation information{GetCurrentThreadId(), exception, FALSE};
            write(GetCurrentProcess(), pid, file, 0x1000, &information, nullptr, nullptr);
            CloseHandle(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// SetUnhandledExceptionFilter never runs for an uncaught C++ exception: that path ends in
// std::terminate, which calls abort() and skips the SEH filter entirely. The Vulkan wrapper throws
// on driver errors, so a failed device creation died with no report at all. This records one.
[[noreturn]] void OnTerminate() {
    Common::Log::FlushFileBackend();
    std::string reason = "std::terminate called with no active exception";
    if (auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            reason = e.what();
        } catch (...) {
            reason = "non-std exception type";
        }
    }
    if (g_crash_directory[0] != '\0') {
        const DWORD pid = GetCurrentProcessId();
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "%s\\eden-crash-%lu-terminate.txt", g_crash_directory,
                      static_cast<unsigned long>(pid));
        const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            WriteCrashText(file, "Uncaught exception on thread %lu: %s\n", GetCurrentThreadId(),
                           reason.c_str());
            void* frames[48];
            const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
            for (USHORT i = 0; i < count; ++i) {
                WriteCrashAddress(file, "frame", frames[i]);
            }
            CloseHandle(file);
        }
    }
    std::abort();
}

// Called once at startup, after nx2_switches.txt is loaded so an EDEN_NX2_CRASH_DUMP set there
// counts: resolves the directory the handler writes to, so the handler does no path work itself.
void InstallGuiCrashHandler() {
    std::string directory;
    if (const char* environment = std::getenv("EDEN_NX2_CRASH_DUMP")) {
        directory = environment;
    } else {
        directory = Common::FS::PathToUTF8String(
            Common::FS::GetEdenPath(Common::FS::EdenPath::CrashDumpsDir));
    }
    if (directory.empty() || directory.size() >= sizeof(g_crash_directory)) {
        return;
    }
    std::memcpy(g_crash_directory, directory.c_str(), directory.size());
    g_crash_directory[directory.size()] = '\0';
    CreateDirectoryA(g_crash_directory, nullptr);
    SetUnhandledExceptionFilter(WriteGuiCrashDump);
    std::set_terminate(OnTerminate);
}

} // Anonymous namespace

static void OverrideWindowsFont() {
    // Qt5 chooses these fonts on Windows and they have fairly ugly alphanumeric/cyrillic characters
    // Asking to use "MS Shell Dlg 2" gives better other chars while leaving the Chinese Characters.
    const QString startup_font = QApplication::font().family();
    const QStringList ugly_fonts = {QStringLiteral("SimSun"), QStringLiteral("PMingLiU")};
    if (ugly_fonts.contains(startup_font)) {
        QApplication::setFont(QFont(QStringLiteral("MS Shell Dlg 2"), 9, QFont::Normal));
    }
}
#endif

static Qt::HighDpiScaleFactorRoundingPolicy GetHighDpiRoundingPolicy() {
#ifdef _WIN32
    // For Windows, we want to avoid scaling artifacts on fractional scaling ratios.
    // This is done by setting the optimal scaling policy for the primary screen.

    // Create a temporary QApplication.
    int temp_argc = 0;
    char** temp_argv = nullptr;
    QApplication temp{temp_argc, temp_argv};

    // Get the current screen geometry.
    const QScreen* primary_screen = QGuiApplication::primaryScreen();
    if (primary_screen == nullptr) {
        return Qt::HighDpiScaleFactorRoundingPolicy::PassThrough;
    }

    const QRect screen_rect = primary_screen->geometry();
    const qreal real_ratio = primary_screen->devicePixelRatio();
    const qreal real_width = std::trunc(screen_rect.width() * real_ratio);
    const qreal real_height = std::trunc(screen_rect.height() * real_ratio);

    // Recommended minimum width and height for proper window fit.
    // Any screen with a lower resolution than this will still have a scale of 1.
    constexpr qreal minimum_width = 1350.0;
    constexpr qreal minimum_height = 900.0;

    const qreal width_ratio = std::max(1.0, real_width / minimum_width);
    const qreal height_ratio = std::max(1.0, real_height / minimum_height);

    // Get the lower of the 2 ratios and truncate, this is the maximum integer scale.
    const qreal max_ratio = std::trunc(std::min(width_ratio, height_ratio));
    return max_ratio > real_ratio ? Qt::HighDpiScaleFactorRoundingPolicy::Round
                                  : Qt::HighDpiScaleFactorRoundingPolicy::Floor;
#else
    // Other OSes should be better than Windows at fractional scaling.
    return Qt::HighDpiScaleFactorRoundingPolicy::PassThrough;
#endif
}

int main(int argc, char* argv[]) {
    LoadNx2Switches(argv[0]);
#ifdef _WIN32
    InstallGuiCrashHandler();
#endif
#if YUZU_ROOM
    bool launch_room = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--room") == 0) {
            launch_room = true;
        }
    }

    if (launch_room) {
        LaunchRoom(argc, argv, true);
        return 0;
    }
#endif

    bool has_broken_vulkan = false;
    bool is_child = false;
    if (CheckEnvVars(&is_child)) {
        return 0;
    }

    if (StartupChecks(argv[0], &has_broken_vulkan,
                      Settings::values.perform_vulkan_check.GetValue())) {
        return 0;
    }

#ifdef YUZU_CRASH_DUMPS
    Breakpad::InstallCrashHandler();
#endif

    // Init settings params
    QCoreApplication::setOrganizationName(QStringLiteral("eden"));
    QCoreApplication::setApplicationName(QStringLiteral("Lichii"));

    // Increases the maximum open file limit.
    // TODO: This should be common to all frontends.
#ifdef _WIN32
    // MSVCRT limits this to 2048 for some inexplicable (and likely arcane) reason,
    // so we have to account for that as well.
#ifdef __MSVCRT__
    _setmaxstdio(2048);
#else
    _setmaxstdio(8192);
#endif // __MSVCRT__
#elif defined(__unix__) || defined(__APPLE__)
    // Raise the open file limit to at least 8192, or the hard limit.
    // Most sane systems should not hit the hard limit here.
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = std::min<rlim_t>(1048576, rl.rlim_max);
        setrlimit(RLIMIT_NOFILE, &rl);
    }
#endif // _WIN32

#if defined(__APPLE__)
    // Convert the relative path to an absolute path before the chdir
    char resolved[PATH_MAX];
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "-input-profile") == 0) {
            ++i;
        } else if (argv[i][0] != '-' && argv[i][0] != '/' && realpath(argv[i], resolved)) {
            argv[i] = strdup(resolved);
        }
    }

    // If you start a bundle (binary) on OSX without the Terminal, the working directory is "/".
    // But since we require the working directory to be the executable path for the location of
    // the user folder in the Qt Frontend, we need to cd into that working directory
    const auto bin_path = Common::FS::GetBundleDirectory() / "..";
    chdir(Common::FS::PathToUTF8String(bin_path).c_str());
#endif

#ifdef __unix__
    // Set the DISPLAY variable in order to open web browsers
    // TODO (lat9nq): Find a better solution for AppImages to start external applications
    if (QString::fromLocal8Bit(qgetenv("DISPLAY")).isEmpty()) {
        qputenv("DISPLAY", ":0");
    }

    if (GraphicsBackend::GetForceX11() && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");

    // Fix the Wayland appId. This needs to match the name of the .desktop file without the .desktop
    // suffix.
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.zortos293.lichii"));
#endif

    auto rounding_policy = GetHighDpiRoundingPolicy();
    QApplication::setHighDpiScaleFactorRoundingPolicy(rounding_policy);

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Disables the "?" button on all dialogs. Disabled by default on Qt6.
    QCoreApplication::setAttribute(Qt::AA_DisableWindowContextHelpButton);
#endif

    // Enables the core to make the qt created contexts current on std::threads
    QCoreApplication::setAttribute(Qt::AA_DontCheckOpenGLContextThreadAffinity);

#ifdef _WIN32
    QApplication::setStyle(QStringLiteral("windowsvista"));
#endif

    QApplication app(argc, argv);

#ifdef _WIN32
    OverrideWindowsFont();
#endif

    // Workaround for QTBUG-85409, for Suzhou numerals the number 1 is actually \u3021
    // so we can see if we get \u3008 instead
    // TL;DR all other number formats are consecutive in unicode code points
    // This bug is fixed in Qt6, specifically 6.0.0-alpha1
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const QLocale locale = QLocale::system();
    if (QStringLiteral("\u3008") == locale.toString(1)) {
        QLocale::setDefault(QLocale::system().name());
    }
#endif

    // Qt changes the locale and causes issues in float conversion using std::to_string() when
    // generating shaders
    setlocale(LC_ALL, "C");

    MainWindow main_window{has_broken_vulkan};
    // After settings have been loaded by GMainWindow, apply the filter
    main_window.show();

    app.connect(&app, &QGuiApplication::applicationStateChanged, &main_window, &MainWindow::OnAppFocusStateChanged);
    return app.exec();
}
