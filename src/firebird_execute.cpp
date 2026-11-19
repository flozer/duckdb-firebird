// firebird_execute(alias, sql): runs one statement on an ATTACH opened with
// (TYPE firebird, READ_WRITE), inside the DuckDB transaction's Firebird
// write transaction.
//
// The write transaction starts on the first write of the DuckDB transaction,
// on a connection of the ATTACH's write pool (never the read pool), and is
// committed or rolled back with the DuckDB transaction: one Firebird
// transaction per statement in autocommit, one per BEGIN ... COMMIT.
//
// Returns one row: rows_affected (inserted + updated + deleted, as Firebird
// reports them; NULL for a statement that returns rows) and executions.

#include <algorithm>
#include <cctype>
#include <chrono>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/database_modification_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include "firebird_client.hpp"
#include "firebird_observability.hpp"
#include "firebird_query_function.hpp"

namespace duckdb {

namespace {

constexpr const char *kFunctionName = "firebird_execute";

struct FirebirdExecuteBindData : public TableFunctionData {
    std::string alias;
    std::string sql;
    // The SQL as the query log and error messages show it, and the
    // password literals it hides (see RedactPasswords).
    std::string logged_sql;
    std::vector<std::string> secrets;
    FirebirdAttachHandle attach;
    int64_t lock_timeout_s = 10;
};

struct FirebirdExecuteGlobalState : public GlobalTableFunctionState {
    bool done = false;
};

void CheckWritable(const FirebirdAttachHandle &attach, const std::string &alias) {
    if (attach.read_only) {
        throw BinderException("%s: ATTACH '%s' is READ_ONLY", kFunctionName, alias);
    }
    if (!attach.write_enabled) {
        throw BinderException("%s: ATTACH '%s' does not allow writes. Attach it with "
                              "(TYPE firebird, READ_WRITE) to write to Firebird.",
                              kFunctionName, alias);
    }
}

// The first words of a statement, upper-cased, after leading spaces and
// comments (at most `count`).
std::vector<std::string> LeadingWords(const std::string &sql, size_t count) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < sql.size() && words.size() < count) {
        const unsigned char c = static_cast<unsigned char>(sql[i]);
        if (std::isspace(c)) {
            ++i;
        } else if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') {
                ++i;
            }
        } else if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            const auto end = sql.find("*/", i + 2);
            i = end == std::string::npos ? sql.size() : end + 2;
        } else if (std::isalpha(c)) {
            const size_t start = i;
            while (i < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) {
                ++i;
            }
            words.push_back(StringUtil::Upper(sql.substr(start, i - start)));
        } else {
            break;
        }
    }
    return words;
}

// Session statements change the attachment, not the data. The write
// connection goes back to the pool when the DuckDB transaction ends and
// serves later writes of other DuckDB connections, which would inherit the
// change (a role, a time zone, an idle timeout that makes the server close
// it), so they are refused like transaction control.
void CheckSessionStatement(const std::string &sql) {
    const auto w = LeadingWords(sql, 3);
    std::string what;
    if (w.size() >= 2 && w[0] == "ALTER" && w[1] == "SESSION") {
        what = "ALTER SESSION";
    } else if (w.size() >= 2 && w[0] == "SET") {
        static const char *const kOneWord[] = {"ROLE", "BIND", "DECFLOAT", "SESSION", "OPTIMIZE", "SEARCH_PATH"};
        static const char *const kTwoWords[][2] = {{"TRUSTED", "ROLE"}, {"TIME", "ZONE"}, {"STATEMENT", "TIMEOUT"}};
        for (auto *word : kOneWord) {
            if (w[1] == word) {
                what = "SET " + w[1];
            }
        }
        for (auto &pair : kTwoWords) {
            if (w[1] == pair[0] && w.size() >= 3 && w[2] == pair[1]) {
                what = "SET " + w[1] + " " + w[2];
            }
        }
    }
    if (!what.empty()) {
        throw BinderException("%s: %s is refused: it changes the session of the write connection, which goes back "
                              "to the ATTACH's pool and would keep the change for later writes",
                              kFunctionName, what);
    }
}

// A string literal of a statement: its span in the SQL (quotes included)
// and its text, with '' undone.
struct SqlLiteral {
    size_t begin;
    size_t end;
    std::string text;
};

