#pragma once

#include "ibase.h"

namespace duckdb {

// Function-pointer table for the legacy ISC entry-points the
// extension actually calls. Populated lazily on the first fbapi()
// call by dlopen/LoadLibrary against the platform's Firebird client
// library (or the path in DUCKDB_FIREBIRD_CLIENT_LIBRARY).
//
// Using decltype against the ibase.h declarations keeps every
// signature in lockstep with the vendored headers; if Firebird ever
// renames or changes the calling convention, compilation will fail
// at this struct definition rather than at runtime through the
// silent function pointer.
struct FirebirdClientApi {
    decltype(&::isc_attach_database)         isc_attach_database;
    decltype(&::isc_detach_database)         isc_detach_database;
    decltype(&::isc_start_transaction)       isc_start_transaction;
    decltype(&::isc_rollback_transaction)    isc_rollback_transaction;
    decltype(&::isc_dsql_allocate_statement) isc_dsql_allocate_statement;
    decltype(&::isc_dsql_prepare)            isc_dsql_prepare;
    decltype(&::isc_dsql_describe)           isc_dsql_describe;
    decltype(&::isc_dsql_describe_bind)      isc_dsql_describe_bind;
    decltype(&::isc_dsql_execute)            isc_dsql_execute;
    decltype(&::isc_dsql_fetch)              isc_dsql_fetch;
    decltype(&::isc_dsql_free_statement)     isc_dsql_free_statement;
    decltype(&::isc_open_blob2)              isc_open_blob2;
    decltype(&::isc_close_blob)              isc_close_blob;
    decltype(&::isc_get_segment)             isc_get_segment;
    decltype(&::isc_sqlcode)                 isc_sqlcode;
    decltype(&::fb_interpret)                fb_interpret;
};

// Resolve the Firebird client library (once per process) and return
// the populated function table. Throws IOException with an actionable
// hint when the library cannot be loaded - "Firebird client library
// not found. Install the Firebird client ... or set
// DUCKDB_FIREBIRD_CLIENT_LIBRARY=..."
//
// Thread-safe: the first caller pays the dlopen cost while holding
// an internal lock; subsequent calls return the cached table.
const FirebirdClientApi &fbapi();

} // namespace duckdb
