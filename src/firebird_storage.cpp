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
// the Firebird-side transactions run per scan inside the scanner (the
// DuckDB-side transaction only holds the optional transaction snapshot,
// see FirebirdTransaction), and the catalog surfaces a single
// hard-coded "main" schema.
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
#include "firebird_query_function.hpp"
#include "firebird_dbt_sources.hpp"
#include "firebird_scanner.hpp"
#include "firebird_client.hpp"
#include "firebird_view_analysis.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <unordered_map>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/logical_operator.hpp"
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
                       bool none_pushdown,
                       bool numeric_widen_int64,
                       duckdb::vector<FirebirdColumnDesc> column_descs,
                       PrimaryKeyDescriptor pk_descriptor)
        : TableCatalogEntry(catalog, schema, info),
          conn_info_(std::move(conn_info)),
          pool_(std::move(pool)),
          none_encoding_(none_encoding),
          none_pushdown_(none_pushdown),
          numeric_widen_int64_(numeric_widen_int64),
          cached_column_descs_(std::move(column_descs)),
          pk_descriptor_(std::move(pk_descriptor))
#ifdef FIREBIRD_DUCKDB_V2
          // v2.0: TableCatalogEntry no longer stores the column list —
          // GetColumns() is pure virtual, so we own a copy of info.columns.
          ,
          columns_(info.columns.Copy())