bool IsSqlWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// Reads a statement the way Firebird's lexer does for what matters here:
// comments, quoted identifiers, '...' and q'...' literals. Returns the
// literals, and in password_word whether the word PASSWORD appears outside
// them.
std::vector<SqlLiteral> ScanSqlLiterals(const std::string &sql, bool &password_word) {
    std::vector<SqlLiteral> out;
    password_word = false;
    const size_t n = sql.size();
    size_t i = 0;
    while (i < n) {
        const char c = sql[i];
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n') {
                ++i;
            }
        } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            const auto close = sql.find("*/", i + 2);
            i = close == std::string::npos ? n : close + 2;
        } else if (c == '"') {
            for (++i; i < n; ++i) {
                if (sql[i] == '"') {
                    if (i + 1 < n && sql[i + 1] == '"') {
                        ++i;
                        continue;
                    }
                    ++i;
                    break;
                }
            }
        } else if ((c == 'q' || c == 'Q') && i + 2 < n && sql[i + 1] == '\'' &&
                   (i == 0 || !IsSqlWordChar(sql[i - 1]))) {
            // q'<x>...<x>', with (), [], {} or <> as pairs.
            const char open = sql[i + 2];
            const char close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}' : open == '<' ? '>' : open;
            size_t j = i + 3;
            while (j < n && !(sql[j] == close && j + 1 < n && sql[j + 1] == '\'')) {
                ++j;
            }
            const size_t end = j < n ? j + 2 : n;
            out.push_back({i, end, sql.substr(i + 3, (j < n ? j : n) - (i + 3))});
            i = end;
        } else if (c == '\'') {
            std::string text;
            size_t j = i + 1;
            while (j < n) {
                if (sql[j] == '\'') {
                    if (j + 1 < n && sql[j + 1] == '\'') {
                        text += '\'';
                        j += 2;
                        continue;
                    }
                    break;
                }
                text += sql[j++];
            }
            const size_t end = j < n ? j + 1 : n;
            out.push_back({i, end, text});
            i = end;
        } else if (IsSqlWordChar(c)) {
            const size_t start = i;
            while (i < n && IsSqlWordChar(sql[i])) {
                ++i;
            }
            if (StringUtil::CIEquals(sql.substr(start, i - start), "PASSWORD")) {
                password_word = true;
            }
        } else {
            ++i;
        }
    }
    return out;
}

