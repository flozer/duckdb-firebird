// Firebird ATTACH support.
//
// Hooks four classes into DuckDB's catalog framework:
//
//   FirebirdCatalog            : Catalog
//   FirebirdSchemaEntry        : SchemaCatalogEntry
//   FirebirdTableEntry         : TableCatalogEntry
//   FirebirdTransactionManager : TransactionManager
//
// Plus a `StorageExtension` that wires them together. The whole thing is
// read-only — every CreateXxx / DropEntry / Alter throws NotImplemented,
// the TransactionManager is a no-op (we run the Firebird-side
// transactions per-statement inside the scanner), and the catalog
// surfaces a single hard-coded "main" schema.
//
// Discovery is lazy: tables are listed on the first ScanSchemas /
// LookupEntry call (RDB$RELATIONS), and individual table column lists
// are filled when GetScanFunction is hit (RDB$RELATION_FIELDS).
//
// Re-uses the firebird_scan() table function: when GetScanFunction is
// called we build a fully-populated FirebirdBindData up front (schema +
// PK probe), then return the same TableFunction object the
// standalone-call path registers. The planner uses our pre-built bind
// data verbatim and never re-invokes the bind callback.

#include "firebird_storage.hpp"
#include "firebird_dbt_sources.hpp"
#include "firebird_scanner.hpp"
#include "firebird_client.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

static const char *const FIREBIRD_MAIN_SCHEMA = "main";

