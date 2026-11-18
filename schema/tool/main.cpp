// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/cli.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <crtdbg.h>
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _MSC_VER
    // A failure of the tool ends with a message and an exit code, never with a dialog that stops a build.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    try {
        return tessera::schema::Run(args, std::cout, std::cerr);
    } catch (const std::exception& problem) {
        std::cerr << "tessera_schema: unexpected error: " << problem.what() << "\n";
        return 3;
    }
}
