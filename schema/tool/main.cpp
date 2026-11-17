// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/cli.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return tessera::schema::Run(args, std::cout, std::cerr);
}
