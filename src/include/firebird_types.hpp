#pragma once

#include "duckdb.hpp"
#include "firebird_client.hpp"

namespace duckdb {

// Maps a Firebird XSQLVAR (sqltype/subtype/scale) to a DuckDB LogicalType.
// Mirrors the conversion in firebird_peregrine_falcon's extractor.rs but
// produces native DuckDB types (DATE/TIMESTAMP/DECIMAL) instead of Arrow.
//
// `numeric_widen_int64` is the G3 opt-in: when true, NUMERIC/DECIMAL columns
// physically stored as a 64-bit scaled integer (SQL_INT64, precision hint 8)
// with a non-zero scale project as DECIMAL(38, scale) instead of the default
// DECIMAL(18, scale), so the full int64 scaled range (±9.22e18) fits without
// silent overflow. Every caller on one scan/ATTACH must pass the same value
// so the declared type and the fetch vector always agree.
LogicalType FirebirdToDuckDBType(const FirebirdColumnDesc &col,
                                 bool numeric_widen_int64 = false);

// Materializes one cell from a fetched row into the destination Vector slot.
// Returns false when the cell was NULL (caller still needs to flag the
// validity bit; this function does so via FlatVector::Validity).
//
// `none_encoding` only matters for text columns whose source-side
// `RDB$CHARACTER_SET_ID = 0` (Firebird CHARACTER SET NONE) — STRICT
// raises on invalid UTF-8, WIN1252/ISO_8859_1 transcode to UTF-8,
// BLOB writes the raw bytes assuming the target Vector is BLOB.
void FirebirdAppendValue(FirebirdStatement &stmt,
                         idx_t col_idx,
                         Vector &target,
                         idx_t target_offset,
                         NoneEncoding none_encoding = NoneEncoding::WIN1252);

// Quotes a Firebird identifier for inclusion in generated SQL. Per the SQL
// standard Firebird upper-cases unquoted identifiers; we always emit
// double-quoted forms to preserve the catalog casing.
std::string QuoteIdent(const std::string &name);

} // namespace duckdb
