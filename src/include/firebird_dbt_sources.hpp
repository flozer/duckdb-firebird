#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// Internal handle borrowed from a Firebird ATTACH catalog for one
// metadata-extraction call. Holds a strong reference to the catalog's
// pool plus an active connection lease; releasing the connection back
// to the pool is automatic via the destructor, so callers can throw
// freely during metadata SQL without leaking handles.
//
// Not part of the public extension API. Move-only.
struct FirebirdMetadataLease {
    std::shared_ptr<FirebirdConnectionPool> pool;
    std::unique_ptr<FirebirdConnection>     conn;
    NoneEncoding                            none_encoding = NoneEncoding::WIN1252;

    FirebirdMetadataLease() = default;
    FirebirdMetadataLease(FirebirdMetadataLease &&) noexcept = default;
    FirebirdMetadataLease &operator=(FirebirdMetadataLease &&) noexcept = default;
    FirebirdMetadataLease(const FirebirdMetadataLease &) = delete;
    FirebirdMetadataLease &operator=(const FirebirdMetadataLease &) = delete;

    ~FirebirdMetadataLease() {
        if (pool && conn) {
            pool->Release(std::move(conn));
        }
    }
};

// Internal helper: validate that `catalog_name` resolves through DuckDB's
// catalog manager AND that the resolved catalog is a Firebird ATTACH.
// Throws BinderException with an actionable message when the catalog
// does not exist OR when its GetCatalogType() is not "firebird".
void ValidateFirebirdAttachAlias(ClientContext &context,
                                 const std::string &catalog_name);

// Acquire a metadata-only lease against the FirebirdCatalog reachable
// through `catalog_name`. Validates the alias first (calls
// ValidateFirebirdAttachAlias internally), then leases a connection
// from the catalog's pool. The returned FirebirdMetadataLease releases
// the connection back when it goes out of scope - including on
// exception - so the caller can run arbitrary RDB$ queries through
// `lease.conn` without manual cleanup.
//
// The lease intentionally does NOT expose the FirebirdConnectionInfo;
// only the pool, the connection, and the catalog's NoneEncoding
// preference are surfaced. Keeps credentials inside the catalog.
FirebirdMetadataLease AcquireFirebirdCatalogLease(ClientContext &context,
                                                   const std::string &catalog_name);

// firebird_generate_dbt_sources(catalog_name VARCHAR)
//
// Emits a single VARCHAR row containing a dbt-compatible sources.yml
// derived from the Firebird catalog reachable through the given ATTACH
// alias. Pipe through COPY to write to disk:
//
//   COPY (SELECT yaml FROM firebird_generate_dbt_sources('fb')) TO 'sources.yml';
//
// The output never carries the Firebird connection string or any
// credential - only the catalog alias supplied by the caller, the
// schema name, relation names, column names, DuckDB-facing data_type
// strings, and dbt's `not_null` / `unique` tests on detected PKs.
TableFunction GetFirebirdDbtSourcesFunction();

} // namespace duckdb
