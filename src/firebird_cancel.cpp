// Cancelling a running Firebird request when the DuckDB query is
// interrupted. See firebird_cancel.hpp.

#include "firebird_cancel.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
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
    // Held while the watcher cancels; the guard's destructor takes it after
    // removing the entry, so no cancel runs once the destructor returns.
    std::mutex cancel_lock;
    bool removed = false;  // under cancel_lock
};

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(100);
// A guard still registered this long after the raise gets the abort.
constexpr auto kAbortAfter = std::chrono::milliseconds(1000);

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

    bool Cancelled(const FirebirdCancelGuard::Entry &entry) {
        std::lock_guard<std::mutex> g(lock_);
        return entry.cancelled;
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
                            due.emplace_back(entry, false);
                        }
                    } else if (!entry->aborted && now - entry->cancelled_at >= kAbortAfter) {
                        entry->aborted = true;
                        due.emplace_back(entry, true);
                    }
                }
            }
            // Outside the watcher's lock: a slow cancel doesn't hold up guards
            // of other connections.
            for (auto &item : due) {
                auto &entry = item.first;
                std::lock_guard<std::mutex> c(entry->cancel_lock);
                if (!entry->removed) {
                    try {
                        entry->conn->CancelOperation(item.second);
                    } catch (...) {
                    }
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
    CancelWatcher::Get().Add(entry_);
}

FirebirdCancelGuard::~FirebirdCancelGuard() {
    CancelWatcher::Get().Remove(entry_);
}

bool FirebirdCancelGuard::Cancelled() const {
    return CancelWatcher::Get().Cancelled(*entry_);
}

void ThrowIfFirebirdCancelled(const FirebirdCancelGuard &guard) {
    if (guard.Cancelled()) {
        throw InterruptException();
    }
}

} // namespace duckdb