// Uppercase helper — Firebird identifiers are upper unless quoted at create
// time. We store our internal cache keys in uppercase and look up case-
// insensitively so `fb.main.employee`, `fb.main.EMPLOYEE`, and
// `fb.main."EMPLOYEE"` all resolve.
static std::string ToUpper(const std::string &s) {
    std::string out = s;
    for (auto &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// ---------------------------------------------------------------------------
//  FirebirdTableEntry
// ---------------------------------------------------------------------------

class FirebirdTableEntry final : public TableCatalogEntry {
public:
    FirebirdTableEntry(Catalog &catalog,
                       SchemaCatalogEntry &schema,
                       CreateTableInfo &info,
                       FirebirdConnectionInfo conn_info,
                       std::shared_ptr<FirebirdConnectionPool> pool,
                       NoneEncoding none_encoding,
                       duckdb::vector<FirebirdColumnDesc> column_descs)
        : TableCatalogEntry(catalog, schema, info),
          conn_info_(std::move(conn_info)),
          pool_(std::move(pool)),
          none_encoding_(none_encoding),
          cached_column_descs_(std::move(column_descs)) {
        // Mirror the column list out of CreateTableInfo so we can hand
        // a fresh copy to every GetScanFunction call without re-reading
        // RDB$RELATION_FIELDS. EnsureTablesLoaded in the schema entry
        // already paid the round-trip once at attach time.
        for (auto &col : columns.Logical()) {
            cached_column_names_.push_back(col.Name());
            cached_column_types_.push_back(col.Type());
        }
    }

    unique_ptr<BaseStatistics>
    GetStatistics(ClientContext & /*ctx*/, column_t /*column_id*/) override {
        return nullptr;            // no stats — DuckDB falls back to defaults
    }

    TableStorageInfo GetStorageInfo(ClientContext & /*ctx*/) override {
        return TableStorageInfo();
    }

    TableFunction GetScanFunction(ClientContext & /*ctx*/,
                                  unique_ptr<FunctionData> &bind_data) override {
        auto data = make_uniq<FirebirdBindData>();
        data->conn_info = conn_info_;
        data->table_name = name;
        data->column_names = cached_column_names_;   // O(1) — cached at construction
        data->column_types = cached_column_types_;
        data->column_descs = cached_column_descs_;
        data->none_encoding = none_encoding_;
        data->pool = pool_;

        // PK probe — lazy + memoised. The first scan against a table
        // pays the three RDB$ round-trips; subsequent scans return the
        // cached PrimaryKeyInfo (or nullopt) for free. The probe uses
        // the pool so we don't pay the connect cost on top.
        {
            std::lock_guard<std::mutex> g(pk_lock_);
            if (!pk_loaded_) {
                auto conn = pool_ ? pool_->Acquire()
                                  : make_uniq<FirebirdConnection>(conn_info_);
                pk_cache_ = ProbePrimaryKey(*conn, name,
                                            data->column_names, data->column_types);
                if (pool_) pool_->Release(std::move(conn));
                pk_loaded_ = true;
            }
            if (pk_cache_) {
                data->pk = make_uniq<PrimaryKeyInfo>(*pk_cache_);
            }
        }

        bind_data = std::move(data);
        return GetFirebirdScanFunction();
    }

private:
    FirebirdConnectionInfo conn_info_;
    std::shared_ptr<FirebirdConnectionPool> pool_;
    NoneEncoding none_encoding_;
    duckdb::vector<FirebirdColumnDesc> cached_column_descs_;
    duckdb::vector<std::string> cached_column_names_;
    duckdb::vector<LogicalType> cached_column_types_;

    std::mutex pk_lock_;
    bool pk_loaded_ = false;
    std::unique_ptr<PrimaryKeyInfo> pk_cache_;
};

// ---------------------------------------------------------------------------
//  FirebirdSchemaEntry
// ---------------------------------------------------------------------------

class FirebirdSchemaEntry final : public SchemaCatalogEntry {
public:
    FirebirdSchemaEntry(Catalog &catalog,
                        CreateSchemaInfo &info,
                        FirebirdConnectionInfo conn_info,
                        std::shared_ptr<FirebirdConnectionPool> pool,
                        NoneEncoding none_encoding)
        : SchemaCatalogEntry(catalog, info),
          conn_info_(std::move(conn_info)),
          pool_(std::move(pool)),
          none_encoding_(none_encoding) {}

    // -- discovery (the only non-stub members) -------------------------------

    void Scan(ClientContext & /*ctx*/, CatalogType type,
              const std::function<void(CatalogEntry &)> &callback) override {
        if (type != CatalogType::TABLE_ENTRY) return;
        EnsureTablesLoaded();
        for (auto &kv : tables_) {
            callback(*kv.second);
        }
    }

    void Scan(CatalogType type,
              const std::function<void(CatalogEntry &)> &callback) override {
        if (type != CatalogType::TABLE_ENTRY) return;
        EnsureTablesLoaded();
        for (auto &kv : tables_) {
            callback(*kv.second);
        }
    }

    optional_ptr<CatalogEntry>
    LookupEntry(CatalogTransaction /*transaction*/,
                const EntryLookupInfo &lookup_info) override {
        if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
            return nullptr;
        }
        EnsureTablesLoaded();
        auto key = ToUpper(lookup_info.GetEntryName());
        auto it = tables_.find(key);
        if (it == tables_.end()) return nullptr;
        return it->second.get();
    }

    // -- mutating ops — all throw, this is a read-only catalog ---------------

    optional_ptr<CatalogEntry>
    CreateFunction(CatalogTransaction, CreateFunctionInfo &) override   { Unsupported("CREATE FUNCTION"); }
    optional_ptr<CatalogEntry>
    CreateTable(CatalogTransaction, BoundCreateTableInfo &) override    { Unsupported("CREATE TABLE"); }
    optional_ptr<CatalogEntry>
    CreateView(CatalogTransaction, CreateViewInfo &) override           { Unsupported("CREATE VIEW"); }
    optional_ptr<CatalogEntry>
    CreateSequence(CatalogTransaction, CreateSequenceInfo &) override   { Unsupported("CREATE SEQUENCE"); }
    optional_ptr<CatalogEntry>
    CreateTableFunction(CatalogTransaction,
                        CreateTableFunctionInfo &) override             { Unsupported("CREATE TABLE FUNCTION"); }
    optional_ptr<CatalogEntry>
    CreateCopyFunction(CatalogTransaction,
                       CreateCopyFunctionInfo &) override               { Unsupported("CREATE COPY FUNCTION"); }
    optional_ptr<CatalogEntry>
    CreatePragmaFunction(CatalogTransaction,
                         CreatePragmaFunctionInfo &) override           { Unsupported("CREATE PRAGMA FUNCTION"); }
    optional_ptr<CatalogEntry>
    CreateCollation(CatalogTransaction, CreateCollationInfo &) override { Unsupported("CREATE COLLATION"); }
    optional_ptr<CatalogEntry>
    CreateType(CatalogTransaction, CreateTypeInfo &) override           { Unsupported("CREATE TYPE"); }
    optional_ptr<CatalogEntry>
    CreateIndex(CatalogTransaction, CreateIndexInfo &,
                TableCatalogEntry &) override                           { Unsupported("CREATE INDEX"); }

    void DropEntry(ClientContext &, DropInfo &) override { Unsupported("DROP"); }
    void Alter(CatalogTransaction, AlterInfo &) override { Unsupported("ALTER"); }

private:
    // Holds the columns belonging to one PK or UNIQUE constraint.
    struct UniqueKey {
        std::string constraint_name;
        bool        is_primary = false;
        duckdb::vector<std::string> columns; // ordered by RDB$FIELD_POSITION
    };

    // Load every PRIMARY KEY and UNIQUE constraint for all user tables in one
    // round-trip, returning a map from relation name -> list of UniqueKey.
    static std::unordered_map<std::string, std::vector<UniqueKey>>
    LoadUniqueConstraints(FirebirdConnection &conn) {
        const std::string sql =
            "SELECT TRIM(rc.RDB$RELATION_NAME), TRIM(rc.RDB$CONSTRAINT_NAME), "
            "       TRIM(rc.RDB$CONSTRAINT_TYPE), TRIM(seg.RDB$FIELD_NAME), "
            "       seg.RDB$FIELD_POSITION "
            "  FROM RDB$RELATION_CONSTRAINTS rc "
            "  JOIN RDB$INDEX_SEGMENTS seg ON seg.RDB$INDEX_NAME = rc.RDB$INDEX_NAME "
            " WHERE rc.RDB$CONSTRAINT_TYPE IN ('PRIMARY KEY','UNIQUE') "
            " ORDER BY rc.RDB$RELATION_NAME, rc.RDB$CONSTRAINT_NAME, seg.RDB$FIELD_POSITION";

        std::unordered_map<std::string, std::vector<UniqueKey>> out;
        auto cur = conn.OpenCursor(sql);
        UniqueKey *active = nullptr;
        std::string cur_rel, cur_con;
        while (cur->Fetch()) {
            std::string rel   = cur->GetText(0);
            std::string con   = cur->GetText(1);
            std::string ctype = cur->GetText(2);
            if (rel != cur_rel || con != cur_con) {
                out[rel].push_back({con, ctype == "PRIMARY KEY", {}});
                active  = &out[rel].back();
                cur_rel = rel;
                cur_con = con;
            }
            active->columns.push_back(cur->GetText(3));
        }
        return out;
    }

    // One FK relationship from a child table to its referenced parent.
    struct ForeignKey {
        std::string constraint_name;
        std::string pk_table;
        duckdb::vector<std::string> fk_columns; // ordered by RDB$FIELD_POSITION
        duckdb::vector<std::string> pk_columns; // same ordinal as fk_columns
    };

    // Load every FOREIGN KEY constraint for all user tables in one round-trip,
    // returning a map from child relation name -> list of ForeignKey.
    static std::unordered_map<std::string, std::vector<ForeignKey>>
    LoadForeignKeys(FirebirdConnection &conn) {
        const std::string sql =
            "SELECT TRIM(rc.RDB$RELATION_NAME), TRIM(rc.RDB$CONSTRAINT_NAME), "
            "       TRIM(uq.RDB$RELATION_NAME), TRIM(fkseg.RDB$FIELD_NAME), "
            "       TRIM(uqseg.RDB$FIELD_NAME), fkseg.RDB$FIELD_POSITION "
            "  FROM RDB$RELATION_CONSTRAINTS rc "
            "  JOIN RDB$REF_CONSTRAINTS ref ON ref.RDB$CONSTRAINT_NAME = rc.RDB$CONSTRAINT_NAME "
            "  JOIN RDB$RELATION_CONSTRAINTS uq ON uq.RDB$CONSTRAINT_NAME = ref.RDB$CONST_NAME_UQ "
            "  JOIN RDB$INDEX_SEGMENTS fkseg ON fkseg.RDB$INDEX_NAME = rc.RDB$INDEX_NAME "
            "  JOIN RDB$INDEX_SEGMENTS uqseg ON uqseg.RDB$INDEX_NAME = uq.RDB$INDEX_NAME "
            "       AND uqseg.RDB$FIELD_POSITION = fkseg.RDB$FIELD_POSITION "
            " WHERE rc.RDB$CONSTRAINT_TYPE = 'FOREIGN KEY' "
            " ORDER BY rc.RDB$RELATION_NAME, rc.RDB$CONSTRAINT_NAME, fkseg.RDB$FIELD_POSITION";

        std::unordered_map<std::string, std::vector<ForeignKey>> out;
        auto cur = conn.OpenCursor(sql);
        std::string cur_rel, cur_con;
        ForeignKey *active = nullptr;
        while (cur->Fetch()) {
            std::string rel = cur->GetText(0);
            std::string con = cur->GetText(1);
            if (!active || rel != cur_rel || con != cur_con) {
                out[rel].push_back(ForeignKey{con, cur->GetText(2), {}, {}});
                active = &out[rel].back();
                cur_rel = rel;
                cur_con = con;
            }
            active->fk_columns.push_back(cur->GetText(3));
            active->pk_columns.push_back(cur->GetText(4));
        }
        return out;
    }

    void EnsureTablesLoaded() {
        std::lock_guard<std::mutex> g(load_lock_);
        if (loaded_) return;

        // One connection covers all the schema-discovery queries; return
        // it to the pool at the end so the first user query can re-use
        // it warm.
        auto conn_owned = pool_ ? pool_->Acquire()
                                : make_uniq<FirebirdConnection>(conn_info_);
        FirebirdConnection &conn = *conn_owned;

        // Load PRIMARY KEY and UNIQUE constraints once for all tables.
        // A failure here leaves the map empty — constraints are simply not
        // attached, which matches the existing fallback philosophy (an error
        // in a metadata side-query must not break ATTACH).
        std::unordered_map<std::string, std::vector<UniqueKey>> unique_keys;
        try {
            unique_keys = LoadUniqueConstraints(conn);
        } catch (std::exception &e) {
            // Leave unique_keys empty — PK/UNIQUE constraints will not appear
            // in information_schema but the catalog is otherwise usable.
            Printer::Print(StringUtil::Format(
                "firebird: failed to load PK/UNIQUE constraints "
                "(information_schema will be incomplete): %s", e.what()));
        }

        // Load FOREIGN KEY constraints. Same fallback: failure leaves the map
        // empty and the catalog is still usable — only FK metadata is absent.
        std::unordered_map<std::string, std::vector<ForeignKey>> fkeys;
        try {
            fkeys = LoadForeignKeys(conn);
        } catch (std::exception &e) {
            // Leave fkeys empty — FK constraints will simply not appear in
            // information_schema but the catalog remains usable.
            Printer::Print(StringUtil::Format(
                "firebird: failed to load FK constraints "
                "(information_schema will be incomplete): %s", e.what()));
        }

        // Build + register one catalog entry from a resolved column list.
        // The binder asks the TableCatalogEntry for its full column list
        // before it ever calls GetScanFunction, so the real schema has to
        // be materialised here. Shared by both the batch path and the
        // per-table fallback below.
        auto add_entry = [&](const std::string &table_name,
                             const duckdb::vector<std::string> &col_names,
                             const duckdb::vector<LogicalType> &col_types,
                             duckdb::vector<FirebirdColumnDesc> col_descs) {
            if (col_names.empty()) return;
            CreateTableInfo info(catalog.GetName(), this->name, table_name);
            for (size_t i = 0; i < col_names.size(); ++i) {
                info.columns.AddColumn(ColumnDefinition(col_names[i], col_types[i]));
                // Firebird NOT NULL → DuckDB NotNullConstraint, so
                // information_schema.columns.is_nullable reports 'NO' for
                // the real NOT NULL columns. LogicalIndex is the column's
                // position in this CreateTableInfo, matching our order.
                if (i < col_descs.size() && !col_descs[i].nullable) {
                    info.constraints.push_back(
                        make_uniq<NotNullConstraint>(LogicalIndex(i)));
                }
            }
            // Attach PK / UNIQUE constraints so DuckDB derives
            // information_schema.table_constraints and key_column_usage.
            auto uk_it = unique_keys.find(table_name);
            if (uk_it != unique_keys.end()) {
                for (auto &key : uk_it->second) {
                    info.constraints.push_back(
                        make_uniq<UniqueConstraint>(key.columns, key.is_primary));
                }
            }
            // Attach FOREIGN KEY constraints. DuckDB accepts
            // ForeignKeyConstraint on read-only catalog entries as a
            // declarative annotation; it surfaces FK rows in
            // information_schema.table_constraints and key_column_usage
            // without enforcing referential integrity.
            auto fk_it = fkeys.find(table_name);
            if (fk_it != fkeys.end()) {
                for (auto &fk : fk_it->second) {
                    ForeignKeyInfo fk_info;
                    fk_info.type   = ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE;
                    fk_info.schema = FIREBIRD_MAIN_SCHEMA;
                    fk_info.table  = fk.pk_table;
                    info.constraints.push_back(make_uniq<ForeignKeyConstraint>(
                        fk.pk_columns, fk.fk_columns, std::move(fk_info)));
                }
            }
            auto entry = make_uniq<FirebirdTableEntry>(
                catalog, *this, info, conn_info_, pool_,
                none_encoding_, std::move(col_descs));
            tables_.emplace(ToUpper(table_name), std::move(entry));
        };

        // Fast path: a single query loads every user table's columns at once
        // (LoadAllTableSchemas). This collapses what used to be one
        // RDB$RELATION_FIELDS round-trip per table into a single round-trip —
        // the dominant cost when ATTACHing a large schema over a high-latency
        // link (e.g. ~25s for 2789 tables on a 3ms LAN; minutes over a WAN).
        // See docs/en/release_v0.6.1_plan.md.
        bool batch_ok = false;
        std::string batch_error;
        try {
            auto schemas = LoadAllTableSchemas(conn, none_encoding_);
            for (auto &ts : schemas) {
                add_entry(ts.table_name, ts.names, ts.types,
                          std::move(ts.descs));
            }
            batch_ok = true;
        } catch (std::exception &e) {
            // Batch discovery failed. This can be a benign SQL/metadata
            // incompatibility (older Firebird, an unexpected RDB$ shape) — in
            // which case the per-table fallback below recovers — OR a real
            // connection / credential / permission failure that the fallback
            // cannot fix. Keep the original message so we can surface it
            // instead of masking the root cause behind a slower repeat.
            batch_error = e.what();
            tables_.clear();
        }

        if (!batch_ok) {
            // User-visible relations: filter system_flag, but include both
            // persistent tables (type 0 / NULL) AND views (type 1) AND
            // external tables (type 2) AND global temporaries (4, 5). The
            // only thing we deliberately drop is the in-memory MON$ virtual
            // snapshots (type 3) — those are best queried directly through
            // firebird_scan('SELECT * FROM MON$…') and don't belong in a
            // user-facing catalog.
            std::vector<std::string> table_names;
            try {
                auto cur = conn.OpenCursor(
                    "SELECT TRIM(r.RDB$RELATION_NAME) "
                    "  FROM RDB$RELATIONS r "
                    " WHERE r.RDB$SYSTEM_FLAG = 0 "
                    "   AND (r.RDB$RELATION_TYPE IS NULL OR r.RDB$RELATION_TYPE <> 3) "
                    " ORDER BY r.RDB$RELATION_NAME");
                while (cur->Fetch()) table_names.push_back(cur->GetText(0));
            } catch (std::exception &e) {
                // The fallback's own discovery query failed too — a genuine
                // connection / credential / permission problem, not a
                // batch-query quirk. Surface BOTH errors so the batch failure
                // can't hide behind a second identical one (a silent empty
                // catalog would otherwise be the only symptom).
                throw IOException(
                    "Firebird catalog discovery failed — batch load error: [" +
                    batch_error + "]; per-table fallback error: [" +
                    std::string(e.what()) + "]");
            }
            // One RDB$RELATION_FIELDS round-trip per table. Individual table
            // failures are downgraded to a skip so a single bad relation
            // doesn't poison the whole ATTACH.
            for (auto &table_name : table_names) {
                try {
                    duckdb::vector<std::string> col_names;
                    duckdb::vector<LogicalType> col_types;
                    duckdb::vector<FirebirdColumnDesc> col_descs;
                    LoadTableSchema(conn, table_name,
                                    col_names, col_types, col_descs,
                                    none_encoding_);
                    add_entry(table_name, col_names, col_types,
                              std::move(col_descs));
                } catch (std::exception &) {
                    // Skip — table will simply not appear in fb.main.
                }
            }
            // If the fallback found relations but loaded none of them, the
            // per-table failures are systemic (permission on
            // RDB$RELATION_FIELDS, a dead transaction) rather than isolated
            // bad tables. Don't hand back a silent empty catalog — surface
            // the original batch error, which is the most informative signal.
            if (!table_names.empty() && tables_.empty()) {
                throw IOException(
                    "Firebird catalog discovery loaded 0 of " +
                    std::to_string(table_names.size()) +
                    " relations after the batch path failed — likely a "
                    "systemic permission or connection problem rather than "
                    "per-table quirks. Batch load error: [" + batch_error + "]");
            }
        }

        if (pool_) pool_->Release(std::move(conn_owned));
        loaded_ = true;
    }

    [[noreturn]] static void Unsupported(const char *op) {
        throw NotImplementedException(
            std::string("Firebird ATTACH catalog is read-only — ") + op +
            " is not supported. Use a regular DuckDB schema for writes.");
    }

    FirebirdConnectionInfo conn_info_;
    std::shared_ptr<FirebirdConnectionPool> pool_;
    NoneEncoding none_encoding_;
    std::mutex load_lock_;
    bool loaded_ = false;
    std::unordered_map<std::string, std::unique_ptr<FirebirdTableEntry>> tables_;
};

// ---------------------------------------------------------------------------
//  FirebirdCatalog
// ---------------------------------------------------------------------------

// Forward declaration at namespace scope so the friend declaration inside
// FirebirdCatalog can name it as a return type. GCC requires the type to be
// visible here (MSVC was lenient and accepted a friend-only declaration,
// which masked this until the Linux/community build).
struct FirebirdPoolStatsRow;
FirebirdPoolStatsRow ReadFirebirdPoolStats(ClientContext &context,
                                           const string &catalog_name);

class FirebirdCatalog final : public Catalog {
public:
    FirebirdCatalog(AttachedDatabase &db, FirebirdConnectionInfo conn_info,
                    NoneEncoding none_encoding,
                    FirebirdConnectionPoolConfig pool_config = {})
        : Catalog(db),
          conn_info_(std::move(conn_info)),
          pool_(std::make_shared<FirebirdConnectionPool>(conn_info_, pool_config)),
          none_encoding_(none_encoding) {}

    // Allow the dbt-sources metadata-extraction helper to lease a
    // connection from this catalog's pool without exposing
    // conn_info_ publicly. Implementation lives at file scope just
    // below this class.
    friend FirebirdMetadataLease AcquireFirebirdCatalogLease(
        ClientContext &context, const string &catalog_name);

    // firebird_pool_stats('alias') reads this catalog's pool counters
    // without leasing a connection. Implementation just below the class.
    // (FirebirdPoolStatsRow / ReadFirebirdPoolStats are forward-declared at
    // namespace scope above so this friend names them portably under GCC.)
    friend FirebirdPoolStatsRow ReadFirebirdPoolStats(
        ClientContext &context, const string &catalog_name);

    string GetCatalogType() override { return "firebird"; }

    void Initialize(bool /*load_builtin*/) override {
        // One hard-coded "main" schema. Real schema discovery on Firebird
        // would require RDB$OWNER_NAME segmentation, which the legacy
        // OLTP catalogs we target don't actually populate meaningfully.
        CreateSchemaInfo info;
        info.schema = FIREBIRD_MAIN_SCHEMA;
        info.on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
        main_schema_ = make_uniq<FirebirdSchemaEntry>(*this, info, conn_info_, pool_, none_encoding_);
    }

    bool InMemory() override { return false; }

    string GetDBPath() override { return conn_info_.database; }

    DatabaseSize GetDatabaseSize(ClientContext & /*ctx*/) override {
        // We could surface MON$RECORD_STATS / RDB$PAGES totals here, but
        // the figures are mostly noise (page count != logical row count).
        // Returning a zero-initialised DatabaseSize keeps SHOW DATABASES
        // happy without lying.
        return DatabaseSize();
    }

    void ScanSchemas(ClientContext & /*ctx*/,
                     std::function<void(SchemaCatalogEntry &)> callback) override {
        if (main_schema_) callback(*main_schema_);
    }

    optional_ptr<SchemaCatalogEntry>
    LookupSchema(CatalogTransaction /*transaction*/,
                 const EntryLookupInfo &schema_lookup,
                 OnEntryNotFound if_not_found) override {
        const auto &name = schema_lookup.GetEntryName();
        if (name.empty() || name == FIREBIRD_MAIN_SCHEMA) {
            return main_schema_.get();
        }
        if (if_not_found == OnEntryNotFound::RETURN_NULL) return nullptr;
        throw BinderException("Schema '" + name +
                              "' not found in Firebird catalog (only 'main' is exposed)");
    }

    optional_ptr<CatalogEntry>
    CreateSchema(CatalogTransaction, CreateSchemaInfo &) override {
        throw NotImplementedException(
            "CREATE SCHEMA is not supported on a Firebird-attached catalog "
            "(read-only).");
    }

    void DropSchema(ClientContext &, DropInfo &) override {
        throw NotImplementedException(
            "DROP SCHEMA is not supported on a Firebird-attached catalog.");
    }

    // -- planner hooks: read-only → all throw --------------------------------
    PhysicalOperator &PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &,
                                        LogicalCreateTable &, PhysicalOperator &) override {
        throw NotImplementedException("CTAS not supported on Firebird catalog");
    }
    PhysicalOperator &PlanInsert(ClientContext &, PhysicalPlanGenerator &,
                                 LogicalInsert &, optional_ptr<PhysicalOperator>) override {
        throw NotImplementedException("INSERT not supported on Firebird catalog");
    }
    PhysicalOperator &PlanDelete(ClientContext &, PhysicalPlanGenerator &,
                                 LogicalDelete &, PhysicalOperator &) override {
        throw NotImplementedException("DELETE not supported on Firebird catalog");
    }
    PhysicalOperator &PlanUpdate(ClientContext &, PhysicalPlanGenerator &,
                                 LogicalUpdate &, PhysicalOperator &) override {
        throw NotImplementedException("UPDATE not supported on Firebird catalog");
    }
    PhysicalOperator &PlanMergeInto(ClientContext &, PhysicalPlanGenerator &,
                                    LogicalMergeInto &, PhysicalOperator &) override {
        throw NotImplementedException("MERGE not supported on Firebird catalog");
    }
    unique_ptr<LogicalOperator> BindCreateIndex(Binder &, CreateStatement &,
                                                TableCatalogEntry &,
                                                unique_ptr<LogicalOperator>) override {
        throw NotImplementedException("CREATE INDEX not supported on Firebird catalog");
    }
    unique_ptr<LogicalOperator> BindAlterAddIndex(Binder &, TableCatalogEntry &,
                                                  unique_ptr<LogicalOperator>,
                                                  unique_ptr<CreateIndexInfo>,
                                                  unique_ptr<AlterTableInfo>) override {
        throw NotImplementedException("ALTER ... ADD INDEX not supported on Firebird catalog");
    }

private:
    FirebirdConnectionInfo conn_info_;
    std::shared_ptr<FirebirdConnectionPool> pool_;
    NoneEncoding none_encoding_;
    std::unique_ptr<FirebirdSchemaEntry> main_schema_;
};

