#pragma once

#include "duckdb.hpp"
#include "firebird_client.hpp"

namespace duckdb {

// Maps a Firebird XSQLVAR (sqltype/subtype/scale) to a DuckDB LogicalType.
// Mirrors the conversion in firebird_peregrine_falcon's extractor.rs but
// produces native DuckDB types (DATE/TIMESTAMP/DECIMAL) instead of Arrow.
LogicalType FirebirdToDuckDBType(const FirebirdColumnDesc &col);

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
                         NoneEncoding none_encoding = NoneEncoding::STRICT);

// Quotes a Firebird identifier for inclusion in generated SQL. Per the SQL
// standard Firebird upper-cases unquoted identifiers; we always emit
// double-quoted forms to preserve the catalog casing.
std::string QuoteIdent(const std::string &name);

} // namespace duckdb
