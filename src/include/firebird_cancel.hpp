#pragma once

#include <memory>
#include <string>

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
// was cancelled", which FirebirdConnection::CheckStatus throws as DuckDB's
// InterruptException. A raise can go unanswered: a caller sending one short
// request after another was seen waiting on the server's reply for good
// after one, on Firebird 3, 4 and 5. So if the guard is still there 1 s
// after the raise, the watcher aborts the attachment (fb_cancel_abort: the
// client closes its network connection, the blocked call fails, the server
// rolls back).
//
// Each cancel runs on a short-lived thread of its own, so a cancel that
// blocks never holds up the watcher or another connection's cancel. (In the
// client library a raise sends one small packet under the port's write
// lock, so it blocks only when the network send does; an abort closes the
// socket. The library runs the cancels of one attachment one at a time, so
// a raise that blocks also delays that attachment's abort.) The guard's
// destructor waits for a cancel in flight on its connection, so a guard
// must not outlive the connection it watches.
class FirebirdCancelGuard {
public:
    FirebirdCancelGuard(ClientContext &context, FirebirdConnection *conn);
    ~FirebirdCancelGuard();

    FirebirdCancelGuard(const FirebirdCancelGuard &) = delete;
    FirebirdCancelGuard &operator=(const FirebirdCancelGuard &) = delete;

    FirebirdConnection *Connection() const;

    struct Entry;

private:
    std::shared_ptr<Entry> entry_;
};

// After a guarded call that reports its error without throwing (for
// example TryStartSnapshotTransaction): DuckDB's InterruptException when
// the call failed because of the guard's cancel, nothing otherwise.
void ThrowIfFirebirdCancelled(const FirebirdCancelGuard &guard);

// The query log's error_message for a query that a cancel ended.
std::string FirebirdInterruptMessage(const FirebirdConnection &conn);

} // namespace duckdb