// ---------------------------------------------------------------------------
//  FirebirdTransactionManager
//
// Firebird-side transactions are owned by the connection inside the scan
// itself (read-only, no-auto-undo). The DuckDB-level transaction manager
// here only needs to expose a Transaction object — it never carries write
// state, and commit/rollback are no-ops.
// ---------------------------------------------------------------------------

class FirebirdTransaction final : public Transaction {
public:
    FirebirdTransaction(TransactionManager &manager, ClientContext &context)
        : Transaction(manager, context) {}
};

class FirebirdTransactionManager final : public TransactionManager {
public:
    explicit FirebirdTransactionManager(AttachedDatabase &db)
        : TransactionManager(db) {}

    Transaction &StartTransaction(ClientContext &context) override {
        auto tx = make_uniq<FirebirdTransaction>(*this, context);
        auto &ref = *tx;
        std::lock_guard<std::mutex> g(lock_);
        transactions_.emplace(&ref, std::move(tx));
        return ref;
    }

    ErrorData CommitTransaction(ClientContext &, Transaction &transaction) override {
        std::lock_guard<std::mutex> g(lock_);
        transactions_.erase(&transaction);
        return ErrorData();
    }

    void RollbackTransaction(Transaction &transaction) override {
        std::lock_guard<std::mutex> g(lock_);
        transactions_.erase(&transaction);
    }

