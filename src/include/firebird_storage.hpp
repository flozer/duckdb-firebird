#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

// Returns a fresh StorageExtension instance to register with DuckDB's
// extension loader. Hooking this up makes
//
//     ATTACH 'firebird://SYSDBA:masterkey@host/path/db.fdb' AS fb (TYPE firebird);
//     SELECT * FROM fb.main.EMPLOYEE WHERE …;
//
// work as a federated read-only catalog.
unique_ptr<StorageExtension> GetFirebirdStorageExtension();

// firebird_pool_stats(catalog_name VARCHAR) — factual connection-pool
// introspection for one attached Firebird catalog. Emits a single row of
// pool config + idle-queue size + lifetime counters. Reads only what the
// pool already tracks; never leases a connection.
TableFunction GetFirebirdPoolStatsFunction();

// One Firebird snapshot shared by every scan of an ATTACH in one DuckDB
// transaction (opt-in: ATTACH option transaction_snapshot, else
// SET firebird_transaction_snapshot). Held by the ATTACH's DuckDB-side
// transaction on a coordinating connection from the ATTACH's pool; every
// scan of the transaction joins it with isc_tpb_at_snapshot_number, the
// same way the workers of one partitioned scan do (#94).
class FirebirdTransactionSnapshot {
public:
    virtual ~FirebirdTransactionSnapshot() = default;
    // Pins the snapshot on the first call and returns its number, or 0
    // on Firebird 3 (no snapshot numbers): the scan then behaves as
    // without this option. Throws an IOException when the snapshot can't
    // be started, or was lost (later calls); never returns 0 after a
    // failure. idle_timeout_s > 0 (read on the first call) makes the
    // server release the snapshot when that many seconds pass from the
    // end of a statement to the next call.
    virtual int64_t Pin(ClientContext &context, int64_t idle_timeout_s) = 0;
    // Called by a scan worker right after it joined the snapshot number:
    // throws an IOException unless the coordinator still holds it. A
    // coordinator that is still alive means the database never closed in
    // between, so the number the worker joined was this snapshot's
    // (Firebird restarts the numbers when a database is reopened).
    virtual void CheckHeld() = 0;
};

// Validates firebird_transaction_snapshot_idle_timeout: 0 (no limit) up to
// 2147483646 seconds (the server's limit is 2^31 - 1, and one second is
// added for its whole-second timer). Throws a BinderException otherwise.
void ValidateTransactionSnapshotIdleTimeout(int64_t seconds);

// The snapshot of the current DuckDB transaction on this Firebird ATTACH
// when transaction snapshots are enabled for it (the ATTACH option wins
// over the session setting), nullptr otherwise.
FirebirdTransactionSnapshot *GetFirebirdTransactionSnapshot(ClientContext &context,
                                                            Catalog &catalog);

} // namespace duckdb
