#include "firebird_health.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"

#include "firebird_dbt_sources.hpp" // AcquireFirebirdCatalogLease, ValidateFirebirdAttachAlias

#include <string>
#include <vector>

namespace duckdb {

// Conservative default gap signal (NOT a universal limit) — documented in
// the function manual. Changing this constant changes both gap warnings.
static const int64_t FB_HEALTH_GAP_THRESHOLD = 1000000;

struct HealthInfo {
    // Always readable by any attachment.
    std::string engine_version;
    std::string default_charset;

    // MON$-sourced; valid only when mon_ok.
    bool mon_ok = false;
    int16_t ods_major = 0;
    int16_t ods_minor = 0;
    int16_t sql_dialect = 0;
    int32_t page_size = 0;
    bool forced_writes = false;
    int32_t sweep_interval = 0;
    int64_t oit = 0;
    int64_t oat = 0;
    int64_t ost = 0;
    int64_t next_tx = 0;
    int32_t attachments = 0;

    std::vector<std::string> warnings;
};

static HealthInfo BuildHealth(FirebirdConnection &conn) {
    HealthInfo h;

    // 1) Always-readable fields (any user can read these). A failure here
    //    means a broken attach — let it propagate as a normal scan error.
    {
        auto cur = conn.OpenCursor(
            "SELECT rdb$get_context('SYSTEM','ENGINE_VERSION'), "
            "       TRIM(RDB$CHARACTER_SET_NAME) "
            "  FROM RDB$DATABASE");
        if (cur->Fetch()) {
            h.engine_version = cur->IsNull(0) ? std::string() : cur->GetText(0);
            h.default_charset = cur->IsNull(1) ? std::string() : cur->GetText(1);
        }
    }

    // 2) Privilege-filtered monitoring metrics. The query returns one row
    //    for every attachment (the database row is universal); a partial
    //    attachments count under limited privilege is still a faithful
    //    result. Only a real query failure (throw) degrades to NULLs +
    //    mon_unavailable.
    try {
        auto cur = conn.OpenCursor(
            "SELECT m.MON$ODS_MAJOR, m.MON$ODS_MINOR, m.MON$SQL_DIALECT, "
            "       m.MON$PAGE_SIZE, m.MON$FORCED_WRITES, m.MON$SWEEP_INTERVAL, "
            "       m.MON$OLDEST_TRANSACTION, m.MON$OLDEST_ACTIVE, "
            "       m.MON$OLDEST_SNAPSHOT, m.MON$NEXT_TRANSACTION, "
            "       (SELECT COUNT(*) FROM MON$ATTACHMENTS) "
            "  FROM MON$DATABASE m");
        if (cur->Fetch()) {
            h.ods_major = cur->GetShort(0);
            h.ods_minor = cur->GetShort(1);
            h.sql_dialect = cur->GetShort(2);
            h.page_size = cur->GetLong(3);
            h.forced_writes = (cur->GetShort(4) != 0);
            h.sweep_interval = cur->GetLong(5);
            h.oit = cur->GetInt64(6);
            h.oat = cur->GetInt64(7);
            h.ost = cur->GetInt64(8);
            h.next_tx = cur->GetInt64(9);
            // COUNT(*) is BIGINT in dialect 3; attachment counts are small,
            // so narrowing to INTEGER is safe.
            h.attachments = static_cast<int32_t>(cur->GetInt64(10));
            h.mon_ok = true;
        }
    } catch (...) {
        h.mon_ok = false;
    }

    // 3) Warnings — explicit, documented criteria, deterministic order.
    if (h.mon_ok) {
        if ((h.next_tx - h.oit) > FB_HEALTH_GAP_THRESHOLD) {
            h.warnings.push_back("oit_gap_high");
        }
        if ((h.next_tx - h.oat) > FB_HEALTH_GAP_THRESHOLD) {
            h.warnings.push_back("oat_gap_high");
        }
        if (h.sweep_interval == 0) {
            h.warnings.push_back("sweep_disabled");
        }
        if (!h.forced_writes) {
            h.warnings.push_back("forced_writes_off");
        }
    }
    if (h.default_charset == "NONE") {
        h.warnings.push_back("charset_none");
    }
    if (!h.mon_ok) {
        h.warnings.push_back("mon_unavailable");
    }

    return h;
}

struct HealthBindData : public TableFunctionData {
    std::string catalog_name;
};

struct HealthGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
HealthBind(ClientContext &context, TableFunctionBindInput &input,
           vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException(
            "firebird_health(alias VARCHAR): alias is required (the ATTACH "
            "alias, e.g. 'fb').");
    }
    auto bind = make_uniq<HealthBindData>();
    bind->catalog_name = input.inputs[0].ToString();

