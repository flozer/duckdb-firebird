#pragma once

#include "duckdb.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// RDB$RELATIONS.RDB$RELATION_TYPE: 0/NULL persistent table, 1 view, 2
// external, 3 MON$ snapshot, 4/5 GTT. RDB$VIEW_BLR being non-NULL is the
// canonical "this is a view" signal across Firebird versions, so we lean
// on it as the primary discriminator and fall back to RELATION_TYPE.
// Returns false (with no rows) when the relation does not exist.
bool LookupObjectType(FirebirdConnection &conn,
                     const std::string &upper_table,
                     std::string &out_type);

// Heavy-view analysis result. All best-effort: when the view source can't
// be read safely, `inspected` stays false and the caller emits an explicit
// "definition not inspected" signal instead of guessing.
struct ViewAnalysis {
    bool inspected = false;
    bool has_join = false;
    bool has_group_by = false;
    bool has_aggregate = false;
    bool has_where = false;
};

// Reads RDB$RELATIONS.RDB$VIEW_SOURCE (BLOB SUB_TYPE 1, the original view
// SELECT text) and runs cheap, conservative pattern detection over it. We
// never expose the source text itself — only boolean shape flags drive
// downstream diagnostics. Detection is intentionally shallow (token search
// on an upper-cased copy with string/comment noise stripped), NOT a SQL
// parser: the goal is a factual "this view joins / aggregates / has no
// filter" signal, not plan-level analysis.
//
// Any failure (no source blob, read error, empty text) returns
// inspected=false so the caller can say so explicitly rather than imply a
// clean simple view.
ViewAnalysis AnalyzeViewSource(FirebirdConnection &conn,
                               const std::string &upper_table);

// Reconciles column_types/column_descs for a VIEW target from a live,
// execute-free XSQLDA describe, correcting for the case where the view's
// catalog-frozen column metadata (RDB$FIELDS, set at CREATE VIEW time)
// disagrees with what Firebird's live DSQL compiler produces for the
// identical projected expression today (e.g. SUM() promotion widening or
// narrowing a NUMERIC column's effective storage width). column_types/
// column_descs are mutated IN PLACE only when the target is a view AND
// the live describe succeeds with a matching column count;
// character_set_id is always preserved from the original column_descs
// entries (XSQLDA cannot supply it for BLOB columns — that slot carries
// the blob subtype instead). Best-effort: any failure (exception, column
// count mismatch) leaves column_types/column_descs unchanged.
//
// Returns true iff the target is a view (regardless of whether
// reconciliation changed anything) — callers that also need "is this a
// view" for other purposes (e.g. pagination-safety decisions) can reuse
// this return value instead of paying for a second LookupObjectType
// call.
bool ReconcileViewColumnTypes(FirebirdConnection &conn,
                              const std::string &table_name,
                              const duckdb::vector<std::string> &column_names,
                              duckdb::vector<LogicalType> &column_types,
                              duckdb::vector<FirebirdColumnDesc> &column_descs);

} // namespace duckdb
