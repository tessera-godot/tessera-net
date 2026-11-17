// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/cli.hpp"

#include <ostream>

namespace tessera::schema {

namespace {

void PrintUsage(std::ostream& stream) {
    stream << "Usage:\n"
           << "  tessera_schema check <schema.json>\n"
           << "  tessera_schema hash <schema.json>\n"
           << "  tessera_schema generate --cpp <output.hpp> [--namespace a::b] <schema.json>\n";
}

} // namespace

int Run(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
    if (!args.empty() && (args[0] == "--help" || args[0] == "-h" || args[0] == "help")) {
        PrintUsage(out);
        return 0;
    }
    PrintUsage(err);
    return 2;
}

} // namespace tessera::schema
