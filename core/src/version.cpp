// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/version.hpp"

namespace tessera::net {

Version GetVersion() noexcept {
    return Version{0, 0, 1};
}

std::string_view GetVersionString() noexcept {
    return "0.0.1";
}

}  // namespace tessera::net
