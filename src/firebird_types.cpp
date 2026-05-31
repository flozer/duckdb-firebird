#include "firebird_types.hpp"

#include <cmath>
#include <cstdint>

#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

// --- character-set helpers ---------------------------------------------------
//
// Reaches the row-fetch path for columns whose Firebird storage CHARACTER
// SET is NONE (id = 0). Firebird does not transliterate NONE columns to
// the client's lc_ctype on the wire, so we receive whatever bytes the
// writing application stored.

// Returns true when every byte in [data, data+len) forms a valid UTF-8
// sequence according to RFC 3629 (no overlongs, no surrogates).
static bool IsValidUtf8(const char *data, size_t len) {
    auto p = reinterpret_cast<const unsigned char *>(data);
    const auto end = p + len;
    while (p < end) {
        const unsigned char c = *p;
        if (c < 0x80) { ++p; continue; }
        size_t want;
        unsigned int min_codepoint;
        if      ((c & 0xE0) == 0xC0) { want = 1; min_codepoint = 0x80; }
        else if ((c & 0xF0) == 0xE0) { want = 2; min_codepoint = 0x800; }
        else if ((c & 0xF8) == 0xF0) { want = 3; min_codepoint = 0x10000; }
        else return false;
        if (p + 1 + want > end) return false;
        unsigned int cp = c & ((1u << (6 - want)) - 1);
        for (size_t i = 1; i <= want; ++i) {
            if ((p[i] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (cp < min_codepoint) return false;          // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return false; // surrogates
        if (cp > 0x10FFFF) return false;
        p += 1 + want;
    }
    return true;
}

// Append the UTF-8 encoding of `cp` (a Unicode code point in [0, 0x10FFFF])
// to `out`. Only used for input code points already known to be valid.
static void AppendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Windows-1252 → Unicode for the 0x80..0x9F range (the rest of 0x80..0xFF
// matches Latin-1 directly). Five positions are undefined (0x81, 0x8D,
// 0x8F, 0x90, 0x9D) — we preserve the input byte as a Latin-1 round-trip
// to avoid silent data loss.
static uint32_t Cp1252HighByteToCodePoint(unsigned char b) {
    static const uint16_t MAP[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    };
    return MAP[b - 0x80];
}

static std::string Win1252ToUtf8(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char b : in) {
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
        } else if (b < 0xA0) {
            AppendUtf8(out, Cp1252HighByteToCodePoint(b));
        } else {
            AppendUtf8(out, b);  // 0xA0..0xFF: Latin-1 == Unicode
        }
    }
    return out;
}

static std::string Latin1ToUtf8(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char b : in) {
        if (b < 0x80) out.push_back(static_cast<char>(b));
        else          AppendUtf8(out, b);
    }
    return out;
}

// Decide what to do when a text column whose Firebird charset is NONE
// surfaces a byte string. Returns the UTF-8 string to ingest, or throws
// IOException if `mode == STRICT` and the bytes aren't already valid
// UTF-8. The BLOB mode is handled at the call site (target is BLOB and
// receives the raw bytes directly).
static std::string TranscodeNoneCharset(const std::string &raw,
                                        NoneEncoding mode,
                                        const std::string &col_name) {
    switch (mode) {
    case NoneEncoding::STRICT:
        if (IsValidUtf8(raw.data(), raw.size())) return raw;
        throw IOException(
            "firebird: column '" + col_name + "' has Firebird CHARACTER SET "
            "NONE and the row's bytes are not valid UTF-8 under strict mode. "
            "For legacy Brazilian / Western-European ERPs (Athenas, IBExpert "
            "exports, Delphi-era apps), pass none_encoding='win1252' — that "
            "is also the default when this option is omitted. Other choices: "
            "none_encoding='iso8859_1' (alias 'latin1') for pure Latin-1 "
            "inputs, or none_encoding='blob' to surface raw bytes. See "
            "README -> 'Charset handling: CHARACTER SET NONE'.");
    case NoneEncoding::WIN1252:    return Win1252ToUtf8(raw);
    case NoneEncoding::ISO_8859_1: return Latin1ToUtf8(raw);
    case NoneEncoding::BLOB:
        // Caller materialises the BLOB directly without going through here.
        return raw;
    }
    return raw;
}

// Firebird stores NUMERIC/DECIMAL as scaled integers (sqlscale is the negative
// power-of-10 exponent). For widths up to DECIMAL(38,…) we can use DuckDB's
// native DECIMAL; otherwise we degrade to DOUBLE.
static LogicalType ScaledIntegerToDecimal(int precision_hint, int sqlscale) {
    if (sqlscale == 0) {
        // Plain integer.
        switch (precision_hint) {
        case 2:  return LogicalType::SMALLINT;
        case 4:  return LogicalType::INTEGER;
        default: return LogicalType::BIGINT;
        }
    }
    // Convert "10**sqlscale" exponent into DECIMAL(width, scale).
    int scale = -sqlscale;
    int width = 0;
    switch (precision_hint) {
    case 2:  width = 4;  break;   // SMALLINT-backed → up to 4 digits
    case 4:  width = 9;  break;   // INTEGER-backed  → up to 9 digits
    case 8:  width = 18; break;   // BIGINT-backed   → up to 18 digits
    default: width = 18;
    }
    if (scale < 0 || scale > width) {
        return LogicalType::DOUBLE;
    }
    return LogicalType::DECIMAL(width, scale);
}

LogicalType FirebirdToDuckDBType(const FirebirdColumnDesc &col) {
    switch (col.sqltype) {
    case SQL_TEXT:
    case SQL_VARYING:
        return LogicalType::VARCHAR;
    case SQL_SHORT:    return ScaledIntegerToDecimal(2, col.sqlscale);
    case SQL_LONG:     return ScaledIntegerToDecimal(4, col.sqlscale);
    case SQL_INT64:    return ScaledIntegerToDecimal(8, col.sqlscale);
    case SQL_INT128:
        // Firebird 4 INT128 — scale 0 → HUGEINT; with scale → DECIMAL(38, -scale).
        // Firebird always reports scale <= 0 in RDB$FIELD_SCALE.
        if (col.sqlscale == 0) return LogicalType::HUGEINT;
        if (-col.sqlscale > 38) return LogicalType::DOUBLE;
        return LogicalType::DECIMAL(38, -col.sqlscale);
    case SQL_FLOAT:    return LogicalType::FLOAT;
    case SQL_D_FLOAT:
    case SQL_DOUBLE:   return LogicalType::DOUBLE;
    case SQL_DEC16:
    case SQL_DEC34:
        // IEEE decimal-floating-point (Firebird 4 DECFLOAT(16/34),
        // Decimal64/Decimal128). DuckDB has no native equivalent and the
        // legacy isc_/XSQLDA path has no decimal-float decoder, so the
        // scanner projects these columns as CAST(... AS VARCHAR(64))
        // server-side (see FirebirdQueryBuilder::Build). Surface them as
        // VARCHAR: lossless and honest, instead of the previous DOUBLE
        // schema that silently fetched NULL. A lossy numeric fast path
        // would be a future opt-in.
        return LogicalType::VARCHAR;
    case SQL_TIMESTAMP:       return LogicalType::TIMESTAMP;
    case SQL_TIMESTAMP_TZ:
    case SQL_TIMESTAMP_TZ_EX: return LogicalType::TIMESTAMP_TZ;
    case SQL_TYPE_DATE:       return LogicalType::DATE;
    case SQL_TYPE_TIME:       return LogicalType::TIME;
    case SQL_TIME_TZ:
    case SQL_TIME_TZ_EX:      return LogicalType::TIME_TZ;
    case SQL_BOOLEAN:      return LogicalType::BOOLEAN;
    case SQL_BLOB:
        // sub_type 1 = TEXT BLOB; everything else is binary.
        return col.sqlsubtype == 1 ? LogicalType::VARCHAR : LogicalType::BLOB;
    default:
        return LogicalType::VARCHAR;
    }
}

// Helper: write a scaled integer (Firebird NUMERIC/DECIMAL) into a DuckDB
// DECIMAL vector slot. The DECIMAL is physically stored as the underlying
// integer width, so the scaled int from Firebird is already the right value.
template <class T>
static void StoreDecimal(Vector &target, idx_t offset, T value) {
    auto data = FlatVector::GetData<T>(target);
    data[offset] = value;
}

void FirebirdAppendValue(FirebirdStatement &stmt,
                         idx_t col_idx,
                         Vector &target,
                         idx_t target_offset,
                         NoneEncoding none_encoding) {
    if (stmt.IsNull(col_idx)) {
        FlatVector::SetNull(target, target_offset, true);
        return;
    }

    const auto &col = stmt.columns()[col_idx];
    const auto &dst_type = target.GetType();

    switch (col.sqltype) {
    case SQL_TEXT:
    case SQL_VARYING: {
        auto str = stmt.GetText(col_idx);
        // CHARACTER SET NONE (id=0) bytes need either UTF-8 validation
        // (STRICT), a transcode (WIN1252 / ISO_8859_1), or to be stored as
        // BLOB raw. When the col's charset_id is unknown (-1, e.g. probe
        // came from XSQLDA without a metadata fetch) we leave the bytes
        // alone and let DuckDB's append handle them.
        if (col.character_set_id == 0) {
            if (none_encoding == NoneEncoding::BLOB) {
                FlatVector::GetData<string_t>(target)[target_offset] =
                    StringVector::AddStringOrBlob(target, str);
                break;
            }
            str = TranscodeNoneCharset(str, none_encoding, col.name);
        }
        FlatVector::GetData<string_t>(target)[target_offset] =
            StringVector::AddString(target, str);
        break;
    }
    case SQL_SHORT: {
        int16_t v = stmt.GetShort(col_idx);
        if (dst_type.id() == LogicalTypeId::DECIMAL) {
            // DECIMAL with internal width SMALLINT.
            StoreDecimal<int16_t>(target, target_offset, v);
        } else {
            FlatVector::GetData<int16_t>(target)[target_offset] = v;
        }
        break;
    }
    case SQL_LONG: {
        int32_t v = stmt.GetLong(col_idx);
        if (dst_type.id() == LogicalTypeId::DECIMAL) {
            StoreDecimal<int32_t>(target, target_offset, v);
        } else {
            FlatVector::GetData<int32_t>(target)[target_offset] = v;
        }
        break;
    }
    case SQL_INT64: {
        int64_t v = stmt.GetInt64(col_idx);
        if (dst_type.id() == LogicalTypeId::DECIMAL) {
            StoreDecimal<int64_t>(target, target_offset, v);
        } else {
            FlatVector::GetData<int64_t>(target)[target_offset] = v;
        }
        break;
    }
    case SQL_FLOAT:
        FlatVector::GetData<float>(target)[target_offset] = stmt.GetFloat(col_idx);
        break;
    case SQL_D_FLOAT:
    case SQL_DOUBLE:
        FlatVector::GetData<double>(target)[target_offset] = stmt.GetDouble(col_idx);
        break;
    case SQL_BOOLEAN:
        FlatVector::GetData<bool>(target)[target_offset] = stmt.GetBool(col_idx);
        break;
    case SQL_TIMESTAMP: {
        auto ts = stmt.GetTimestamp(col_idx);
        // Firebird timestamp = (date in days since 1858-11-17,
        //                       time in 1/10000 of a second).
        date_t      d = Date::FromDate(1858, 11, 17);
        d.days       += ts.timestamp_date;
        dtime_t     t(static_cast<int64_t>(ts.timestamp_time) * 100);  // -> microseconds
        FlatVector::GetData<timestamp_t>(target)[target_offset] =
            Timestamp::FromDatetime(d, t);
        break;
    }
    case SQL_TYPE_DATE: {
        auto d  = stmt.GetDate(col_idx);
        date_t out = Date::FromDate(1858, 11, 17);
        out.days  += d;
        FlatVector::GetData<date_t>(target)[target_offset] = out;
        break;
    }
    case SQL_TYPE_TIME: {
        auto t = stmt.GetTime(col_idx);
        FlatVector::GetData<dtime_t>(target)[target_offset] =
            dtime_t(static_cast<int64_t>(t) * 100);  // 1/10000s → microseconds
        break;
    }
    case SQL_INT128: {
        uint64_t lo;
        int64_t  hi;
        stmt.GetInt128(col_idx, lo, hi);
        // hugeint_t is {uint64_t lower; int64_t upper;} — same layout
        // Firebird writes. DECIMAL(>18,…) is also backed by hugeint_t.
        hugeint_t v;
        v.lower = lo;
        v.upper = hi;
        FlatVector::GetData<hugeint_t>(target)[target_offset] = v;
        break;
    }
    case SQL_TIMESTAMP_TZ:
    case SQL_TIMESTAMP_TZ_EX: {
        // EX variants add a session-offset field after the (date,time,zoneId)
        // triple, but the first 10 bytes are layout-compatible — the UTC
        // accessor reads only those.
        auto ts = stmt.GetTimestampTzUtc(col_idx);
        date_t  d = Date::FromDate(1858, 11, 17);
        d.days   += ts.timestamp_date;
        dtime_t t(static_cast<int64_t>(ts.timestamp_time) * 100);
        FlatVector::GetData<timestamp_tz_t>(target)[target_offset] =
            timestamp_tz_t(Timestamp::FromDatetime(d, t));
        break;
    }
    case SQL_TIME_TZ:
    case SQL_TIME_TZ_EX: {
        auto t = stmt.GetTimeTzUtc(col_idx);
        // Firebird stores the UTC component; surface it with a zero
        // offset (DuckDB's dtime_tz_t encodes time+offset in 64 bits).
        FlatVector::GetData<dtime_tz_t>(target)[target_offset] =
            dtime_tz_t(dtime_t(static_cast<int64_t>(t) * 100), 0);
        break;
    }
    case SQL_DEC16:
    case SQL_DEC34:
        // Unreachable on the normal scan path: the query builder projects
        // DECFLOAT columns as CAST(... AS VARCHAR(64)), so they arrive here
        // as SQL_VARYING and are handled by the text path above. This
        // branch remains only as a defensive guard if a DECFLOAT column
        // ever reaches fetch uncast (e.g. a future code path that bypasses
        // the builder); we surface NULL rather than mis-decode the raw
        // Decimal64/Decimal128 bytes. The builder change makes the common
        // case lossless VARCHAR instead of this silent NULL.
        FlatVector::SetNull(target, target_offset, true);
        break;
    case SQL_BLOB: {
        auto blob = stmt.ReadBlob(col_idx);
        if (col.sqlsubtype == 1) {
            // Text BLOB. Mirror the SQL_VARYING / SQL_TEXT path for
            // NONE-charset columns so the same none_encoding policy
            // applies (otherwise we'd accept invalid UTF-8 silently
            // through BLOBs while the matching VARCHAR path rejects).
            if (col.character_set_id == 0) {
                if (none_encoding == NoneEncoding::BLOB) {
                    FlatVector::GetData<string_t>(target)[target_offset] =
                        StringVector::AddStringOrBlob(target, blob);
                    break;
                }
                blob = TranscodeNoneCharset(blob, none_encoding, col.name);
            }
            FlatVector::GetData<string_t>(target)[target_offset] =
                StringVector::AddString(target, blob);
        } else {
            FlatVector::GetData<string_t>(target)[target_offset] =
                StringVector::AddStringOrBlob(target, blob);
        }
        break;
    }
    default:
        FlatVector::SetNull(target, target_offset, true);
        break;
    }
}

std::string QuoteIdent(const std::string &name) {
    std::string out;
    out.reserve(name.size() + 2);
    out.push_back('"');
    for (char c : name) {
        if (c == '"') out.push_back('"');  // escape embedded quote
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace duckdb
