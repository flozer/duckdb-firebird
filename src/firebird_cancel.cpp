// Cancelling a running Firebird request when the DuckDB query is
// interrupted. See firebird_cancel.hpp.

#include "firebird_cancel.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

#include "firebird_client.hpp"

namespace duckdb {

struct FirebirdCancelGuard::Entry {
    ClientContext *context = nullptr;
    FirebirdConnection *conn = nullptr;
    bool cancelled = false;  // under the watcher's lock
    bool aborted = false;    // under the watcher's lock
    std::chrono::steady_clock::time_point cancelled_at;
    // Held while a cancel runs; the guard's destructor takes it after
    // removing the entry, so no cancel runs once the destructor returns.
    std::mutex cancel_lock;
    bool removed = false;  // under cancel_lock
};

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(100);
// A guard still registered this long after the raise gets the abort.
constexpr auto kAbortAfter = std::chrono::milliseconds(1000);

// Test only: with DUCKDB_FIREBIRD_TEST_CANCEL_ABORT=1 in the environment,
// the first cancel is the abort, so the CI can exercise that path
// (scripts/check_cancel.py).
bool AbortFirst() {
    const char *v = std::getenv("DUCKDB_FIREBIRD_TEST_CANCEL_ABORT");
    return v && std::strcmp(v, "1") == 0;
}

void RunCancel(FirebirdCancelGuard::Entry &entry, bool with_abort) {
    std::lock_guard<std::mutex> c(entry.cancel_lock);
    if (entry.removed) {
        return;
    }
    try {
        entry.conn->CancelOperation(with_abort);
    } catch (...) {
    }
}

class CancelWatcher {
public:
    void Add(const std::shared_ptr<FirebirdCancelGuard::Entry> &entry) {
        std::lock_guard<std::mutex> g(lock_);
        if (!started_) {
            // Detached: the watcher lives as long as the process (see Get).
            std::thread([this] { Run(); }).detach();
            started_ = true;
        }
        entries_.push_back(entry);
        wake_.notify_one();
    }

    void Remove(const std::shared_ptr<FirebirdCancelGuard::Entry> &entry) {
        {
            std::lock_guard<std::mutex> g(lock_);
            entries_.erase(std::remove(entries_.begin(), entries_.end(), entry), entries_.end());
        }
        // Wait for a cancel in flight on this connection.
        std::lock_guard<std::mutex> c(entry->cancel_lock);
        entry->removed = true;
    }

    // Never destroyed: the detached thread may still run while static
    // objects are destroyed at process exit.
    static CancelWatcher &Get() {
        static CancelWatcher *watcher = new CancelWatcher();
        return *watcher;
    }

private:
    void Run() {
        while (true) {
            std::vector<std::pair<std::shared_ptr<FirebirdCancelGuard::Entry>, bool>> due;
            {
                std::unique_lock<std::mutex> g(lock_);
                while (entries_.empty()) {
                    wake_.wait(g);
                }
                const auto now = std::chrono::steady_clock::now();
                for (auto &entry : entries_) {
                    if (!entry->conn) {
                        continue;
                    }
                    if (!entry->cancelled) {
                        if (entry->context->IsInterrupted()) {
                            entry->cancelled = true;
                            entry->cancelled_at = now;
                            const bool with_abort = AbortFirst();
                            entry->aborted = with_abort;
                            due.emplace_back(entry, with_abort);
                        }
                    } else if (!entry->aborted && now - entry->cancelled_at >= kAbortAfter) {
                        entry->aborted = true;
                        due.emplace_back(entry, true);
                    }
                }
            }
            // Each cancel on its own short-lived thread, outside the
            // watcher's lock: one that blocks holds up neither this loop
            // (the next connection's raise, any connection's abort) nor
            // another guard. The thread keeps the entry alive; the guard's
            // destructor waits for it through cancel_lock.
            for (auto &item : due) {
                auto entry = item.first;
                const bool with_abort = item.second;
                try {
                    std::thread([entry, with_abort] { RunCancel(*entry, with_abort); }).detach();
                } catch (const std::system_error &) {
                    RunCancel(*entry, with_abort);  // no thread to be had: cancel here
                }
            }
            std::unique_lock<std::mutex> g(lock_);
            wake_.wait_for(g, kPollInterval);
        }
    }

    std::mutex lock_;
    std::condition_variable wake_;
    std::vector<std::shared_ptr<FirebirdCancelGuard::Entry>> entries_;
    bool started_ = false;
};

} // namespace

FirebirdCancelGuard::FirebirdCancelGuard(ClientContext &context, FirebirdConnection *conn)
    : entry_(std::make_shared<Entry>()) {
    entry_->context = &context;
    entry_->conn = conn;
    // What this guard reports is about the calls it watches only: a raise
    // an earlier guard sent and the server never answered must not turn a
    // later error on the same connection into the interrupt.
    if (conn) {
        conn->ResetCancelState();
    }
    CancelWatcher::Get().Add(entry_);
}

FirebirdCancelGuard::~FirebirdCancelGuard() {
    CancelWatcher::Get().Remove(entry_);
}

FirebirdConnection *FirebirdCancelGuard::Connection() const {
    return entry_->conn;
}

void ThrowIfFirebirdCancelled(const FirebirdCancelGuard &guard) {
    if (guard.Connection() && guard.Connection()->CancelAnswered()) {
        throw InterruptException();
    }
}

std::string FirebirdInterruptMessage(const FirebirdConnection &conn) {
    return conn.CancelAborted()
               ? "Interrupted: the Firebird connection was closed (fb_cancel_abort)"
               :"Interrupted: the Firebird request was cancelled (fb_cancel_raise)";
}

} // namespace duckdb
