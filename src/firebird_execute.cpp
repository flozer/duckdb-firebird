// firebird_execute(alias, sql [, source := '<duckdb query>',
//                  commit_every := N, count_rows := true]):
// runs a statement that returns no rows on an ATTACH opened with
// (TYPE firebird, READ_WRITE), inside the DuckDB transaction's Firebird
// write transaction.
//
// The write transaction starts on the first write of the DuckDB transaction,
// on a connection of the ATTACH's write pool (never the read pool), and is
// committed or rolled back with the DuckDB transaction: one Firebird
// transaction per statement in autocommit, one per BEGIN ... COMMIT.
//
// With `source`, the statement is prepared once and executed once per row
// of the DuckDB query, with the row's columns as its `?` parameters (column
// i -> parameter i), encoded by FirebirdParamEncoder. commit_every = N
// commits every N executions (those rows stay committed if a later row
// fails); 0 (default) commits with the DuckDB transaction.
//
// Returns one row: rows_affected (inserted + updated + deleted, as Firebird
// reports them; NULL with count_rows := false) and executions.

#include <algorithm>
#include <cctype>
#include <chrono>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/database_modification_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#ifdef FIREBIRD_DUCKDB_V2
#include "duckdb/main/query_result_stream.hpp"
#endif
#include "duckdb/planner/binder.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#ifdef FIREBIRD_DUCKDB_V2
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#endif

#include "firebird_client.hpp"
#include "firebird_observability.hpp"
#include "firebird_param_encoder.hpp"
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
    // DuckDB 2.0: the call is the whole statement (the plan is the call, or
    // a choice of its columns). Set by the optimizer hook, MarkWholeStatement.
    bool whole_statement = false;
    bool has_source = false;
    std::string source;
    vector<LogicalType> source_types;
    int64_t commit_every = 0;
    bool count_rows = true;
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
// (SELECT, a selectable procedure, EXECUTE BLOCK with SUSPEND) is accepted
// only without a source: it runs, its rows are read and discarded (so a
// selectable procedure's side effects happen, as in isql), and
// rows_affected is NULL. Returns whether the statement returns rows.
bool CheckStatementType(FirebirdStatement &stmt, bool allow_rows) {
    const int type = stmt.StatementType();
    const bool returns_rows = type == isc_info_sql_stmt_select || type == isc_info_sql_stmt_select_for_upd;
    if (returns_rows && !allow_rows) {
        throw InvalidInputException("%s: with source, the statement must not return rows (this one does); "
                                    "use firebird_query() to read",
                                    kFunctionName);
    }
    if (type == isc_info_sql_stmt_start_trans || type == isc_info_sql_stmt_commit ||
        type == isc_info_sql_stmt_rollback) {
        throw InvalidInputException("%s: transactions follow the DuckDB transaction; use BEGIN, COMMIT "
                                    "and ROLLBACK in DuckDB instead of SET TRANSACTION, COMMIT or "
                                    "ROLLBACK in the statement",
                                    kFunctionName);
    }
    return returns_rows;
}

