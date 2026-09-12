#include "platform/crash_handler.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <utility>
#include <vector>

#pragma comment(lib, "Dbghelp.lib")

namespace screenrec::crash {

namespace {

constexpr size_t MAX_KEPT_DUMPS = 5;
constexpr int MAX_FRAMES = 62;

char gTag[32] = "process";
char gLogsDir[MAX_PATH] = "";
LONG gHandled = 0;

void buildStamps(char* logStamp, size_t logStampSize, char* fileStamp, size_t fileStampSize) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(logStamp, logStampSize, _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds);
    _snprintf_s(fileStamp, fileStampSize, _TRUNCATE, "%04u%02u%02uT%02u%02u%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void writeLine(HANDLE file, const char* stamp, const char* text) {
    char line[1200];
    const int len = _snprintf_s(line, _TRUNCATE, "[%s] [error] [CrashHandler] %s\n", stamp, text);
    if (len > 0) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(len), &written, nullptr);
    }
}

void writeDump(EXCEPTION_POINTERS* info, const char* fileStamp, DWORD pid, char* dumpPath,
    size_t dumpPathSize) {
    _snprintf_s(dumpPath, dumpPathSize, _TRUNCATE, "%s\\crash_%s_%s_pid%lu.dmp",
        gLogsDir, gTag, fileStamp, pid);
    const HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        dumpPath[0] = '\0';
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    if (MiniDumpWriteDump(GetCurrentProcess(), pid, file, MiniDumpNormal,
            info != nullptr ? &mei : nullptr, nullptr, nullptr) == 0) {
        dumpPath[0] = '\0';
    }
    CloseHandle(file);
}

void writeFrame(HANDLE file, const char* stamp, int index, DWORD64 pc) {
    const HANDLE process = GetCurrentProcess();

    char moduleName[MAX_PATH] = "?";
    const DWORD64 moduleBase = SymGetModuleBase64(process, pc);
    if (moduleBase != 0) {
        char fullPath[MAX_PATH] = "";
        if (GetModuleFileNameA(reinterpret_cast<HMODULE>(moduleBase), fullPath,
                MAX_PATH) != 0) {
            const char* base = strrchr(fullPath, '\\');
            strncpy_s(moduleName, base != nullptr ? base + 1 : fullPath, _TRUNCATE);
        }
    }

    unsigned char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;

    char text[512];
    if (SymFromAddr(process, pc, &displacement, symbol) != 0) {
        _snprintf_s(text, _TRUNCATE, "  #%02d %s!%s+0x%llx", index, moduleName,
            symbol->Name, static_cast<unsigned long long>(displacement));
    } else {
        _snprintf_s(text, _TRUNCATE, "  #%02d %s+0x%llx", index, moduleName,
            static_cast<unsigned long long>(pc - moduleBase));
    }
    writeLine(file, stamp, text);
}

void writeCrashReport(EXCEPTION_POINTERS* info, const char* reason) {
    if (InterlockedCompareExchange(&gHandled, 1, 0) != 0) {
        return;
    }

    char logStamp[32];
    char fileStamp[24];
    buildStamps(logStamp, sizeof(logStamp), fileStamp, sizeof(fileStamp));
    const DWORD pid = GetCurrentProcessId();

    char dumpPath[MAX_PATH];
    writeDump(info, fileStamp, pid, dumpPath, sizeof(dumpPath));

    char logPath[MAX_PATH];
    _snprintf_s(logPath, _TRUNCATE, "%s\\screen_recording_%s_crash_%s_pid%lu.log",
        gLogsDir, gTag, fileStamp, pid);
    const HANDLE file = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    char header[600];
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        _snprintf_s(header, _TRUNCATE,
            "%s in %s: code=0x%08lX address=0x%p thread=%lu pid=%lu dump=%s",
            reason, gTag,
            static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
            info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(), pid,
            dumpPath[0] != '\0' ? dumpPath : "unavailable");
    } else {
        _snprintf_s(header, _TRUNCATE, "%s in %s: thread=%lu pid=%lu dump=%s", reason, gTag,
            GetCurrentThreadId(), pid,
            dumpPath[0] != '\0' ? dumpPath : "unavailable");
    }
    writeLine(file, logStamp, header);

    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

#if defined(_M_X64)
    if (info != nullptr && info->ContextRecord != nullptr) {
        CONTEXT ctx = *info->ContextRecord;
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < MAX_FRAMES; ++i) {
            if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame,
                    &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                    nullptr) == 0 ||
                frame.AddrPC.Offset == 0) {
                break;
            }
            writeFrame(file, logStamp, i, frame.AddrPC.Offset);
        }
    } else
#endif
    {
        void* frames[MAX_FRAMES] = {};
        const USHORT captured = RtlCaptureStackBackTrace(0, MAX_FRAMES, frames, nullptr);
        for (USHORT i = 0; i < captured; ++i) {
            writeFrame(file, logStamp, i, reinterpret_cast<DWORD64>(frames[i]));
        }
    }

    FlushFileBuffers(file);
    CloseHandle(file);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* info) {
    writeCrashReport(info, "Unhandled exception");
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() {
    writeCrashReport(nullptr, "std::terminate");
    ExitProcess(3);
}

void pruneOldDumps() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> dumps;
    for (fs::directory_iterator it(gLogsDir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string name = it->path().filename().string();
        if (name.rfind("crash_", 0) == 0 && it->path().extension() == ".dmp") {
            dumps.emplace_back(it->last_write_time(ec), it->path());
        }
    }
    std::sort(dumps.begin(), dumps.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = MAX_KEPT_DUMPS; i < dumps.size(); ++i) {
        fs::remove(dumps[i].second, ec);
    }
}

}

void install(const std::string& processTag, const std::string& logsDir) {
    strncpy_s(gTag, processTag.c_str(), _TRUNCATE);
    strncpy_s(gLogsDir, logsDir.c_str(), _TRUNCATE);
    CreateDirectoryA(gLogsDir, nullptr);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    std::set_terminate(onTerminate);
    pruneOldDumps();
}

}

#else

namespace screenrec::crash {

void install(const std::string& /*processTag*/, const std::string& /*logsDir*/) {}

}

#endif
