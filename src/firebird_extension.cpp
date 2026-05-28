#define DUCKDB_EXTENSION_MAIN

#include "firebird_extension.hpp"
#include "firebird_observability.hpp"
#include "firebird_scanner.hpp"
#include "firebird_storage.hpp"

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
    loader.RegisterFunction(GetFirebirdScanFunction());
    loader.RegisterFunction(GetFirebirdTablesFunction());
    loader.RegisterFunction(GetFirebirdAttachFunction());
    loader.RegisterFunction(GetFirebirdLastQueryFunction());
    loader.RegisterFunction(GetFirebirdQueryLogFunction());

    // Register the StorageExtension so DuckDB knows how to handle
    //   ATTACH 'firebird://…' AS fb (TYPE firebird);
    //
    // The registration entry point varies a little across DuckDB versions:
    //   v1.4 had `config.storage_extensions[name] = unique_ptr<>;`
    //   v1.5+ exposes `StorageExtension::Register(config, name, shared_ptr<>)`
    //          (via DBConfig::GetCallbackManager — the field was made
    //          private and moved into the callback manager registry).
    auto &db = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(db);
    auto storage_ext = GetFirebirdStorageExtension();
    StorageExtension::Register(config, "firebird",
                               shared_ptr<StorageExtension>(storage_ext.release()));

    // Chunk E - firebird_query_log() ring buffer is opt-in. The default
    // (0) disables capture; users opt in per session via:
    //   SET firebird_query_log_size = 16;
    // Per-ClientContext storage (FirebirdObservabilityState) ensures one
    // session never reads another's ring buffer.
    config.AddExtensionOption(
        "firebird_query_log_size",
        "Maximum entries kept by firebird_query_log() per session. "
        "0 disables the log (default).",
        LogicalType::BIGINT,
        Value::BIGINT(0));
}

void FirebirdExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
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
// DuckDB ≥ 1.4 calls the *_duckdb_cpp_init symbol declared by this macro.
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(firebird, loader) {
    duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *firebird_version() {
    return duckdb::DuckDB::LibraryVersion();
}

} // extern "C"

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