    // Fail a bad alias at bind time, not mid-scan.
    ValidateFirebirdAttachAlias(context, bind->catalog_name);

    names = {
        "engine_version", "ods_version", "sql_dialect", "default_charset",
        "page_size", "forced_writes", "sweep_interval", "oldest_transaction",
        "oldest_active", "oldest_snapshot", "next_transaction", "oit_gap",
        "oat_gap", "attachments", "warnings",
    };
    return_types = {
        LogicalType::VARCHAR,                  // engine_version
        LogicalType::VARCHAR,                  // ods_version
        LogicalType::INTEGER,                  // sql_dialect
        LogicalType::VARCHAR,                  // default_charset
        LogicalType::INTEGER,                  // page_size
        LogicalType::BOOLEAN,                  // forced_writes
        LogicalType::INTEGER,                  // sweep_interval
        LogicalType::BIGINT,                   // oldest_transaction
        LogicalType::BIGINT,                   // oldest_active
        LogicalType::BIGINT,                   // oldest_snapshot
        LogicalType::BIGINT,                   // next_transaction
        LogicalType::BIGINT,                   // oit_gap
        LogicalType::BIGINT,                   // oat_gap
        LogicalType::INTEGER,                  // attachments
        LogicalType::LIST(LogicalType::VARCHAR), // warnings
    };
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState>
HealthInitGlobal(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<HealthGlobalState>();
}

static Value VarcharList(const std::vector<std::string> &xs) {
    vector<Value> vals;
    vals.reserve(xs.size());
    for (const auto &s : xs) {
        vals.emplace_back(Value(s));
    }
    return Value::LIST(LogicalType::VARCHAR, std::move(vals));
}

static void HealthFunction(ClientContext &context, TableFunctionInput &input,
                           DataChunk &output) {
    auto &g = input.global_state->Cast<HealthGlobalState>();
    if (g.emitted) {
        output.SetCardinality(0);
        return;
    }
    auto &bind = input.bind_data->Cast<HealthBindData>();

    auto lease = AcquireFirebirdCatalogLease(context, bind.catalog_name);
    HealthInfo h = BuildHealth(*lease.conn);

    // MON$-sourced columns are typed NULLs when the monitoring query failed.
    auto bigint_or_null = [&](int64_t v) {
        return h.mon_ok ? Value::BIGINT(v) : Value(LogicalType::BIGINT);
    };
    auto int_or_null = [&](int32_t v) {
        return h.mon_ok ? Value::INTEGER(v) : Value(LogicalType::INTEGER);
    };

    output.SetCardinality(1);
    output.data[0].SetValue(0, Value(h.engine_version));
    output.data[1].SetValue(
        0, h.mon_ok ? Value(std::to_string(h.ods_major) + "." +
                            std::to_string(h.ods_minor))
                    : Value(LogicalType::VARCHAR));
    output.data[2].SetValue(0, int_or_null(h.sql_dialect));
    output.data[3].SetValue(0, Value(h.default_charset));
    output.data[4].SetValue(0, int_or_null(h.page_size));
    output.data[5].SetValue(
        0, h.mon_ok ? Value::BOOLEAN(h.forced_writes)
                    : Value(LogicalType::BOOLEAN));
    output.data[6].SetValue(0, int_or_null(h.sweep_interval));
    output.data[7].SetValue(0, bigint_or_null(h.oit));
    output.data[8].SetValue(0, bigint_or_null(h.oat));
    output.data[9].SetValue(0, bigint_or_null(h.ost));
    output.data[10].SetValue(0, bigint_or_null(h.next_tx));
    output.data[11].SetValue(0, bigint_or_null(h.next_tx - h.oit));
    output.data[12].SetValue(0, bigint_or_null(h.next_tx - h.oat));
    output.data[13].SetValue(0, int_or_null(h.attachments));
    output.data[14].SetValue(0, VarcharList(h.warnings));
    g.emitted = true;
}

TableFunction GetFirebirdHealthFunction() {
    TableFunction fn("firebird_health", {LogicalType::VARCHAR},
                     HealthFunction, HealthBind, HealthInitGlobal);
    return fn;
}

} // namespace duckdb
