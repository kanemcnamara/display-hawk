#include "CrashReport.h"

#include <QDir>

#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <dbghelp.h>
#else
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

// Prepared up front: the crash handler should not have to allocate much.
char g_folder[1024] = {};

void stampedPath(char *out, size_t size, const char *ext)
{
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    std::snprintf(out, size, "%s/crash-%s.%s", g_folder, stamp, ext);
}

#if defined(Q_OS_WIN) && (defined(_M_X64) || defined(__x86_64__))

LONG WINAPI onCrash(EXCEPTION_POINTERS *info)
{
    char path[1200];
    stampedPath(path, sizeof(path), "txt");
    if (FILE *f = std::fopen(path, "w")) {
        std::fprintf(f, "Display Hawk crashed: exception 0x%08lX at %p (thread %lu)\n",
                     info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress,
                     GetCurrentThreadId());
        // The faulting thread's stack from the exception's context.
        CONTEXT ctx = *info->ContextRecord;
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
        HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
        SymInitialize(process, nullptr, TRUE);
        for (int i = 0; i < 48; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)
                || frame.AddrPC.Offset == 0)
                break;
            const DWORD64 pc = frame.AddrPC.Offset;
            HMODULE module = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(pc), &module))
                GetModuleFileNameA(module, name, sizeof(name));
            const char *base = std::strrchr(name, '\\');
            // Offsets are as the module's preferred image base would put them (addr2line -e <module>).
            const DWORD64 rva = module ? pc - DWORD64(module) : pc;
            char symbol[256] = "";
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
            auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, pc, &displacement, sym))
                std::snprintf(symbol, sizeof(symbol), "  %s+0x%llx", sym->Name, displacement);
            std::fprintf(f, "#%-2d %s+0x%llx%s\n", i, base ? base + 1 : name, rva, symbol);
        }
        std::fclose(f);
    }
    char dumpPath[1200];
    stampedPath(dumpPath, sizeof(dumpPath), "dmp");
    HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION ex{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &ex,
                          nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH; // and let Windows end the process as usual
}

#elif !defined(Q_OS_WIN)

void onSignal(int sig)
{
    char path[1200];
    stampedPath(path, sizeof(path), "txt");
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char line[128];
        const int n = std::snprintf(line, sizeof(line), "Display Hawk crashed: signal %d\n", sig);
        if (n > 0)
            (void)write(fd, line, size_t(n));
        void *frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, fd);
        close(fd);
    }
    std::signal(sig, SIG_DFL); // and crash as usual (the OS crash reporter still runs)
    std::raise(sig);
}

#endif

} // namespace

namespace CrashReport {

void install(const QString &folder)
{
    QDir().mkpath(folder);
    const QByteArray f = QDir::toNativeSeparators(folder).toLocal8Bit();
    std::snprintf(g_folder, sizeof(g_folder), "%s", f.constData());
#if defined(Q_OS_WIN) && (defined(_M_X64) || defined(__x86_64__))
    SetUnhandledExceptionFilter(onCrash);
#elif !defined(Q_OS_WIN)
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        std::signal(sig, onSignal);
#endif
}

} // namespace CrashReport
