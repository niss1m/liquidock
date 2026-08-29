#include "core/CrashHandler.h"

#include <windows.h>
#include <dbghelp.h>

#include "core/ConfigPaths.h"

namespace liquidock {
namespace {

constexpr int kMaxFrames = 48;

void Write(HANDLE file, const char* text) {
    if (file == INVALID_HANDLE_VALUE || !text) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
}

const char* ExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        default:                              return "EXCEPTION";
    }
}

LONG WINAPI Filter(EXCEPTION_POINTERS* info) {
    const std::wstring path = ConfigFilePath(L"crash.txt");
    HANDLE file = path.empty() ? INVALID_HANDLE_VALUE
                               : CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    char line[1024];
    if (info && info->ExceptionRecord) {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        wsprintfA(line, "LiquiDock %s crash\r\n%s (0x%08X) at 0x%p\r\n", LIQUIDOCK_VERSION,
                  ExceptionName(code), code, info->ExceptionRecord->ExceptionAddress);
        Write(file, line);
        if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
            wsprintfA(line, "  %s address 0x%p\r\n",
                      info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                      reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
            Write(file, line);
        }
        wsprintfA(line, "  thread %u\r\n\r\n", GetCurrentThreadId());
        Write(file, line);
    }

    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);

    // The context is copied: StackWalk64 modifies whatever it is handed, and
    // corrupting the record the OS is about to act on makes a bad day worse.
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    for (int i = 0; i < kMaxFrames; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }

        DWORD64 displacement = 0;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
            wsprintfA(line, "  %02d  %s + 0x%X\r\n", i, symbol->Name,
                      static_cast<unsigned>(displacement));
        } else {
            wsprintfA(line, "  %02d  0x%p\r\n", i,
                      reinterpret_cast<void*>(frame.AddrPC.Offset));
        }
        Write(file, line);

        // Line numbers only resolve when the .pdb is beside the executable,
        // which is true for a local build and not for a shipped one. The
        // function names come from the export table either way.
        IMAGEHLP_LINE64 source{};
        source.SizeOfStruct = sizeof(source);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &source)) {
            wsprintfA(line, "        %s:%u\r\n", source.FileName, source.LineNumber);
            Write(file, line);
        }
    }

    SymCleanup(process);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void InstallCrashHandler() {
    SetUnhandledExceptionFilter(&Filter);
}

} // namespace liquidock