#endif
    {
        // Mirror the column list out of CreateTableInfo so we can hand
        // a fresh copy to every GetScanFunction call without re-reading
        // RDB$RELATION_FIELDS. EnsureTablesLoaded in the schema entry
        // already paid the round-trip once at attach time.
#ifdef FIREBIRD_DUCKDB_V2
        for (auto &col : columns_.Logical()) {
            // v2.0: ColumnDefinition::Name() returns an Identifier.
            cached_column_names_.push_back(col.Name().GetIdentifierName());
#else
        for (auto &col : columns.Logical()) {
            cached_column_names_.push_back(col.Name());
#endif
            cached_column_types_.push_back(col.Type());
        }
    }

#ifdef FIREBIRD_DUCKDB_V2
    const ColumnList &GetColumns() const override {
        return columns_;
    }
#endif

    const PrimaryKeyDescriptor &GetPkDescriptor() const { return pk_descriptor_; }

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
#ifdef FIREBIRD_DUCKDB_V2
        data->table_name = name.GetIdentifierName();  // v2.0: name is Identifier
#else
        data->table_name = name;
#endif
        data->column_names = cached_column_names_;   // O(1) — cached at construction
        data->column_types = cached_column_types_;
        data->column_descs = cached_column_descs_;
        data->none_encoding = none_encoding_;
        data->none_pushdown = none_pushdown_;
        data->numeric_widen_int64 = numeric_widen_int64_;
        data->pool = pool_;
        data->attach_catalog = &ParentCatalog();
        data->pk_descriptor = pk_descriptor_; // cached at ATTACH, zero I/O

        // View column types/descriptors are already correct here — they
        // were reconciled in FirebirdSchemaEntry::EnsureTablesLoaded's
        // add_entry lambda, against local mutable copies, BEFORE this
        // (shared, published) TableCatalogEntry was ever constructed. So
        // this method only has to serve the PK-probe cache: lazy +
        // memoised under pk_lock_, the first scan against a table pays
        // the round trip, subsequent scans (and concurrent scans, once
        // the lock is released) reuse the cached result for free.
        {
            std::lock_guard<std::mutex> g(pk_lock_);
            if (!pk_loaded_) {
                auto conn = pool_ ? pool_->Acquire()
                                  : make_uniq<FirebirdConnection>(conn_info_);
#ifdef FIREBIRD_DUCKDB_V2
                pk_cache_ = ProbePrimaryKey(*conn, name.GetIdentifierName(),
#else
                pk_cache_ = ProbePrimaryKey(*conn, name,
#endif

                                            data->column_names, data->column_types);
                // ProbePrimaryKey swallows its errors and returns null. If
                // the connection broke, null doesn't mean "no usable PK", so
                // don't memoise it: the next scan probes again. Release()
                // discards the broken connection.
                const bool probe_ok = !conn->IsBroken();
                if (pool_) pool_->Release(std::move(conn));
                pk_loaded_ = probe_ok || pk_cache_ != nullptr;
            }
            if (pk_cache_) {
                data->pk = make_uniq<PrimaryKeyInfo>(*pk_cache_);
            }
        }

        bind_data = std::move(data);
        return GetFirebirdScanFunction();
    }

private:
#ifdef FIREBIRD_DUCKDB_V2
    ColumnList columns_;   // v2.0: owned copy — TableCatalogEntry::GetColumns() is pure virtual
#endif
    FirebirdConnectionInfo conn_info_;
    std::shared_ptr<FirebirdConnectionPool> pool_;
    NoneEncoding none_encoding_;
    bool none_pushdown_;
    bool numeric_widen_int64_;
    duckdb::vector<FirebirdColumnDesc> cached_column_descs_;
    duckdb::vector<std::string> cached_column_names_;
    duckdb::vector<LogicalType> cached_column_types_;
    PrimaryKeyDescriptor pk_descriptor_; // populated at ATTACH, zero new I/O

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
                        NoneEncoding none_encoding,
                        bool none_pushdown,
                        bool numeric_widen_int64)
        : SchemaCatalogEntry(catalog, info),
          conn_info_(std::move(conn_info)),
          pool_(std::move(pool)),
          none_encoding_(none_encoding),
          none_pushdown_(none_pushdown),
          numeric_widen_int64_(numeric_widen_int64) {}

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
        // If discovery throws, give the lease back to the pool as discarded:
        // the connection may be the reason it failed (an attachment killed
        // on the server, a broken network link), so it must not be parked.
        // Without this the exception destroyed it without telling the pool,
        // and active_connections stayed one too high for the life of the
        // ATTACH. On success conn_owned is released below and the guard
        // does nothing.
        struct DiscardOnThrow {
            FirebirdConnectionPool *pool;
            std::unique_ptr<FirebirdConnection> &conn;
            ~DiscardOnThrow() {
                if (pool && conn) {
                    pool->Discard(std::move(conn));
                }
            }
        } discard_on_throw {pool_.get(), conn_owned};

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
                             duckdb::vector<LogicalType> col_types,
                             duckdb::vector<FirebirdColumnDesc> col_descs,
                             bool is_view) {
            if (col_names.empty()) return;
            // View column types/descs must be finalized BEFORE the entry is
            // built and published — FirebirdTableEntry instances are shared
            // across every connection, so mutating a published entry's
            // columns later (as GetScanFunction used to do) is a data race
            // against concurrent readers (DESCRIBE, information_schema,
            // catalog introspection) that access GetColumns() without going
            // through GetScanFunction first. Reconciling here, on local
            // mutable copies, means info.columns (and therefore
            // cached_column_types_, mirrored from it in the
            // FirebirdTableEntry constructor) are correct at construction
            // time — nothing downstream needs to mutate a shared entry.
            if (is_view) {
                ReconcileViewColumnTypes(conn, table_name, col_names,
                                         col_types, col_descs,
                                         numeric_widen_int64_);
            }
#ifdef FIREBIRD_DUCKDB_V2
            // v2.0: CreateTableInfo takes a QualifiedName (Identifier-based).
            CreateTableInfo info(QualifiedName(catalog.GetName(), this->name,
                                               Identifier(table_name)));
#else
            CreateTableInfo info(catalog.GetName(), this->name, table_name);
#endif
            for (size_t i = 0; i < col_names.size(); ++i) {
#ifdef FIREBIRD_DUCKDB_V2
                // v2.0: ColumnDefinition names are Identifier.
                info.columns.AddColumn(ColumnDefinition(Identifier(col_names[i]),
                                                        col_types[i]));
#else
                info.columns.AddColumn(ColumnDefinition(col_names[i], col_types[i]));
#endif
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
#ifdef FIREBIRD_DUCKDB_V2
                    // v2.0: UniqueConstraint takes Identifier column names.
                    vector<Identifier> key_cols;
                    key_cols.reserve(key.columns.size());
                    for (auto &c : key.columns) key_cols.emplace_back(c);
                    info.constraints.push_back(
                        make_uniq<UniqueConstraint>(std::move(key_cols), key.is_primary));
#else
                    info.constraints.push_back(
                        make_uniq<UniqueConstraint>(key.columns, key.is_primary));
#endif
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
#ifdef FIREBIRD_DUCKDB_V2
                    fk_info.schema = Identifier(FIREBIRD_MAIN_SCHEMA);  // v2.0: Identifier
                    fk_info.table  = Identifier(fk.pk_table);           // v2.0: Identifier
                    // v2.0: ForeignKeyConstraint takes Identifier column names.
                    vector<Identifier> fk_pk_cols;
                    vector<Identifier> fk_fk_cols;
                    for (auto &c : fk.pk_columns) fk_pk_cols.emplace_back(c);
                    for (auto &c : fk.fk_columns) fk_fk_cols.emplace_back(c);
                    info.constraints.push_back(make_uniq<ForeignKeyConstraint>(
                        std::move(fk_pk_cols), std::move(fk_fk_cols), std::move(fk_info)));
#else
                    fk_info.schema = FIREBIRD_MAIN_SCHEMA;
                    fk_info.table  = fk.pk_table;
                    info.constraints.push_back(make_uniq<ForeignKeyConstraint>(
                        fk.pk_columns, fk.fk_columns, std::move(fk_info)));
#endif
                }
            }
            // Build PrimaryKeyDescriptor from already-loaded constraint map +
            // column types — zero new Firebird I/O.
            PrimaryKeyDescriptor pk_desc;
            if (uk_it != unique_keys.end()) {
                for (auto &key : uk_it->second) {
                    if (key.is_primary) {
                        pk_desc.has_pk  = true;
                        pk_desc.columns = key.columns;
                        // single_numeric: exactly one column AND that column's
                        // LogicalType is an integer family type.
                        if (key.columns.size() == 1) {
                            const auto &pk_col = key.columns[0];
                            for (size_t ci = 0; ci < col_names.size(); ++ci) {
                                if (col_names[ci] == pk_col) {
                                    auto tid = col_types[ci].id();
                                    pk_desc.single_numeric =
                                        (tid == LogicalTypeId::INTEGER  ||
                                         tid == LogicalTypeId::BIGINT   ||
                                         tid == LogicalTypeId::SMALLINT ||
                                         tid == LogicalTypeId::HUGEINT  ||
                                         tid == LogicalTypeId::TINYINT  ||
                                         tid == LogicalTypeId::UINTEGER ||
                                         tid == LogicalTypeId::UBIGINT);
                                    break;
                                }
                            }
                        }
                        break; // only one PK can exist per table
                    }
                }
            }
            auto entry = make_uniq<FirebirdTableEntry>(
                catalog, *this, info, conn_info_, pool_,
                none_encoding_, none_pushdown_, numeric_widen_int64_,
                std::move(col_descs), std::move(pk_desc));
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
            auto schemas = LoadAllTableSchemas(conn, none_encoding_,
                                               numeric_widen_int64_);
            for (auto &ts : schemas) {
                add_entry(ts.table_name, ts.names, ts.types,
                          std::move(ts.descs), ts.is_view);
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
            std::vector<bool> table_is_view;
            try {
                auto cur = conn.OpenCursor(
                    "SELECT TRIM(r.RDB$RELATION_NAME), "
                    "       r.RDB$VIEW_BLR IS NOT NULL "
                    "  FROM RDB$RELATIONS r "
                    " WHERE r.RDB$SYSTEM_FLAG = 0 "
                    "   AND (r.RDB$RELATION_TYPE IS NULL OR r.RDB$RELATION_TYPE <> 3) "
                    " ORDER BY r.RDB$RELATION_NAME");
                while (cur->Fetch()) {
                    table_names.push_back(cur->GetText(0));
                    table_is_view.push_back(cur->GetBool(1));
                }
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
            for (size_t ti = 0; ti < table_names.size(); ++ti) {
                const auto &table_name = table_names[ti];
                try {
                    duckdb::vector<std::string> col_names;
                    duckdb::vector<LogicalType> col_types;
                    duckdb::vector<FirebirdColumnDesc> col_descs;
                    LoadTableSchema(conn, table_name,
                                    col_names, col_types, col_descs,
                                    none_encoding_, numeric_widen_int64_);
                    add_entry(table_name, col_names, col_types,
                              std::move(col_descs), table_is_view[ti]);
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

        // The per-table fallback above skips tables whose load failed. If the
        // connection itself broke on the way, that list is incomplete: don't
        // cache it. Throwing lets discard_on_throw drop the connection, and
        // the next query loads the catalog again.
        if (conn.IsBroken()) {
            tables_.clear();
            throw IOException("Firebird catalog discovery: the connection was lost "
                              "while the table list was loading");
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
    bool none_pushdown_;
    bool numeric_widen_int64_;
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
                    NoneEncoding none_encoding, bool none_pushdown,
                    bool numeric_widen_int64,
                    FirebirdConnectionPoolConfig pool_config = {})
        : Catalog(db),
          conn_info_(std::move(conn_info)),
          pool_(std::make_shared<FirebirdConnectionPool>(conn_info_, pool_config)),
          none_encoding_(none_encoding),
          none_pushdown_(none_pushdown),
          numeric_widen_int64_(numeric_widen_int64) {}

    // Allow the dbt-sources metadata-extraction helper to lease a
    // connection from this catalog's pool without exposing
    // conn_info_ publicly. Implementation lives at file scope just
    // below this class.
    friend FirebirdMetadataLease AcquireFirebirdCatalogLease(
        ClientContext &context, const string &catalog_name,
        const string &function_name);

    // firebird_pool_stats('alias') reads this catalog's pool counters
    // without leasing a connection. Implementation just below the class.
    // (FirebirdPoolStatsRow / ReadFirebirdPoolStats are forward-declared at
    // namespace scope above so this friend names them portably under GCC.)
    friend FirebirdPoolStatsRow ReadFirebirdPoolStats(
        ClientContext &context, const string &catalog_name);

    string GetCatalogType() override { return "firebird"; }

    // ATTACH ... (TYPE firebird, transaction_snapshot true|false): -1 when
    // not given, so the session setting firebird_transaction_snapshot
    // decides.
    int transaction_snapshot_option = -1;
    // Set once a healthy connection of this ATTACH reported no snapshot
    // number (Firebird 3): later transactions don't try again.
    std::shared_ptr<std::atomic<bool>> no_snapshot_numbers =
        std::make_shared<std::atomic<bool>>(false);

    const std::shared_ptr<FirebirdConnectionPool> &Pool() const { return pool_; }
    NoneEncoding NoneEncodingOption() const { return none_encoding_; }
    bool NumericWidenInt64() const { return numeric_widen_int64_; }

    void Initialize(bool /*load_builtin*/) override {
        // One hard-coded "main" schema. Real schema discovery on Firebird
        // would require RDB$OWNER_NAME segmentation, which the legacy
        // OLTP catalogs we target don't actually populate meaningfully.
        CreateSchemaInfo info;
#ifdef FIREBIRD_DUCKDB_V2
        // v2.0: CreateSchemaInfo carries the schema in its QualifiedName
        // path ([catalog, schema, <empty name>]), not a `schema` field.
        info.SetQualifiedName(QualifiedName(this->GetName(),
                                            Identifier(FIREBIRD_MAIN_SCHEMA),
                                            Identifier()));
#else
        info.schema = FIREBIRD_MAIN_SCHEMA;
#endif
        info.on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
        main_schema_ = make_uniq<FirebirdSchemaEntry>(*this, info, conn_info_,
                                                      pool_, none_encoding_,
                                                      none_pushdown_,
                                                      numeric_widen_int64_);
    }

    bool InMemory() override { return false; }

    string GetDBPath() override {
        return RedactConnectionSecrets(conn_info_.database, conn_info_.password);
    }

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
    bool none_pushdown_;
    bool numeric_widen_int64_;
    std::unique_ptr<FirebirdSchemaEntry> main_schema_;
};

// ---------------------------------------------------------------------------
//  FirebirdTransactionManager
//
// Firebird-side transactions are owned by the connections of the scans
// (read-only, read committed). The DuckDB-level transaction carries no
// write state; commit and rollback only end it. With transaction
// snapshots on (opt-in), it also holds the coordinating connection whose
// SNAPSHOT transaction every scan of the DuckDB transaction joins.
// ---------------------------------------------------------------------------

class FirebirdTransaction;

// Per-ClientContext list of the transactions that hold a coordinator with
// an idle limit. At the end of every query the ones still open (an
// explicit BEGIN ... COMMIT) arm the server-side idle timer; the next
// scan of the transaction disarms it. So only the time between statements
// counts as idle, never the time a statement spends running.
class FirebirdTransactionSnapshotState : public ClientContextState {
public:
    static constexpr const char *KEY = "firebird.transaction_snapshot";

    void Register(FirebirdTransaction *tx) {
        std::lock_guard<std::mutex> g(lock_);
        open_.push_back(tx);
    }
    void Unregister(FirebirdTransaction *tx) {
        std::lock_guard<std::mutex> g(lock_);
        open_.erase(std::remove(open_.begin(), open_.end(), tx), open_.end());
    }
    void QueryEnd(ClientContext &context) override;

private:
    std::mutex lock_;
    std::vector<FirebirdTransaction *> open_;
};

class FirebirdTransaction final : public Transaction, public FirebirdTransactionSnapshot {
public:
    FirebirdTransaction(TransactionManager &manager, ClientContext &context,
                        FirebirdCatalog &catalog)
        : Transaction(manager, context), pool_(catalog.Pool()), catalog_name_(catalog.GetName()),
          no_snapshot_numbers_(catalog.no_snapshot_numbers) {}

    ~FirebirdTransaction() override {
        if (state_) {
            state_->Unregister(this);
        }
        if (conn_) {
            GiveBack(std::move(conn_));
        }
    }

    // Transaction snapshot (opt-in, see GetFirebirdTransactionSnapshot).
    // The coordinating connection comes from the ATTACH's pool and holds a
    // read-only SNAPSHOT transaction until this DuckDB transaction ends;
    // every scan joins its snapshot number.
    int64_t Pin(ClientContext &context, int64_t idle_timeout_s) override {
        bool registered = false;
        {
            std::lock_guard<std::mutex> g(lock_);
            if (released_) {
                throw IOException(ReleasedMessage());
            }
            if (attempted_) {
                if (start_failed_) {
                    throw IOException(StartFailedMessage(""));
                }
                Disarm();  // the workers check the coordinator after joining
                return number_;
            }
            attempted_ = true;
            if (no_snapshot_numbers_->load()) {
                return 0;  // Firebird 3: scans behave as without this option
            }
            try {
                StartCoordinator();
            } catch (...) {
                start_failed_ = true;  // never fall back to no snapshot later
                throw;
            }
            if (conn_) {
                idle_timeout_s_ = idle_timeout_s;
            }
            registered = conn_ && idle_timeout_s_ > 0;
        }
        if (registered) {
            state_ = context.registered_state->GetOrCreate<FirebirdTransactionSnapshotState>(
                FirebirdTransactionSnapshotState::KEY);
            state_->Register(this);
        }
        return number_;
    }

    void CheckHeld() override {
        std::lock_guard<std::mutex> g(lock_);
        if (released_) {
            throw IOException(ReleasedMessage());
        }
        Verify();
    }

    // End of a query with this transaction still open: from now on the
    // server closes the coordinator, releasing the snapshot and its hold
    // on garbage collection, after idle_timeout_s_ seconds without a call.
    void Arm() {
        std::lock_guard<std::mutex> g(lock_);
        if (!conn_ || released_ || armed_) {
            return;
        }
        try {
            // The server's timer counts whole seconds (it expires at the
            // current second plus the timeout), so n + 1 gives at least n.
            conn_->Execute("SET SESSION IDLE TIMEOUT " + std::to_string(idle_timeout_s_ + 1) + " SECOND");
            armed_ = true;
        } catch (std::exception &ex) {
            // Never keep a snapshot without the limit it was asked for: the
            // next scan fails, naming the reason. The coordinator goes now,
            // so the pool doesn't count it until the transaction ends.
            if (!conn_->IsBroken()) {
                refused_ = ErrorData(ex).RawMessage();
            }
            released_ = true;
            pool_->Discard(std::move(conn_));
        }
    }

private:
    // Leases a coordinator and starts the snapshot on it. A pooled
    // connection the server already closed is discarded and the next one
    // tried; a healthy connection without a snapshot number (Firebird 3)
    // goes back to the pool and leaves number_ at 0. Anything else throws,
    // so the transaction never reads without its snapshot by accident.
    void StartCoordinator() {
        std::string error;
        // Parked connections may be dead; try until a new one was tried.
        while (true) {
            auto lease = pool_->AcquireWithInfo();
            auto conn = std::move(lease.conn);
            try {
                error.clear();
                if (conn->TryStartSnapshotTransaction(0, &error)) {
                    const int64_t n = conn->SnapshotNumber();
                    if (n > 0) {
                        number_ = n;
                        conn_ = std::move(conn);
                        return;
                    }
                    if (n == -1) {
                        // The server doesn't know snapshot numbers
                        // (Firebird 3): remembered for this ATTACH.
                        no_snapshot_numbers_->store(true);
                        GiveBack(std::move(conn));
                        return;
                    }
                    // Any other failure: never taken as Firebird 3.
                    error = "the snapshot number could not be read";
                }
            } catch (...) {
                pool_->Discard(std::move(conn));
                throw;
            }
            const bool broken = conn->IsBroken();
            pool_->Discard(std::move(conn));
            if (!broken || !lease.reused) {
                break;
            }
        }
        throw IOException(StartFailedMessage(error));
    }

    std::string StartFailedMessage(const std::string &error) const {
        return "Firebird ATTACH '" + catalog_name_ +
               "': could not start the snapshot of this DuckDB transaction "
               "(firebird_transaction_snapshot). Run the query again (inside "
               "BEGIN ... COMMIT, after a ROLLBACK)." +
               (error.empty() ? std::string() : " Last error: " + error);
    }

    // Checks that the coordinator still holds the snapshot (one round
    // trip). Snapshot numbers restart when the database is reopened, so a
    // lost coordinator's number could be joined in someone else's snapshot.
    void Verify() {
        if (!conn_) {
            return;
        }
        if (conn_->SnapshotNumber() != number_) {
            released_ = true;
            pool_->Discard(std::move(conn_));
            throw IOException(ReleasedMessage());
        }
    }

    // The first scan of a new statement: stop the idle timer. This call
    // also tells whether the server closed the coordinator meanwhile.
    void Disarm() {
        if (!armed_ || !conn_) {
            return;
        }
        try {
            conn_->Execute("SET SESSION IDLE TIMEOUT 0");
            armed_ = false;
        } catch (...) {
            released_ = true;
            pool_->Discard(std::move(conn_));
            throw IOException(ReleasedMessage());
        }
    }

    std::string ReleasedMessage() const {
        if (!refused_.empty()) {
            return "Firebird ATTACH '" + catalog_name_ +
                   "': the snapshot of this DuckDB transaction was released because "
                   "the server refused its idle limit "
                   "(firebird_transaction_snapshot_idle_timeout): " + refused_ +
                   ". To run without the limit, SET firebird_transaction_snapshot_idle_timeout = 0, "
                   "then run the query again (inside BEGIN ... COMMIT, after a ROLLBACK).";
        }
        const std::string why =
            idle_timeout_s_ > 0
                ? "was closed by the server, because no scan of this ATTACH ran in "
                  "the transaction for firebird_transaction_snapshot_idle_timeout (" +
                      std::to_string(idle_timeout_s_) + " s), or because the connection was lost"
                : "was lost";
        return "Firebird ATTACH '" + catalog_name_ +
               "': the snapshot of this DuckDB transaction is gone. Its "
               "coordinating connection " + why +
               ". Run the query again (inside BEGIN ... COMMIT, after a ROLLBACK).";
    }

    // Resets the coordinator for its next user and returns it to the pool;
    // a connection that fails that, or is broken, is discarded.
    void GiveBack(std::unique_ptr<FirebirdConnection> conn) {
        try {
            if (!conn->IsBroken()) {
                if (armed_) {
                    conn->Execute("SET SESSION IDLE TIMEOUT 0");
                }
                conn->RestoreDefaultTransaction();
            }
            pool_->Release(std::move(conn));
        } catch (...) {
            if (conn) {
                pool_->Discard(std::move(conn));
            }
        }
    }

    std::shared_ptr<FirebirdConnectionPool> pool_;
    std::string catalog_name_;
    std::shared_ptr<std::atomic<bool>> no_snapshot_numbers_;
    shared_ptr<FirebirdTransactionSnapshotState> state_;
    std::mutex lock_;
    bool attempted_ = false;
    bool start_failed_ = false;
    bool released_ = false;
    std::string refused_;  // why the server refused the idle limit, if it did
    bool armed_ = false;
    int64_t number_ = 0;
    int64_t idle_timeout_s_ = 0;
    std::unique_ptr<FirebirdConnection> conn_;
};

void FirebirdTransactionSnapshotState::QueryEnd(ClientContext &) {
    std::lock_guard<std::mutex> g(lock_);
    for (auto *tx : open_) {
        tx->Arm();
    }
}

class FirebirdTransactionManager final : public TransactionManager {
public:
    explicit FirebirdTransactionManager(AttachedDatabase &db)
        : TransactionManager(db) {}

    Transaction &StartTransaction(ClientContext &context) override {
        auto tx = make_uniq<FirebirdTransaction>(
            *this, context, GetDB().GetCatalog().Cast<FirebirdCatalog>());
        auto &ref = *tx;
        std::lock_guard<std::mutex> g(lock_);
        transactions_.emplace(&ref, std::move(tx));
        return ref;
    }

    ErrorData CommitTransaction(ClientContext &, Transaction &transaction) override {
        End(transaction);
        return ErrorData();
    }

    void RollbackTransaction(Transaction &transaction) override {
        End(transaction);
    }

    void Checkpoint(ClientContext &, bool /*force*/) override {
        // No-op: we don't own writable state.
    }

private:
    // Destroys the transaction outside the lock: giving its coordinator
    // back to the pool takes round trips.
    void End(Transaction &transaction) {
        unique_ptr<FirebirdTransaction> tx;
        {
            std::lock_guard<std::mutex> g(lock_);
            auto it = transactions_.find(&transaction);
            if (it != transactions_.end()) {
                tx = std::move(it->second);
                transactions_.erase(it);
            }
        }
    }

    std::mutex lock_;
    std::unordered_map<Transaction *, unique_ptr<FirebirdTransaction>> transactions_;
};

void ValidateTransactionSnapshotIdleTimeout(int64_t seconds) {
    if (seconds < 0 || seconds > 2147483646) {
        throw BinderException(
            "firebird_transaction_snapshot_idle_timeout must be 0 (no limit) "
            "to 2147483646 seconds, got %lld",
            static_cast<long long>(seconds));
    }
}

FirebirdTransactionSnapshot *GetFirebirdTransactionSnapshot(ClientContext &context,
                                                            Catalog &catalog) {
    auto &fb = catalog.Cast<FirebirdCatalog>();
    bool enabled = false;
    if (fb.transaction_snapshot_option >= 0) {
        enabled = fb.transaction_snapshot_option == 1;
    } else {
        Value v;
        if (context.TryGetCurrentSetting("firebird_transaction_snapshot", v) && !v.IsNull()) {
            enabled = v.GetValue<bool>();
        }
    }
    if (!enabled) {
        return nullptr;
    }
    auto &tx = Transaction::Get(context, catalog);
    return &tx.Cast<FirebirdTransaction>();
}

// ---------------------------------------------------------------------------
//  StorageExtension wiring
// ---------------------------------------------------------------------------

// Parses the ATTACH path + (TYPE firebird, key=value, …) options into a
// FirebirdConnectionInfo and (optionally) a NoneEncoding choice plus the
// G2 `none_pushdown` and G3 `numeric_widen_int64` opt-ins.
static FirebirdConnectionInfo BuildConnectionInfo(const std::string &path,
                                                  AttachInfo &info,
                                                  NoneEncoding &out_none,
                                                  bool &out_none_pushdown,
                                                  bool &out_numeric_widen) {
    auto conn = FirebirdConnectionInfo::Parse(path);
    // Match the firebird_scan default — most NONE-storage databases we
    // see in the wild (Athenas-ERP, IBExpert exports, Delphi-era apps)
    // wrote through a Windows-1252 client.
    out_none = NoneEncoding::WIN1252;
    out_none_pushdown = false;
    out_numeric_widen = false;
    // v1.2.1 — connection-string preferences seed the NONE settings so
    // they ride the DSN/URI form; the explicit ATTACH options below keep
    // precedence when supplied.
    if (!conn.none_encoding.empty()) {
        out_none = ParseNoneEncoding(conn.none_encoding);
    }
    out_none_pushdown = conn.none_pushdown;
    for (auto &kv : info.options) {
        const auto &key = kv.first;
        const auto &val = kv.second;
        if      (key == "user")     conn.user     = val.ToString();
        else if (key == "password") conn.password = val.ToString();
        else if (key == "charset")  conn.charset  = val.ToString();
        else if (key == "role")     conn.role     = val.ToString();
        else if (key == "dialect")  conn.dialect  = static_cast<int>(val.GetValue<int32_t>());
        else if (key == "none_encoding") out_none  = ParseNoneEncoding(val.ToString());
        else if (key == "none_pushdown") out_none_pushdown = val.GetValue<bool>();
        else if (key == "numeric_widen_int64") out_numeric_widen = val.GetValue<bool>();
        // unrecognised keys (including "type") are ignored — DuckDB has
        // already routed us here based on TYPE firebird.
    }
    return conn;
}

// Read a single SET firebird_pool_* option from the current
// ClientContext. Missing or NULL -> use the supplied default. This
// keeps the StorageExtension safe to load even if a future host
// loader skipped the AddExtensionOption registrations.
// (v2.0: TryGetCurrentSetting keys are Identifier; the lookup returns a
// SettingLookupResult convertible to bool.)
#ifdef FIREBIRD_DUCKDB_V2
static int64_t ReadPoolInt64Setting(ClientContext &ctx,
                                     const Identifier &key,
                                     int64_t default_value) {
#else
static int64_t ReadPoolInt64Setting(ClientContext &ctx,
                                     const std::string &key,
                                     int64_t default_value) {
#endif
    Value v;
    if (!ctx.TryGetCurrentSetting(key, v)) return default_value;
    if (v.IsNull()) return default_value;
    return v.GetValue<int64_t>();
}

#ifdef FIREBIRD_DUCKDB_V2
static bool ReadPoolBoolSetting(ClientContext &ctx,
                                 const Identifier &key,
                                 bool default_value) {
#else
static bool ReadPoolBoolSetting(ClientContext &ctx,
                                 const std::string &key,
                                 bool default_value) {
#endif
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
    bool none_pushdown = false;
    bool numeric_widen_int64 = false;
    auto conn = BuildConnectionInfo(attach_info.path, attach_info,
                                    none_encoding, none_pushdown,
                                    numeric_widen_int64);
    // G4 session stability - keepalive for long fetches. Read from the
    // session settings at ATTACH time (same lifecycle as the pool
    // settings: a later SET does not retune an existing attachment).
    // Storing it on FirebirdConnectionInfo propagates everywhere the
    // catalog clones the struct: the pool's template connection, the
    // metadata lease, schema/table catalog entries, and every scanner
    // connection this attachment ever opens.
    conn.dummy_packet_interval_secs =
        ReadPoolInt64Setting(context, "firebird_dummy_packet_interval", 0);
    ValidateDummyPacketInterval(conn.dummy_packet_interval_secs);
    // Inline blobs: same lifecycle as the keepalive (a later SET does not
    // retune an existing attachment).
    conn.max_inline_blob_size =
        ReadPoolInt64Setting(context, "firebird_max_inline_blob_size", -1);
    ValidateMaxInlineBlobSize(conn.max_inline_blob_size);
    auto pool_config = BuildPoolConfig(context);
    auto catalog = make_uniq<FirebirdCatalog>(db, std::move(conn), none_encoding,
                                              none_pushdown, numeric_widen_int64,
                                              pool_config);
    for (auto &kv : attach_info.options) {
        if (kv.first == "transaction_snapshot") {  // keys compared as in BuildConnectionInfo
            Value b;
            try {
                b = kv.second.DefaultCastAs(LogicalType::BOOLEAN);
            } catch (std::exception &) {
            }
            if (b.IsNull()) {
                throw InvalidInputException(
                    "ATTACH option transaction_snapshot must be true or false, got '%s'",
                    kv.second.ToString());
            }
            catalog->transaction_snapshot_option = b.GetValue<bool>() ? 1 : 0;
        }
    }
    return std::move(catalog);
}

// ---------------------------------------------------------------------------
//  Metadata-only lease used by firebird_generate_dbt_sources(...) and any
//  future RDB$-introspection table function. Implemented here so it can
//  see the private pool_/none_encoding_ members of FirebirdCatalog
//  without exposing them publicly. Declared in firebird_dbt_sources.hpp.
// ---------------------------------------------------------------------------

FirebirdMetadataLease AcquireFirebirdCatalogLease(ClientContext &context,
                                                   const string &catalog_name,
                                                   const string &function_name) {
    ValidateFirebirdAttachAlias(context, catalog_name, function_name);
#ifdef FIREBIRD_DUCKDB_V2
    // v2.0: GetCatalogEntry keys by Identifier.
    auto catalog_ptr = Catalog::GetCatalogEntry(context, Identifier(catalog_name));
#else
    auto catalog_ptr = Catalog::GetCatalogEntry(context, catalog_name);
#endif
    // ValidateFirebirdAttachAlias already confirmed catalog_ptr != null
    // and GetCatalogType() == "firebird"; the Cast is safe.
    auto &fb_catalog = catalog_ptr->Cast<FirebirdCatalog>();

    FirebirdMetadataLease lease;
    lease.pool          = fb_catalog.pool_;
    lease.conn          = fb_catalog.pool_->Acquire();
    lease.none_encoding = fb_catalog.none_encoding_;
    return lease;
}

FirebirdAttachHandle GetFirebirdAttachHandle(ClientContext &context, const string &alias,
                                             const string &function_name) {
    ValidateFirebirdAttachAlias(context, alias, function_name);
#ifdef FIREBIRD_DUCKDB_V2
    auto catalog_ptr = Catalog::GetCatalogEntry(context, Identifier(alias));
#else
    auto catalog_ptr = Catalog::GetCatalogEntry(context, alias);
#endif
    auto &fb_catalog = catalog_ptr->Cast<FirebirdCatalog>();
    FirebirdAttachHandle handle;
    handle.pool = fb_catalog.Pool();
    handle.catalog = &fb_catalog;
    handle.none_encoding = fb_catalog.NoneEncodingOption();
    handle.numeric_widen_int64 = fb_catalog.NumericWidenInt64();
    return handle;
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
    int64_t     active_connections = 0;
    std::string last_error; // "" -> NULL (no failure recorded)
    // G5 — lifetime payload-byte estimate accumulated by every scan that
    // ran inside this catalog (ATTACH path). Same semantics as the other
    // lifetime counters: process-relative, never reset. Direct
    // firebird_scan() calls are NOT included — they have no catalog.
    int64_t     bytes_read_estimate = 0;
};

FirebirdPoolStatsRow ReadFirebirdPoolStats(ClientContext &context,
                                           const string &catalog_name) {
    ValidateFirebirdAttachAlias(context, catalog_name, "firebird_pool_stats");
#ifdef FIREBIRD_DUCKDB_V2
    // v2.0: GetCatalogEntry keys by Identifier.
    auto catalog_ptr = Catalog::GetCatalogEntry(context, Identifier(catalog_name));
#else
    auto catalog_ptr = Catalog::GetCatalogEntry(context, catalog_name);
#endif
    // Validated above: non-null and GetCatalogType() == "firebird".
    auto &fb_catalog = catalog_ptr->Cast<FirebirdCatalog>();
    auto &pool = *fb_catalog.pool_;
    const auto &cfg = pool.Config();

    FirebirdPoolStatsRow row;
    row.catalog_name       = catalog_name;
    row.pool_enabled       = cfg.enabled;
    row.max_idle_size      = cfg.max_size;
    row.idle_timeout_ms    = cfg.idle_timeout_ms;
    row.idle_connections   = static_cast<int64_t>(pool.IdleCount());
    row.total_created      = pool.TotalCreated();
    row.total_reused       = pool.TotalReused();
    row.total_discarded    = pool.TotalDiscarded();
    row.active_connections = pool.ActiveCount();
    row.last_error         = pool.LastError();
    row.bytes_read_estimate = pool.BytesReadEstimate();
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
#ifdef FIREBIRD_DUCKDB_V2
                                       vector<Identifier> &names) {  // v2.0: Identifier names
#else
                                       vector<string> &names) {
#endif
    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException(
            "firebird_pool_stats(catalog_name VARCHAR): catalog_name is "
            "required (the alias from ATTACH '...' AS <alias> "
            "(TYPE firebird)).");
    }
    auto bind = make_uniq<PoolStatsBindData>();
    bind->catalog_name = input.inputs[0].ToString();
    // Resolve eagerly so a bad alias fails at bind time, not at execute.
    ValidateFirebirdAttachAlias(context, bind->catalog_name, "firebird_pool_stats");

    names = {
        "catalog_name",
        "pool_enabled",
        "max_idle_size",
        "idle_timeout_ms",
        "idle_connections",
        "total_created",
        "total_reused",
        "total_discarded",
        "active_connections",
        "last_error",
        "bytes_read_estimate",
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
        LogicalType::BIGINT,
        LogicalType::VARCHAR,
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
    output.data[8].SetValue(0, Value::BIGINT(r.active_connections));
    output.data[9].SetValue(0, r.last_error.empty()
                                  ? Value(LogicalType::VARCHAR)
                                  : Value(r.last_error));
    output.data[10].SetValue(0, Value::BIGINT(r.bytes_read_estimate));
    g.emitted = true;
}

} // namespace

TableFunction GetFirebirdPoolStatsFunction() {
    TableFunction fn("firebird_pool_stats",
#ifdef FIREBIRD_DUCKDB_V2
                     FunctionSignature()
                         .AddPositionalOnly("catalog_name", LogicalType::VARCHAR),
#else
                     {LogicalType::VARCHAR},
#endif
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
