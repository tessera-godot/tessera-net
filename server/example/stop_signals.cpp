// SPDX-License-Identifier: Apache-2.0
#include "stop_signals.hpp"

#include "tessera/server/server_host.hpp"

#include <atomic>
#include <chrono>
#include <thread>

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

// All a handler does is set this flag, which is safe in a signal handler: one store to a lock-free atomic, with
// no log, no allocation, no lock and no call into the host. A thread of ours polls the flag and asks the host
// to stop from there.
std::atomic<bool> g_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the signal handler needs a lock-free flag");

std::atomic<bool> g_quit_watching{false};
std::thread g_watcher;

void RequestStop() noexcept {
    g_stop_requested.store(true, std::memory_order_release);
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

// Ends the polling thread, if there is one.
void StopWatching() noexcept {
    g_quit_watching.store(true, std::memory_order_release);
    if (g_watcher.joinable()) {
        g_watcher.join();
    }
}

} // namespace

void InstallStopSignals(ServerHost& host) {
    StopWatching(); // asking twice: the new host takes over
    g_stop_requested.store(false, std::memory_order_release);
    g_quit_watching.store(false, std::memory_order_release);
    g_watcher = std::thread([&host] {
        while (!g_quit_watching.load(std::memory_order_acquire)) {
            if (g_stop_requested.load(std::memory_order_acquire)) {
                host.RequestStop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
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
    // The handlers stay installed and only set the flag, which nobody reads any more: a signal that arrives
    // while the process is leaving must not fall back to the default action, which would end it with a
    // failure code.
    StopWatching();
}

} // namespace tessera::server::example
