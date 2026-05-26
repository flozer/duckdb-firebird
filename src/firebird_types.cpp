#include "firebird_types.hpp"

#include <cmath>

#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

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
        // IEEE decimal-floating-point (Firebird 4 DECFLOAT). DuckDB has
        // no native equivalent; degrading to DOUBLE keeps the data
        // queryable at the cost of some precision on the long tail.
        return LogicalType::DOUBLE;
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
                         idx_t target_offset) {
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
        // IEEE Decimal64 / Decimal128 — no native DuckDB equivalent yet.
        // Surface NULL rather than producing a misleading approximation.
        FlatVector::SetNull(target, target_offset, true);
        break;
    case SQL_BLOB: {
        auto blob = stmt.ReadBlob(col_idx);
        if (col.sqlsubtype == 1) {
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