    void Checkpoint(ClientContext &, bool /*force*/) override {
        // No-op: we don't own writable state.
    }

private:
    std::mutex lock_;
    std::unordered_map<Transaction *, unique_ptr<FirebirdTransaction>> transactions_;
};

// ---------------------------------------------------------------------------
//  StorageExtension wiring
// ---------------------------------------------------------------------------

// Parses the ATTACH path + (TYPE firebird, key=value, …) options into a
// FirebirdConnectionInfo and (optionally) a NoneEncoding choice.
static FirebirdConnectionInfo BuildConnectionInfo(const std::string &path,
                                                  AttachInfo &info,
                                                  NoneEncoding &out_none) {
    auto conn = FirebirdConnectionInfo::Parse(path);
    // Match the firebird_scan default — most NONE-storage databases we
    // see in the wild (Athenas-ERP, IBExpert exports, Delphi-era apps)
    // wrote through a Windows-1252 client.
    out_none = NoneEncoding::WIN1252;
    for (auto &kv : info.options) {
        const auto &key = kv.first;
        const auto &val = kv.second;
        if      (key == "user")     conn.user     = val.ToString();
        else if (key == "password") conn.password = val.ToString();
        else if (key == "charset")  conn.charset  = val.ToString();
        else if (key == "role")     conn.role     = val.ToString();
        else if (key == "dialect")  conn.dialect  = static_cast<int>(val.GetValue<int32_t>());
        else if (key == "none_encoding") out_none  = ParseNoneEncoding(val.ToString());
        // unrecognised keys (including "type") are ignored — DuckDB has
        // already routed us here based on TYPE firebird.
    }
    return conn;
}

