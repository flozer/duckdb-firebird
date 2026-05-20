#pragma once

#include "duckdb.hpp"
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

} // namespace duckdb