#ifdef FIREBIRD_DUCKDB_V2
// DuckDB 2.0 fills an omitted keyword-only parameter with NULL, so its value
// alone can't tell `source := NULL` from no source. The call as written still
// names its arguments (the binder moves out their expressions, not their
// names), so this reads whether the caller wrote `name := ...`.
bool CallNamesArgument(const TableFunctionBindInput &input, const char *name) {
    if (!input.ref.function || input.ref.function->GetExpressionClass() != ExpressionClass::FUNCTION) {
        return false;
    }
    for (auto &argument : input.ref.function->Cast<FunctionExpression>().GetArguments()) {
        if (argument.HasName() && argument.GetName() == name) {
            return true;
        }
    }
    return false;
}
#endif

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
    for (auto &kv : input.named_parameters) {
#ifdef FIREBIRD_DUCKDB_V2
        if (kv.second.IsNull()) {
            // An explicit `source := NULL` is refused, as on 1.5; an omitted
            // source arrives as NULL too, and means no source.
            if (kv.first == "source" && CallNamesArgument(input, "source")) {
                throw BinderException("%s: source must not be NULL", kFunctionName);
            }
            continue;  // v2.0 fills omitted keyword-only parameters with NULL
        }
#endif
        if (kv.first == "source") {
            if (kv.second.IsNull()) {
                throw BinderException("%s: source must not be NULL", kFunctionName);
            }
            bind->has_source = true;
            bind->source = kv.second.ToString();
        } else if (kv.first == "commit_every") {
            bind->commit_every = kv.second.IsNull() ? 0 : kv.second.GetValue<int64_t>();
            if (bind->commit_every < 0) {
                throw BinderException("%s: commit_every must be >= 0 (0 = commit with the DuckDB "
                                      "transaction)",
                                      kFunctionName);
            }
        } else if (kv.first == "count_rows") {
            bind->count_rows = kv.second.IsNull() || kv.second.GetValue<bool>();
        }
    }
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
    if (bind->commit_every > 0 && !bind->has_source) {
        throw BinderException("%s: commit_every needs source := '<query>'", kFunctionName);
    }

    Value timeout;
    if (context.TryGetCurrentSetting("firebird_write_lock_timeout", timeout) && !timeout.IsNull()) {
        bind->lock_timeout_s = timeout.GetValue<int64_t>();
    }
    if (bind->lock_timeout_s < 0 || bind->lock_timeout_s > 32767) {
        throw BinderException("firebird_write_lock_timeout must be 0 (no wait) to 32767 seconds, got %lld",
                              static_cast<long long>(bind->lock_timeout_s));
    }

    if (bind->has_source) {
        // The source's columns, without running it.
        Connection con(*context.db);
        auto prepared = con.Prepare(bind->source);
        if (prepared->HasError()) {
            throw BinderException("%s: the source query does not prepare: %s", kFunctionName,
                                  prepared->GetError());
        }
        bind->source_types = prepared->GetTypes();
        // The statement's parameters, on a read connection (prepare only).
        idx_t param_count = 0;
        {
            auto conn = bind->attach.pool->Acquire();
            try {
                FirebirdStatement stmt(*conn, bind->sql, FirebirdStatement::PrepareOnlyTag {});
                CheckStatementType(stmt, false);
                param_count = stmt.DescribeInput().size();
            } catch (std::exception &ex) {
                bind->attach.pool->Release(std::move(conn));
                // A prepare error quotes the statement: hide its literals too.
                if (bind->logged_sql != bind->sql) {
                    ErrorData error(ex);
                    ErrorData(error.Type(), RedactMessage(error.RawMessage(), *bind)).Throw();
                }
                throw;
            }
            bind->attach.pool->Release(std::move(conn));
        }
        if (param_count != bind->source_types.size()) {
            throw BinderException("%s: the statement has %llu parameter(s) (?) but the source query "
                                  "returns %llu column(s); they are matched by position",
                                  kFunctionName, static_cast<unsigned long long>(param_count),
                                  static_cast<unsigned long long>(bind->source_types.size()));
        }
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

#ifdef FIREBIRD_DUCKDB_V2
    // In autocommit the call commits its write before returning its row
    // (see below), so nothing after it in the same statement could roll it
    // back. Only a call that is the whole statement can do that and stay
    // all or nothing; anything else is refused before it writes. The one
    // exception: an INSERT into the same ATTACH that the call feeds directly
    // has the write connection pinned, so the call doesn't commit, and the
    // write commits with the INSERT's statement.
    if (context.transaction.IsAutoCommit() && !bind.whole_statement &&
        !FirebirdWritesPinned(context, *bind.attach.catalog)) {
        throw InvalidInputException(
            "%s: on DuckDB 2.0, in autocommit, firebird_execute must be the whole statement "
            "(SELECT * FROM firebird_execute(...)): it commits as soon as it runs, so the rest of the "
            "statement could not roll it back. Run it between BEGIN and COMMIT to combine it with other work.",
            kFunctionName);
    }
#endif

    // commit_every commits the transaction's write connection, so inside
    // BEGIN ... COMMIT it would also commit the earlier writes of the DuckDB
    // transaction, which a ROLLBACK could then no longer undo.
    if (bind.commit_every > 0 && !context.transaction.IsAutoCommit()) {
        throw InvalidInputException("%s: commit_every needs autocommit; inside BEGIN ... COMMIT its commits would "
                                    "also commit the transaction's earlier writes",
                                    kFunctionName);
    }
    // Inside an INSERT into the same ATTACH (INSERT ... SELECT ... FROM
    // firebird_execute(...)), the INSERT's rows share the write transaction.
    if (bind.commit_every > 0 && FirebirdWritesPinned(context, *bind.attach.catalog)) {
        throw InvalidInputException("%s: commit_every can't run inside an INSERT into the same ATTACH: its "
                                    "commits would also commit the INSERT's rows",
                                    kFunctionName);
    }

    // A DuckDB transaction writes to one attached database at most; this
    // also makes a READ ONLY DuckDB transaction refuse the write.
    auto &db = bind.attach.catalog->GetAttached();
    MetaTransaction::Get(context).ModifyDatabase(
        db, DatabaseModificationType(DatabaseModificationType::INSERT_DATA | DatabaseModificationType::UPDATE_DATA |
                                     DatabaseModificationType::DELETE_DATA));

    int64_t rows_affected = 0;
    int64_t executions = 0;
    bool rows_unknown = !bind.count_rows;
    RecordFirebirdWrite(context, bind.logged_sql, "execute", "", -1);
    auto obs = GetObservabilityState(context);
    const auto t0 = std::chrono::steady_clock::now();
    try {
        // One write at a time on the DuckDB transaction's write connection:
        // two calls in one statement (UNION ALL, ...) can run in parallel
        // pipelines, and they wait for each other here. Held through the
        // commit below.
        auto writes = LockFirebirdWrites(context, *bind.attach.catalog);
        // Each branch frees its statement at its end, before the commit
        // below hands its connection back to the write pool (which may close it).
        if (!bind.has_source) {
            auto &conn = GetFirebirdWriteConnection(context, *bind.attach.catalog, bind.lock_timeout_s);
            FirebirdStatement stmt(conn, bind.sql, FirebirdStatement::PrepareOnlyTag {});
            const bool returns_rows = CheckStatementType(stmt, true);
            const bool is_ddl = stmt.StatementType() == isc_info_sql_stmt_ddl;
            if (is_ddl) {
                // Idle connections of this ATTACH would hold the tables they
                // used ("object ... is in use" on DROP / ALTER).
                ReleaseFirebirdIdleConnections(*bind.attach.catalog);
            }
            const auto params = stmt.DescribeInput().size();
            if (params > 0) {
                throw InvalidInputException("%s: the statement has %llu parameter(s) (?); pass their values with "
                                            "source := '<query>'",
                                            kFunctionName, static_cast<unsigned long long>(params));
            }
            if (returns_rows) {
                stmt.ExecuteCursor(bind.sql);
                while (stmt.Fetch()) {
                }
                rows_unknown = true;
            } else {
                stmt.ExecuteNoCursor(bind.sql);
                if (bind.count_rows) {
                    rows_affected = stmt.RecordsAffected();
                }
                if (is_ddl) {
                    // The ATTACH reloads its tables once this commits.
                    NoteFirebirdDdl(context, *bind.attach.catalog);
                }
            }
            executions = 1;
        } else {
            auto &conn = GetFirebirdWriteConnection(context, *bind.attach.catalog, bind.lock_timeout_s);
            FirebirdStatement stmt(conn, bind.sql, FirebirdStatement::PrepareOnlyTag {});
            CheckStatementType(stmt, false);
            FirebirdParamEncoder encoder(stmt, bind.source_types, bind.attach.none_encoding);

            // The source is read as a stream, a chunk at a time: its connection
            // buffers a chunk or two ahead of the writes (DuckDB's default read-ahead is
            // about 1 MB on 1.5 and 10 MB on 2.0, often 100,000 rows). Writing to
            // Firebird is the slow side, so a longer read-ahead gains nothing; it
            // would only hold more memory, and a source that fails within it
            // would fail before the rows ahead of the failure are written.
            Connection con(*context.db);
#ifdef FIREBIRD_DUCKDB_V2
            auto read_ahead = con.Query("SET max_streaming_buffer_size = '1KB'");
#else
            auto read_ahead = con.Query("SET streaming_buffer_size = '1KB'");
#endif
            if (read_ahead->HasError()) {
                throw InternalException("%s: could not limit the source's read-ahead: %s", kFunctionName,
                                        read_ahead->GetError());
            }
#ifdef FIREBIRD_DUCKDB_V2
            auto submitted = con.Submit(bind.source);
            if (submitted->HasError()) {
                throw InvalidInputException("%s: the source query failed: %s", kFunctionName, submitted->GetError());
            }
            QueryResultStream<> source(std::move(submitted));
#else
            auto result = con.SendQuery(bind.source);
            if (result->HasError()) {
                throw InvalidInputException("%s: the source query failed: %s", kFunctionName, result->GetError());
            }
            auto &source = *result;
#endif
            int64_t committed = 0;
            auto kept = [&]() -> std::string {
                return committed > 0 ? " Source rows 1 to " + std::to_string(committed) +
                                           " were committed (commit_every); the later ones are rolled back."
                                     : " Nothing of this call is kept: the transaction is rolled back.";
            };
            auto fail = [&](int64_t row, const std::string &message) {
                throw InvalidInputException("%s: source row %lld failed: %s%s", kFunctionName,
                                            static_cast<long long>(row), message, kept());
            };
            bool types_checked = false;
            while (true) {
                auto chunk = source.Fetch();
                if (!chunk || chunk->size() == 0) {
                    break;
                }
                if (!types_checked) {
                    // The encoder reads each value as the type found at bind.
                    // DuckDB doesn't bind a prepared statement again when a
                    // table the source reads changes (it isn't a dependency
                    // of this statement), so a column now of another type
                    // would be misread (5 read as 0.05).
                    const auto &now = chunk->GetTypes();
                    if (now.size() != bind.source_types.size()) {
                        throw InvalidInputException("%s: the source query returned %llu column(s) when the call was "
                                                    "bound and returns %llu now; prepare the statement again.%s",
                                                    kFunctionName,
                                                    static_cast<unsigned long long>(bind.source_types.size()),
                                                    static_cast<unsigned long long>(now.size()), kept());
                    }
                    for (idx_t c = 0; c < now.size(); ++c) {
                        if (now[c] != bind.source_types[c]) {
                            throw InvalidInputException(
                                "%s: source column %llu was %s when the call was bound and is %s now; "
                                "prepare the statement again.%s",
                                kFunctionName, static_cast<unsigned long long>(c + 1),
                                bind.source_types[c].ToString(), now[c].ToString(), kept());
                        }
                    }
                    types_checked = true;
                }
                for (idx_t r = 0; r < chunk->size(); ++r) {
                    try {
                        encoder.Encode(*chunk, r);
                        stmt.ExecuteWithInput(bind.sql);
                        if (bind.count_rows) {
                            rows_affected += stmt.RecordsAffected();
                        }
                    } catch (std::exception &ex) {
                        fail(executions + 1, ErrorData(ex).RawMessage());
                    }
                    ++executions;
                    if (bind.commit_every > 0 && executions % bind.commit_every == 0) {
                        // A failed commit keeps only what earlier boundaries
                        // committed; once it succeeds, these rows are kept too,
                        // even if the next write transaction fails to start.
                        try {
                            conn.CommitWrite();
                        } catch (std::exception &ex) {
                            throw InvalidInputException("%s: the commit after source row %lld failed: %s%s",
                                                        kFunctionName, static_cast<long long>(executions),
                                                        ErrorData(ex).RawMessage(), kept());
                        }
                        committed = executions;
                        try {
                            conn.StartWriteTransaction(bind.lock_timeout_s);
                        } catch (std::exception &ex) {
                            throw InvalidInputException("%s: the write transaction after source row %lld "
                                                        "could not start: %s%s",
                                                        kFunctionName, static_cast<long long>(executions),
                                                        ErrorData(ex).RawMessage(), kept());
                        }
                    }
                }
            }
            if (source.HasError()) {
                throw InvalidInputException("%s: the source query failed after %lld row(s): %s%s", kFunctionName,
                                            static_cast<long long>(executions), source.GetError(), kept());
            }
        }
#ifdef FIREBIRD_DUCKDB_V2
        // DuckDB 2.0 runs this statement's result lazily and rolls the
        // statement back when its client closes before reading it all
        // (fetchone(), then close): the write would be lost without an
        // error. In autocommit this call is the whole statement (checked
        // above), so nothing after it can fail or write: commit the Firebird
        // write here, before the row is returned. Inside BEGIN ... COMMIT
        // nothing changes. (1.5 materializes the result instead; see the
        // bind.)
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
                         .AddPositionalOnly("sql", LogicalType::VARCHAR)
                         .AddKeywordOnly("source", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))
                         .AddKeywordOnly("commit_every", LogicalType::BIGINT, Value(LogicalType::BIGINT))
                         .AddKeywordOnly("count_rows", LogicalType::BOOLEAN, Value(LogicalType::BOOLEAN)),
                     FirebirdExecuteFunction, FirebirdExecuteBind, FirebirdExecuteInitGlobal);