// Read a single SET firebird_pool_* option from the current
// ClientContext. Missing or NULL -> use the supplied default. This
// keeps the StorageExtension safe to load even if a future host
// loader skipped the AddExtensionOption registrations.
static int64_t ReadPoolInt64Setting(ClientContext &ctx,
                                     const std::string &key,
                                     int64_t default_value) {
    Value v;
    if (!ctx.TryGetCurrentSetting(key, v)) return default_value;
    if (v.IsNull()) return default_value;
    return v.GetValue<int64_t>();
}

static bool ReadPoolBoolSetting(ClientContext &ctx,
                                 const std::string &key,
                                 bool default_value) {
    Value v;
    if (!ctx.TryGetCurrentSetting(key, v)) return default_value;
    if (v.IsNull()) return default_value;
    return v.GetValue<bool>();
}

static FirebirdConnectionPoolConfig BuildPoolConfig(ClientContext &ctx) {
    FirebirdConnectionPoolConfig cfg;
    cfg.enabled         = ReadPoolBoolSetting(ctx,  "firebird_pool_enabled",         cfg.enabled);
    cfg.max_size        = ReadPoolInt64Setting(ctx, "firebird_pool_max_size",        cfg.max_size);
    cfg.idle_timeout_ms = ReadPoolInt64Setting(ctx, "firebird_pool_idle_timeout_ms", cfg.idle_timeout_ms);

    if (cfg.max_size < 0) {
        throw BinderException(
            "firebird_pool_max_size must be >= 0 (0 = unlimited), got %lld",
            static_cast<long long>(cfg.max_size));
    }
    if (cfg.idle_timeout_ms < 0) {
        throw BinderException(
            "firebird_pool_idle_timeout_ms must be >= 0 (0 = no expiry), got %lld",
            static_cast<long long>(cfg.idle_timeout_ms));
    }
    return cfg;
}

