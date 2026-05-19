#define DUCKDB_EXTENSION_MAIN

#include "firebird_extension.hpp"
#include "firebird_scanner.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension_util.hpp"

namespace duckdb {

static void LoadInternal(DatabaseInstance &db) {
    ExtensionUtil::RegisterFunction(db, GetFirebirdScanFunction());
}

void FirebirdExtension::Load(DuckDB &db) {
    LoadInternal(*db.instance);
}

std::string FirebirdExtension::Name() {
    return "firebird";
}

std::string FirebirdExtension::Version() const {
#ifdef EXT_VERSION_FIREBIRD
    return EXT_VERSION_FIREBIRD;
#else
    return "0.1.0";
#endif
}

} // namespace duckdb

// --- C entry points -----------------------------------------------------------
// DuckDB's loader calls these to instantiate the extension. Mirrors the
// pattern used by postgres_scanner / sqlite_scanner.
extern "C" {

DUCKDB_EXTENSION_API void firebird_init(duckdb::DatabaseInstance &db) {
    duckdb::LoadInternal(db);
}

DUCKDB_EXTENSION_API const char *firebird_version() {
    return duckdb::DuckDB::LibraryVersion();
}

} // extern "C"

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
