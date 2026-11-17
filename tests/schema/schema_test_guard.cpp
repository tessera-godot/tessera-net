// SPDX-License-Identifier: Apache-2.0
// A test run that dies (an uncaught exception, a failed assertion, a bad argument to a C library function) must end
// with an error code and a line on the console, not with a dialog that waits for someone to press a button. This is
// linked into the schema test program only.
#ifdef _MSC_VER

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <windows.h>

namespace {

void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    std::fputs("invalid parameter passed to a C runtime function\n", stderr);
    std::fflush(stderr);
    std::_Exit(3);
}

struct NoDialogs {
    NoDialogs() {
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_invalid_parameter_handler(OnInvalidParameter);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    }
};

const NoDialogs no_dialogs;

} // namespace

#endif