static unique_ptr<Catalog>
FirebirdAttach(optional_ptr<StorageExtensionInfo> /*info*/,
               ClientContext &context,
               AttachedDatabase &db,
               const string & /*name*/,
               AttachInfo &attach_info,
               AttachOptions & /*options*/) {
    NoneEncoding none_encoding;
    auto conn = BuildConnectionInfo(attach_info.path, attach_info, none_encoding);
    auto pool_config = BuildPoolConfig(context);
    return make_uniq<FirebirdCatalog>(db, std::move(conn), none_encoding,
                                       pool_config);
}

// ---------------------------------------------------------------------------
//  Metadata-only lease used by firebird_generate_dbt_sources(...) and any
//  future RDB$-introspection table function. Implemented here so it can
//  see the private pool_/none_encoding_ members of FirebirdCatalog
//  without exposing them publicly. Declared in firebird_dbt_sources.hpp.
// ---------------------------------------------------------------------------

FirebirdMetadataLease AcquireFirebirdCatalogLease(ClientContext &context,
                                                   const string &catalog_name) {
    ValidateFirebirdAttachAlias(context, catalog_name);
    auto catalog_ptr = Catalog::GetCatalogEntry(context, catalog_name);
    // ValidateFirebirdAttachAlias already confirmed catalog_ptr != null
    // and GetCatalogType() == "firebird"; the Cast is safe.
    auto &fb_catalog = catalog_ptr->Cast<FirebirdCatalog>();

    FirebirdMetadataLease lease;
    lease.pool          = fb_catalog.pool_;
    lease.conn          = fb_catalog.pool_->Acquire();
    lease.none_encoding = fb_catalog.none_encoding_;
    return lease;
}