// The SQL as the query log and error messages show it. When the statement
// mentions PASSWORD anywhere, also inside a literal (an EXECUTE STATEMENT
// of a CREATE USER), every string literal becomes '***': a password can sit
// in any of them, nested or not. secrets gets what an error message could
// quote: each literal as written (quotes included), its text when at least
// 4 characters long (a nested statement), and the literals of a nested
// statement that mentions PASSWORD; longest first.
std::string RedactPasswords(const std::string &sql, std::vector<std::string> &secrets) {
    bool password_word;
    const auto literals = ScanSqlLiterals(sql, password_word);
    bool mentioned = password_word;
    for (auto &literal : literals) {
        if (StringUtil::Contains(StringUtil::Upper(literal.text), "PASSWORD")) {
            mentioned = true;
        }
    }
    if (!mentioned) {
        return sql;
    }
    std::string out;
    size_t last = 0;
    for (auto &literal : literals) {
        out.append(sql, last, literal.begin - last);
        out += "'***'";
        last = literal.end;
        if (!literal.text.empty()) {
            secrets.push_back(sql.substr(literal.begin, literal.end - literal.begin));
        }
        if (literal.text.size() >= 4) {
            secrets.push_back(literal.text);
        }
        if (StringUtil::Contains(StringUtil::Upper(literal.text), "PASSWORD")) {
            RedactPasswords(literal.text, secrets);
        }
    }
    out.append(sql, last, std::string::npos);
    std::sort(secrets.begin(), secrets.end(),
              [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    return out;
}

// An error message with the SQL and its literals hidden (see RedactPasswords).
std::string RedactMessage(const std::string &message, const FirebirdExecuteBindData &bind) {
    std::string out = StringUtil::Replace(message, bind.sql, bind.logged_sql);
    for (auto &secret : bind.secrets) {
        out = StringUtil::Replace(out, secret, "***");
    }
    return out;
}

// Transaction control belongs to DuckDB. A statement that returns rows
// (SELECT, a selectable procedure, EXECUTE BLOCK with SUSPEND) is accepted:
// it runs, its rows are read and discarded (so a selectable procedure's side
// effects happen, as in isql), and rows_affected is NULL. Returns whether
// the statement returns rows.
bool CheckStatementType(FirebirdStatement &stmt) {
    const int type = stmt.StatementType();
    if (type == isc_info_sql_stmt_start_trans || type == isc_info_sql_stmt_commit ||
        type == isc_info_sql_stmt_rollback) {
        throw InvalidInputException("%s: transactions follow the DuckDB transaction; use BEGIN, COMMIT "
                                    "and ROLLBACK in DuckDB instead of SET TRANSACTION, COMMIT or "
                                    "ROLLBACK in the statement",
                                    kFunctionName);
    }
    return type == isc_info_sql_stmt_select || type == isc_info_sql_stmt_select_for_upd;
}

unique_ptr<FunctionData> FirebirdExecuteBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types,
#ifdef FIREBIRD_DUCKDB_V2
                                             vector<Identifier> &names) {
#else
                                             vector<string> &names) {
#endif
    if (input.inputs.size() != 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
        throw BinderException("%s(alias, sql) requires two non-NULL arguments", kFunctionName);
    }
    auto bind = make_uniq<FirebirdExecuteBindData>();
    bind->alias = input.inputs[0].ToString();
    bind->sql = input.inputs[1].ToString();
    bind->logged_sql = RedactPasswords(bind->sql, bind->secrets);
    bind->attach = GetFirebirdAttachHandle(context, bind->alias, kFunctionName);
    // Run the whole statement before the client sees its result, as DuckDB
    // does for INSERT. A streamed result commits only once it is fully read:
    // a client that read the one row with fetchone() and closed the
    // connection rolled every write back without an error. DuckDB 1.5
    // materializes the result with this output type; DuckDB 2.0 has none and
    // is handled in the function (see the autocommit commit there).
    // Registering the write marks the statement as a write on both.
    if (input.binder) {
        auto &properties = input.binder->GetStatementProperties();
#ifndef FIREBIRD_DUCKDB_V2
        properties.output_type = QueryResultOutputType::FORCE_MATERIALIZED;
#endif
        if (!bind->attach.read_only && bind->attach.write_enabled) {
            properties.RegisterDBModify(*bind->attach.catalog, context,
                                        DatabaseModificationType(DatabaseModificationType::INSERT_DATA |
                                                                 DatabaseModificationType::UPDATE_DATA |
                                                                 DatabaseModificationType::DELETE_DATA));
        }
    }
    CheckWritable(bind->attach, bind->alias);
    CheckSessionStatement(bind->sql);

    Value timeout;
    if (context.TryGetCurrentSetting("firebird_write_lock_timeout", timeout) && !timeout.IsNull()) {
        bind->lock_timeout_s = timeout.GetValue<int64_t>();
    }
    if (bind->lock_timeout_s < 0 || bind->lock_timeout_s > 32767) {
        throw BinderException("firebird_write_lock_timeout must be 0 (no wait) to 32767 seconds, got %lld",
                              static_cast<long long>(bind->lock_timeout_s));
    }

    return_types = {LogicalType::BIGINT, LogicalType::BIGINT};
#ifdef FIREBIRD_DUCKDB_V2
    names = {Identifier("rows_affected"), Identifier("executions")};
#else
    names = {"rows_affected", "executions"};
#endif
    return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> FirebirdExecuteInitGlobal(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<FirebirdExecuteGlobalState>();
}

void FirebirdExecuteFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &bind = data.bind_data->Cast<FirebirdExecuteBindData>();
    auto &state = data.global_state->Cast<FirebirdExecuteGlobalState>();
    if (state.done) {
        output.SetCardinality(0);
        return;
    }
    state.done = true;

    // A DuckDB transaction writes to one attached database at most; this
    // also makes a READ ONLY DuckDB transaction refuse the write.
    auto &db = bind.attach.catalog->GetAttached();
    MetaTransaction::Get(context).ModifyDatabase(
        db, DatabaseModificationType(DatabaseModificationType::INSERT_DATA | DatabaseModificationType::UPDATE_DATA |
                                     DatabaseModificationType::DELETE_DATA));

    int64_t rows_affected = 0;
    int64_t executions = 0;
    bool rows_unknown = false;
    RecordFirebirdWrite(context, bind.logged_sql, "execute", "", -1);
    auto obs = GetObservabilityState(context);
    const auto t0 = std::chrono::steady_clock::now();
    try {
        // One write at a time on the DuckDB transaction's write connection:
        // two calls in one statement (UNION ALL, ...) can run in parallel
        // pipelines, and they wait for each other here. Held through the
        // commit below.
        auto writes = LockFirebirdWrites(context, *bind.attach.catalog);
        // The statement is freed at the end of this block, before the commit
        // below hands its connection back to the write pool (which may close it).
        {
            auto &conn = GetFirebirdWriteConnection(context, *bind.attach.catalog, bind.lock_timeout_s);
            FirebirdStatement stmt(conn, bind.sql, FirebirdStatement::PrepareOnlyTag {});
            const bool returns_rows = CheckStatementType(stmt);
            const bool is_ddl = stmt.StatementType() == isc_info_sql_stmt_ddl;
            if (is_ddl) {
                // Idle connections of this ATTACH would hold the tables they
                // used ("object ... is in use" on DROP / ALTER).
                ReleaseFirebirdIdleConnections(*bind.attach.catalog);
            }
            const auto params = stmt.DescribeInput().size();
            if (params > 0) {
                throw InvalidInputException("%s: the statement has %llu parameter(s) (?); firebird_execute "
                                            "takes no parameter values",
                                            kFunctionName, static_cast<unsigned long long>(params));
            }
            if (returns_rows) {
                stmt.ExecuteCursor(bind.sql);
                while (stmt.Fetch()) {
                }
                rows_unknown = true;
            } else {
                stmt.ExecuteNoCursor(bind.sql);
                rows_affected = stmt.RecordsAffected();
                if (is_ddl) {
                    // The ATTACH reloads its tables once this commits.
                    NoteFirebirdDdl(context, *bind.attach.catalog);
                }
            }
            executions = 1;
        }
#ifdef FIREBIRD_DUCKDB_V2
        // DuckDB 2.0 runs this statement's result lazily and rolls the
        // statement back when its client closes before reading it all
        // (fetchone(), then close): the write would be lost without an
        // error. In autocommit the statement is the transaction, so commit
        // the Firebird write here, before the row is returned. Inside
        // BEGIN ... COMMIT nothing changes. (1.5 materializes the result
        // instead; see the bind.)
        if (context.transaction.IsAutoCommit()) {
            CommitFirebirdWriteNow(context, *bind.attach.catalog);
        }
#endif
    } catch (std::exception &ex) {
        ErrorData error(ex);
        const bool redact = bind.logged_sql != bind.sql;
        if (redact) {
            error = ErrorData(error.Type(), RedactMessage(error.RawMessage(), bind));
        }
        obs->SetWriteResult("row", executions, -1);
        obs->SetError(error.RawMessage());
        if (redact) {
            error.Throw();
        }
        throw;
    }
    obs->SetWriteResult("row", executions, rows_unknown ? -1 : rows_affected);
    obs->AddFirebirdTimeUs(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    obs->UpdateTotalTimeUs();

    output.SetCardinality(1);
    output.data[0].SetValue(0, rows_unknown ? Value(LogicalType::BIGINT) : Value::BIGINT(rows_affected));
    output.data[1].SetValue(0, Value::BIGINT(executions));
}

// --- firebird_clear_cache(alias) ----------------------------------------------

struct FirebirdClearCacheBindData : public TableFunctionData {
    FirebirdAttachHandle attach;
};

struct FirebirdClearCacheState : public GlobalTableFunctionState {
    bool done = false;
};

unique_ptr<FunctionData> FirebirdClearCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types,
#ifdef FIREBIRD_DUCKDB_V2
                                                vector<Identifier> &names) {
#else
                                                vector<string> &names) {
#endif
    if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
        throw BinderException("firebird_clear_cache(alias) requires a non-NULL alias");
    }
    auto bind = make_uniq<FirebirdClearCacheBindData>();
    bind->attach = GetFirebirdAttachHandle(context, input.inputs[0].ToString(), "firebird_clear_cache");
    return_types = {LogicalType::BOOLEAN};
#ifdef FIREBIRD_DUCKDB_V2
    names = {Identifier("cleared")};
#else
    names = {"cleared"};
#endif
    return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> FirebirdClearCacheInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<FirebirdClearCacheState>();
}

void FirebirdClearCacheFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &state = data.global_state->Cast<FirebirdClearCacheState>();
    if (state.done) {
        output.SetCardinality(0);
        return;
    }
    state.done = true;
    ClearFirebirdCatalogCache(*data.bind_data->Cast<FirebirdClearCacheBindData>().attach.catalog);
    output.SetCardinality(1);
    output.data[0].SetValue(0, Value::BOOLEAN(true));
}

} // namespace

TableFunction GetFirebirdClearCacheFunction() {
    return TableFunction("firebird_clear_cache",
#ifdef FIREBIRD_DUCKDB_V2
                         FunctionSignature().AddPositionalOnly("alias", LogicalType::VARCHAR),
#else
                         {LogicalType::VARCHAR},
#endif
                         FirebirdClearCacheFunction, FirebirdClearCacheBind, FirebirdClearCacheInit);
}

TableFunction GetFirebirdExecuteFunction() {
#ifdef FIREBIRD_DUCKDB_V2
    TableFunction fn(kFunctionName,
                     FunctionSignature()
                         .AddPositionalOnly("alias", LogicalType::VARCHAR)
                         .AddPositionalOnly("sql", LogicalType::VARCHAR),
                     FirebirdExecuteFunction, FirebirdExecuteBind, FirebirdExecuteInitGlobal);
#else
    TableFunction fn(kFunctionName, {LogicalType::VARCHAR, LogicalType::VARCHAR}, FirebirdExecuteFunction,
                     FirebirdExecuteBind, FirebirdExecuteInitGlobal);
#endif
    fn.projection_pushdown = false;
    return fn;
}

} // namespace duckdb
