// SPDX-License-Identifier: Apache-2.0
// A test run that dies (an uncaught exception, a failed assertion) must end with an error code and a line on
// the console, not with a dialog that waits for someone to press a button. This is linked into the server
// test programs only.
#ifdef _MSC_VER

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <crtdbg.h>
#include <cstdlib>

namespace {

struct NoDialogs {
    NoDialogs() {
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
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