// ---------------------------------------------------------------------------
//  firebird_pool_stats('alias') — factual pool introspection (Phase 4 #4)
// ---------------------------------------------------------------------------
//
// Reads the connection-pool counters of one attached Firebird catalog. No
// new instrumentation: every value is something the pool already tracks
// (config snapshot + idle-queue size + lifetime counters). It does NOT
// lease a connection, so calling it never perturbs the pool it reports on.
//
// Single explicit alias argument, mirroring firebird_profile_table() and
// firebird_generate_dbt_sources(). Reuses ValidateFirebirdAttachAlias for
// the "exists + is Firebird" check and its actionable BinderException.

struct FirebirdPoolStatsRow {
    std::string catalog_name;
    bool        pool_enabled = false;
    int64_t     max_idle_size = 0;
    int64_t     idle_timeout_ms = 0;
    int64_t     idle_connections = 0;
    int64_t     total_created = 0;
    int64_t     total_reused = 0;
    int64_t     total_discarded = 0;
};

FirebirdPoolStatsRow ReadFirebirdPoolStats(ClientContext &context,
                                           const string &catalog_name) {
    ValidateFirebirdAttachAlias(context, catalog_name);
    auto catalog_ptr = Catalog::GetCatalogEntry(context, catalog_name);
    // Validated above: non-null and GetCatalogType() == "firebird".
    auto &fb_catalog = catalog_ptr->Cast<FirebirdCatalog>();
    auto &pool = *fb_catalog.pool_;
    const auto &cfg = pool.Config();

    FirebirdPoolStatsRow row;
    row.catalog_name     = catalog_name;
    row.pool_enabled     = cfg.enabled;
    row.max_idle_size    = cfg.max_size;
    row.idle_timeout_ms  = cfg.idle_timeout_ms;
    row.idle_connections = static_cast<int64_t>(pool.IdleCount());
    row.total_created    = pool.TotalCreated();
    row.total_reused     = pool.TotalReused();
    row.total_discarded  = pool.TotalDiscarded();
    return row;
}

