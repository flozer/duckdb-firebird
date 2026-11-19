#pragma once

#include <string>
#include <vector>

#include "duckdb.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// False for a value Firebird can't hold as a parameter of its type: a date
// outside 0001-01-01 to 9999-12-31, TIME '24:00:00', an infinite date or
// timestamp. Scan filter pushdown leaves such a constant to DuckDB instead
// of failing the scan.
bool FirebirdCanBindValue(const Value &value);

// Encodes DuckDB values into the input parameters of a prepared Firebird
// statement, one row at a time.
//
// Each value travels in a Firebird type that represents it exactly, and the
// server converts it to the type it described for the parameter, with its
// own rules (rounding, overflow and truncation errors) -- the same
// conversion a literal in the SQL text would get:
//   * integers as BIGINT; DECIMAL(p,s) as a scaled BIGINT (p <= 18) or a
//     scaled INT128 (wider, when the parameter is INT128), with scale s;
//   * FLOAT/DOUBLE, BOOLEAN, DATE, TIME, TIMESTAMP in their own types;
//     TIMESTAMP/TIME WITH TIME ZONE as UTC in the zone types when the
//     parameter has one;
//   * text, and everything else, as VARCHAR (via DuckDB's cast to VARCHAR):
//     the server parses it (DECFLOAT, wide integers, UUID, ...);
//   * values for BLOB parameters as BLOBs created in the statement's
//     transaction (any size).
// Text sent to a CHARACTER SET NONE parameter is encoded per none_encoding
// (WIN1252 / ISO_8859_1 bytes, UTF-8 as is for STRICT, raw for BLOB); a
// character the encoding can't represent is an error.
//
// Every Encode() call sets every field of every parameter (type, length,
// data pointer, NULL indicator): nothing is carried over from the previous
// row.
class FirebirdParamEncoder {
public:
    // `stmt` must be prepared; its parameters are described here.
    // source_types[i] is the DuckDB type of the values for parameter i.
    FirebirdParamEncoder(FirebirdStatement &stmt, const vector<LogicalType> &source_types,
                         NoneEncoding none_encoding);

    idx_t ParamCount() const {
        return slots_.size();
    }

    // Encodes one row; values.size() must equal ParamCount().
    void Encode(const vector<Value> &values);
    // Encodes row `row` of `chunk` (column i -> parameter i).
    void Encode(DataChunk &chunk, idx_t row);

private:
    // WIDE: a 128-bit integer or wide DECIMAL for a parameter that is not
    // INT128: per value, a scaled BIGINT when it fits (trailing zero
    // decimals dropped), else its decimal text.
    enum class Wire {
        INT64,
        INT128,
        WIDE,
        FLOAT,
        DOUBLE,
        BOOLEAN,
        DATE,
        TIME,
        TIMESTAMP,
        TIMESTAMP_TZ,
        TIME_TZ,
        TEXT,
        BLOB
    };

    struct Slot {
        Wire wire = Wire::TEXT;
        FirebirdColumnDesc target;  // as the server described the parameter
        LogicalType source;
        int16_t sqltype = 0;
        int16_t sqlscale = 0;
        int16_t sqlsubtype = 0;
        int16_t sqllen = 0;
        bool none_target = false;  // text for a CHARACTER SET NONE target
        std::vector<char> buffer;
        short indicator = 0;
    };

    void Setup(Slot &slot);
    void EncodeValue(idx_t index, Slot &slot, const Value &value);
    std::string TextBytes(idx_t index, const Slot &slot, const Value &value);

    FirebirdStatement &stmt_;
    NoneEncoding none_encoding_;
    std::vector<Slot> slots_;
    vector<Value> row_;
};

// Encodes `utf8` into the single-byte bytes of a CHARACTER SET NONE target
// (WIN1252 or ISO_8859_1). Returns false when a character has no
// representation in that encoding.
bool FirebirdEncodeNoneText(const std::string &utf8, NoneEncoding mode, std::string &out_bytes);

} // namespace duckdb