#else
    TableFunction fn(kFunctionName, {LogicalType::VARCHAR, LogicalType::VARCHAR}, FirebirdExecuteFunction,
                     FirebirdExecuteBind, FirebirdExecuteInitGlobal);
    fn.named_parameters["source"] = LogicalType::VARCHAR;
    fn.named_parameters["commit_every"] = LogicalType::BIGINT;
    fn.named_parameters["count_rows"] = LogicalType::BOOLEAN;
#endif
    fn.projection_pushdown = false;
    return fn;
}

#ifdef FIREBIRD_DUCKDB_V2
// Marks the firebird_execute call that is the whole statement: the
// optimized plan is the call itself, or projections that only choose its
// columns. Anything else (an expression, a filter, a join, UNION ALL, an
// INSERT ... SELECT) runs after the call or beside it, and could fail or
// write after the call's autocommit commit. Runs after DuckDB's optimizers;
// a plan the optimizer never sees (optimizer disabled) marks nothing, so its
// autocommit call is refused.
static void MarkWholeStatement(OptimizerExtensionInput &, unique_ptr<LogicalOperator> &plan) {
    LogicalOperator *op = plan.get();
    // PREPARE ... AS <statement>, and an EXECUTE that binds the prepared
    // statement again: the statement's plan is the child.
    while (op &&
           (op->type == LogicalOperatorType::LOGICAL_PREPARE || op->type == LogicalOperatorType::LOGICAL_EXECUTE) &&
           op->children.size() == 1) {
        op = op->children[0].get();
    }
    while (op && op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        for (auto &expr : op->expressions) {
            if (expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
                return;
            }
        }
        if (op->children.size() != 1) {
            return;
        }
        op = op->children[0].get();
    }
    if (!op || op->type != LogicalOperatorType::LOGICAL_GET) {
        return;
    }
    auto &get = op->Cast<LogicalGet>();
    if (get.function.GetName() == kFunctionName && get.bind_data) {
        get.bind_data->Cast<FirebirdExecuteBindData>().whole_statement = true;
    }
}

void RegisterFirebirdExecuteOptimizer(DBConfig &config) {
    OptimizerExtension extension;
    extension.optimize_function = MarkWholeStatement;
    OptimizerExtension::Register(config, std::move(extension));
}
#endif

} // namespace duckdb
