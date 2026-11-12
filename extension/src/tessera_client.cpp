// SPDX-License-Identifier: Apache-2.0
#include "tessera_client.hpp"

#include <godot_cpp/core/class_db.hpp>

namespace godot {

String TesseraClient::ping() const {
    return "tessera";
}

void TesseraClient::_bind_methods() {
    ClassDB::bind_method(D_METHOD("ping"), &TesseraClient::ping);
}

} // namespace godot