namespace {

struct PoolStatsBindData : public TableFunctionData {
    std::string catalog_name;
};

struct PoolStatsGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override { return 1; }
};

unique_ptr<FunctionData> PoolStatsBind(ClientContext &context,
                                       TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types,
                                       vector<string> &names) {
    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException(
            "firebird_pool_stats(catalog_name VARCHAR): catalog_name is "
            "required (the alias from ATTACH '...' AS <alias> "
            "(TYPE firebird)).");
    }
    auto bind = make_uniq<PoolStatsBindData>();
    bind->catalog_name = input.inputs[0].ToString();
    // Resolve eagerly so a bad alias fails at bind time, not at execute.
    ValidateFirebirdAttachAlias(context, bind->catalog_name);

    names = {
        "catalog_name",
        "pool_enabled",
        "max_idle_size",
        "idle_timeout_ms",
        "idle_connections",
        "total_created",
        "total_reused",
        "total_discarded",
    };
    return_types = {
        LogicalType::VARCHAR,
        LogicalType::BOOLEAN,
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::BIGINT,
    };
    return std::move(bind);
}

unique_ptr<GlobalTableFunctionState>
PoolStatsInitGlobal(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<PoolStatsGlobalState>();
}

void PoolStatsFunction(ClientContext &context, TableFunctionInput &input,
                       DataChunk &output) {
    auto &g = input.global_state->Cast<PoolStatsGlobalState>();
    if (g.emitted) {
        output.SetCardinality(0);
        return;
    }
    auto &bind = input.bind_data->Cast<PoolStatsBindData>();
    FirebirdPoolStatsRow r = ReadFirebirdPoolStats(context, bind.catalog_name);

    output.SetCardinality(1);
    output.data[0].SetValue(0, Value(r.catalog_name));
    output.data[1].SetValue(0, Value::BOOLEAN(r.pool_enabled));
    output.data[2].SetValue(0, Value::BIGINT(r.max_idle_size));
    output.data[3].SetValue(0, Value::BIGINT(r.idle_timeout_ms));
    output.data[4].SetValue(0, Value::BIGINT(r.idle_connections));
    output.data[5].SetValue(0, Value::BIGINT(r.total_created));
    output.data[6].SetValue(0, Value::BIGINT(r.total_reused));
    output.data[7].SetValue(0, Value::BIGINT(r.total_discarded));
    g.emitted = true;
}

} // namespace

TableFunction GetFirebirdPoolStatsFunction() {
    TableFunction fn("firebird_pool_stats",
                     {LogicalType::VARCHAR},
                     PoolStatsFunction,
                     PoolStatsBind,
                     PoolStatsInitGlobal);
    return fn;
}

static unique_ptr<TransactionManager>
FirebirdCreateTransactionManager(optional_ptr<StorageExtensionInfo> /*info*/,
                                 AttachedDatabase &db, Catalog & /*catalog*/) {
    return make_uniq<FirebirdTransactionManager>(db);
}

unique_ptr<StorageExtension> GetFirebirdStorageExtension() {
    auto ext = make_uniq<StorageExtension>();
    ext->attach = FirebirdAttach;
    ext->create_transaction_manager = FirebirdCreateTransactionManager;
    return std::move(ext);
}

} // namespace duckdb
