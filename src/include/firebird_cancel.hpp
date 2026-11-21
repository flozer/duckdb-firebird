#pragma once

#include <memory>

#include "duckdb.hpp"

namespace duckdb {

class FirebirdConnection;

// Cancels the Firebird request running on a connection when the DuckDB
// query is interrupted (Ctrl+C, Connection::Interrupt, a client's
// interrupt()). DuckDB only checks its interrupt flag between chunks, and a
// single Firebird call (a cursor open that sorts, a fetch of an aggregate)
// can block in the client library for minutes.
//
// While a guard lives, one shared watcher thread checks the guard's context
// every 100 ms; once the context is interrupted it calls
// FirebirdConnection::CancelOperation (fb_cancel_operation with
// fb_cancel_raise). The blocked call then fails with Firebird's "operation
// was cancelled", and Cancelled() tells the caller to raise DuckDB's
// InterruptException instead. A raise can go unanswered: a caller sending
// one short request after another was seen waiting on the server's reply
// for good after one (about 1 interrupt in 50, on Firebird 3, 4 and 5).
// So if the guard is still there 1 s after the raise, the watcher aborts
// the attachment (fb_cancel_abort: the client closes its network
// connection, the blocked call fails, the server rolls back). The guard's
// destructor waits for a cancel in flight on its connection, so a guard
// must not outlive the connection it watches.
class FirebirdCancelGuard {
public:
    FirebirdCancelGuard(ClientContext &context, FirebirdConnection *conn);
    ~FirebirdCancelGuard();

    FirebirdCancelGuard(const FirebirdCancelGuard &) = delete;
    FirebirdCancelGuard &operator=(const FirebirdCancelGuard &) = delete;

    // The watcher cancelled the request: rethrow as InterruptException.
    bool Cancelled() const;

    struct Entry;

private:
    std::shared_ptr<Entry> entry_;
};

// From a catch block around guarded Firebird calls: an InterruptException
// when the guard cancelled the request, nothing otherwise.
void ThrowIfFirebirdCancelled(const FirebirdCancelGuard &guard);

} // namespace duckdb
