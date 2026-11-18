// SPDX-License-Identifier: Apache-2.0
// The command line of tessera_schema, as a function so that the tests can call it without starting a process.
#pragma once

#include <iosfwd>
#include <span>
#include <string>

namespace tessera::schema {

/// Runs one command. `args` are the arguments without the program name. Messages for the user go to `out`
/// (results) and `err` (usage and errors). Returns the exit code: 0 for success, 1 when the file or the
/// schema is wrong, 2 for a wrong command line, 3 when a file cannot be read or written.
int Run(std::span<const std::string> args, std::ostream& out, std::ostream& err);

} // namespace tessera::schema
