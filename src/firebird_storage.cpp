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
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
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
                       FirebirdConnectionInfo conn_info)
        : TableCatalogEntry(catalog, schema, info),
          conn_info_(std::move(conn_info)) {}

    unique_ptr<BaseStatistics>
    GetStatistics(ClientContext & /*ctx*/, column_t /*column_id*/) override {
        return nullptr;            // no stats — DuckDB falls back to defaults
    }

    TableStorageInfo GetStorageInfo(ClientContext & /*ctx*/) override {
        return TableStorageInfo();
    }

    TableFunction GetScanFunction(ClientContext & /*ctx*/,
                                  unique_ptr<FunctionData> &bind_data) override {
        // Build the FunctionData the firebird_scan() table function
        // expects, up front. This is the same data the standalone
        // `firebird_scan('conn', 'TABLE')` call would produce inside its
        // bind callback — we just skip the round-trip through that
        // callback by providing the already-resolved object.
        auto data = make_uniq<FirebirdBindData>();
        data->conn_info = conn_info_;
        data->table_name = name;

        FirebirdConnection conn(data->conn_info);
        LoadTableSchema(conn, data->table_name,
                        data->column_names, data->column_types);
        data->pk = ProbePrimaryKey(conn, data->table_name,
                                   data->column_names, data->column_types);

        bind_data = std::move(data);
        return GetFirebirdScanFunction();
    }

private:
    FirebirdConnectionInfo conn_info_;
};

// ---------------------------------------------------------------------------
//  FirebirdSchemaEntry
// ---------------------------------------------------------------------------

class FirebirdSchemaEntry final : public SchemaCatalogEntry {
public:
    FirebirdSchemaEntry(Catalog &catalog,
                        CreateSchemaInfo &info,
                        FirebirdConnectionInfo conn_info)
        : SchemaCatalogEntry(catalog, info),
          conn_info_(std::move(conn_info)) {}

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
    void EnsureTablesLoaded() {
        std::lock_guard<std::mutex> g(load_lock_);
        if (loaded_) return;

        FirebirdConnection conn(conn_info_);
        // User tables only: system_flag=0, persistent (type=0 or NULL for
        // older Firebird builds that don't populate that column).
        std::vector<std::string> table_names;
        {
            auto cur = conn.OpenCursor(
                "SELECT TRIM(r.RDB$RELATION_NAME) "
                "  FROM RDB$RELATIONS r "
                " WHERE r.RDB$SYSTEM_FLAG = 0 "
                "   AND (r.RDB$RELATION_TYPE = 0 OR r.RDB$RELATION_TYPE IS NULL) "
                " ORDER BY r.RDB$RELATION_NAME");
            while (cur->Fetch()) table_names.push_back(cur->GetText(0));
        }

        // The binder asks the TableCatalogEntry for its full column list
        // before it ever calls GetScanFunction, so we have to materialise
        // the real schema here — one RDB$RELATION_FIELDS round-trip per
        // table. A batch-fetch (single query that JOINs across all
        // tables) is a follow-up optimisation; on a 100-table legacy
        // database this loop is still well under a second.
        //
        // Individual table failures are downgraded to a console-style
        // skip: a missing-permission or weird-relation table shouldn't
        // poison the whole ATTACH.
        for (auto &table_name : table_names) {
            try {
                CreateTableInfo info(catalog.GetName(), this->name, table_name);

                duckdb::vector<std::string> col_names;
                duckdb::vector<LogicalType> col_types;
                LoadTableSchema(conn, table_name, col_names, col_types);
                for (size_t i = 0; i < col_names.size(); ++i) {
                    info.columns.AddColumn(ColumnDefinition(col_names[i], col_types[i]));
                }

                auto entry = make_uniq<FirebirdTableEntry>(catalog, *this, info, conn_info_);
                tables_.emplace(ToUpper(table_name), std::move(entry));
            } catch (std::exception &) {
                // Skip — table will simply not appear in fb.main.
            }
        }
        loaded_ = true;
    }

    [[noreturn]] static void Unsupported(const char *op) {
        throw NotImplementedException(
            std::string("Firebird ATTACH catalog is read-only — ") + op +
            " is not supported. Use a regular DuckDB schema for writes.");
    }

    FirebirdConnectionInfo conn_info_;
    std::mutex load_lock_;
    bool loaded_ = false;
    std::unordered_map<std::string, std::unique_ptr<FirebirdTableEntry>> tables_;
};

// ---------------------------------------------------------------------------
//  FirebirdCatalog
// ---------------------------------------------------------------------------

class FirebirdCatalog final : public Catalog {
public:
    FirebirdCatalog(AttachedDatabase &db, FirebirdConnectionInfo conn_info)
        : Catalog(db), conn_info_(std::move(conn_info)) {}

    string GetCatalogType() override { return "firebird"; }

    void Initialize(bool /*load_builtin*/) override {
        // One hard-coded "main" schema. Real schema discovery on Firebird
        // would require RDB$OWNER_NAME segmentation, which the legacy
        // OLTP catalogs we target don't actually populate meaningfully.
        CreateSchemaInfo info;
        info.schema = FIREBIRD_MAIN_SCHEMA;
        info.on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
        main_schema_ = make_uniq<FirebirdSchemaEntry>(*this, info, conn_info_);
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
// FirebirdConnectionInfo.
static FirebirdConnectionInfo BuildConnectionInfo(const std::string &path,
                                                  AttachInfo &info) {
    auto conn = FirebirdConnectionInfo::Parse(path);
    for (auto &kv : info.options) {
        const auto &key = kv.first;
        const auto &val = kv.second;
        if      (key == "user")     conn.user     = val.ToString();
        else if (key == "password") conn.password = val.ToString();
        else if (key == "charset")  conn.charset  = val.ToString();
        else if (key == "role")     conn.role     = val.ToString();
        else if (key == "dialect")  conn.dialect  = static_cast<int>(val.GetValue<int32_t>());
        // unrecognised keys (including "type") are ignored — DuckDB has
        // already routed us here based on TYPE firebird.
    }
    return conn;
}

static unique_ptr<Catalog>
FirebirdAttach(optional_ptr<StorageExtensionInfo> /*info*/,
               ClientContext & /*context*/,
               AttachedDatabase &db,
               const string & /*name*/,
               AttachInfo &attach_info,
               AttachOptions & /*options*/) {
    auto conn = BuildConnectionInfo(attach_info.path, attach_info);
    return make_uniq<FirebirdCatalog>(db, std::move(conn));
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
    return ext;
}

} // namespace duckdb
