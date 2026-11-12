// SPDX-License-Identifier: Apache-2.0
// Turns the signals a user or a service manager sends into a stop request. This is the only place the example
// keeps global state (a signal handler can only set a flag), and it is not part of the server library.
#pragma once

namespace tessera::server {
class ServerHost;
}

namespace tessera::server::example {

/// From now on, Ctrl+C and the termination request of the system ask `host` to stop
/// (`ServerHost::RequestStop`): SIGINT and SIGTERM on Linux; Ctrl+C, Ctrl+Break and closing the console on
/// Windows. The handler only sets a flag; a thread started here passes it on to `host`. Asking again hands
/// the signals to the new host. `host` must outlive that thread, so call `RemoveStopSignals` before it is
/// destroyed.
void InstallStopSignals(ServerHost& host);

/// Stops passing signals to the host and ends the thread. The handlers stay installed and do nothing, so a
/// late signal does not end the process with a failure code.
void RemoveStopSignals() noexcept;

} // namespace tessera::server::example
