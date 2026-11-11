// SPDX-License-Identifier: Apache-2.0
// Turns the signals a user or a service manager sends into a stop request. This is the only place the example
// keeps global state (the signal handler has to find the host somehow), and it is not part of the server
// library.
#pragma once

namespace tessera::server {
class ServerHost;
}

namespace tessera::server::example {

/// From now on, Ctrl+C and the termination request of the system ask `host` to stop
/// (`ServerHost::RequestStop`): SIGINT and SIGTERM on Linux; Ctrl+C, Ctrl+Break and closing the console on
/// Windows. Asking twice changes nothing. `host` must outlive the handlers, so call `RemoveStopSignals`
/// before it is destroyed.
void InstallStopSignals(ServerHost& host);

/// Puts the default behavior of the signals back.
void RemoveStopSignals() noexcept;

} // namespace tessera::server::example
