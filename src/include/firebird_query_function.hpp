#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// What a function that runs its own SQL on an ATTACH needs from the
// catalog: the connection pool and the type options the ATTACH's scans use.
struct FirebirdAttachHandle {
    std::shared_ptr<FirebirdConnectionPool> pool;
    Catalog *catalog = nullptr;
    NoneEncoding none_encoding = NoneEncoding::WIN1252;
    bool numeric_widen_int64 = false;
};

// Resolves `alias` to a Firebird ATTACH (BinderException naming
// `function_name` otherwise). Leases nothing.
FirebirdAttachHandle GetFirebirdAttachHandle(ClientContext &context, const std::string &alias,
                                             const std::string &function_name);

// firebird_query(alias, sql): runs a SELECT on the attached database as
// written (no pushdown, no partitions) and returns its rows.
TableFunction GetFirebirdQueryFunction();

// For firebird_explain_pushdown: the SQL and the column names of a bound
// firebird_query call.
void DescribeFirebirdQueryBind(const FunctionData &bind_data, std::string &sql,
                               std::vector<std::string> &columns);

} // namespace duckdb
