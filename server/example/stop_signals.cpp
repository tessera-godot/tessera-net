// SPDX-License-Identifier: Apache-2.0
#include "stop_signals.hpp"

#include "tessera/server/server_host.hpp"

#include <atomic>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <signal.h>
#endif

namespace tessera::server::example {

namespace {

// What the handlers use to find the host. A handler may only do what is safe in a signal handler: this reads
// an atomic and `RequestStop` writes one, and nothing else happens in it (no log, no allocation, no lock).
std::atomic<ServerHost*> g_host{nullptr};

void RequestStop() noexcept {
    if (ServerHost* host = g_host.load(std::memory_order_acquire)) {
        host->RequestStop();
    }
}

#ifdef _WIN32

BOOL WINAPI ConsoleHandler(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        RequestStop();
        return TRUE;
    case CTRL_CLOSE_EVENT:
        RequestStop();
        // Windows ends the process when this handler returns, or after a few seconds. Waiting here gives the
        // server the time to shut down and leave on its own.
        Sleep(4000);
        return TRUE;
    default:
        return FALSE;
    }
}

#else

void SignalHandler(int) {
    RequestStop();
}

#endif

} // namespace

void InstallStopSignals(ServerHost& host) {
    g_host.store(&host, std::memory_order_release);
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
    struct sigaction action {};
    action.sa_handler = SignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif
}

void RemoveStopSignals() noexcept {
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
#else
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif
    g_host.store(nullptr, std::memory_order_release);
}

} // namespace tessera::server::example
