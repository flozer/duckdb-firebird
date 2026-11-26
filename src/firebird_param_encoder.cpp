#include "firebird_param_encoder.hpp"

#include <cstring>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

namespace {

// Firebird dates count days from 1858-11-17 (MJD); DuckDB's from 1970-01-01.
constexpr int32_t kMjdToUnixDays = 40587;
// Firebird's time zone id for GMT/UTC (TimeZoneUtil::GMT_ZONE).
constexpr uint16_t kGmtZone = 65535;
// Capacity of a VARCHAR carrying a non-text value (numbers, dates, ...).
constexpr int16_t kTextForOtherTypes = 1024;

bool IsTextType(int16_t sqltype) {
    return sqltype == SQL_TEXT || sqltype == SQL_VARYING;
}

bool IsIntegerType(int16_t sqltype) {
    return sqltype == SQL_SHORT || sqltype == SQL_LONG || sqltype == SQL_INT64 || sqltype == SQL_INT128;
}

bool IsDecfloatType(int16_t sqltype) {
    return sqltype == SQL_DEC16 || sqltype == SQL_DEC34;
}

int64_t TimeMicros(dtime_t t) {
#ifdef FIREBIRD_DUCKDB_V2
    return t.value;
#else
    return t.micros;
#endif
}

template <class T>
void Put(std::vector<char> &buffer, const T &value, size_t offset = 0) {
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

} // namespace

FirebirdParamEncoder::FirebirdParamEncoder(FirebirdStatement &stmt, const vector<LogicalType> &source_types,
                                           NoneEncoding none_encoding)
    : stmt_(stmt), none_encoding_(none_encoding) {
    const auto &params = stmt_.DescribeInput();
    if (params.size() != source_types.size()) {
        throw InvalidInputException("the statement has %llu parameter(s) (?) but %llu value(s) were given",
                                    static_cast<unsigned long long>(params.size()),
                                    static_cast<unsigned long long>(source_types.size()));
    }
    slots_.resize(params.size());
    for (idx_t i = 0; i < params.size(); ++i) {
        slots_[i].target = params[i];
        slots_[i].source = source_types[i];
        Setup(slots_[i]);
    }
    row_.resize(params.size());
}

void FirebirdParamEncoder::Setup(Slot &slot) {
    const auto &t = slot.target;
    const auto &src = slot.source;
    const bool target_text = IsTextType(t.sqltype);
    const bool target_int = IsIntegerType(t.sqltype);
    const bool target_decfloat = IsDecfloatType(t.sqltype);
    const bool target_tstz = t.sqltype == SQL_TIMESTAMP_TZ || t.sqltype == SQL_TIMESTAMP_TZ_EX;
    const bool target_ttz = t.sqltype == SQL_TIME_TZ || t.sqltype == SQL_TIME_TZ_EX;

    if (t.sqltype == SQL_BLOB) {
        slot.wire = Wire::BLOB;
    } else {
        switch (src.id()) {
        case LogicalTypeId::BOOLEAN:
            slot.wire = target_int ? Wire::INT64 : Wire::BOOLEAN;
            break;
        case LogicalTypeId::TINYINT:
        case LogicalTypeId::SMALLINT:
        case LogicalTypeId::INTEGER:
        case LogicalTypeId::BIGINT:
        case LogicalTypeId::UTINYINT:
        case LogicalTypeId::USMALLINT:
        case LogicalTypeId::UINTEGER:
            slot.wire = target_decfloat ? Wire::TEXT : Wire::INT64;
            break;
        case LogicalTypeId::UBIGINT:
        case LogicalTypeId::HUGEINT:
        case LogicalTypeId::UHUGEINT:
            slot.wire = target_decfloat ? Wire::TEXT : t.sqltype == SQL_INT128 ? Wire::INT128 : Wire::WIDE;
            break;
        case LogicalTypeId::DECIMAL:
            if (target_decfloat) {
                slot.wire = Wire::TEXT;
            } else if (DecimalType::GetWidth(src) <= 18) {
                slot.wire = Wire::INT64;
                slot.sqlscale = static_cast<int16_t>(-DecimalType::GetScale(src));
            } else if (t.sqltype == SQL_INT128) {
                slot.wire = Wire::INT128;
                slot.sqlscale = static_cast<int16_t>(-DecimalType::GetScale(src));
            } else {
                // Firebird 3 overflows converting text with more digits than
                // the target holds (before rescaling), so not text first.
                slot.wire = Wire::WIDE;
            }
            break;
        case LogicalTypeId::FLOAT:
            slot.wire = target_decfloat ? Wire::TEXT : Wire::FLOAT;
            break;
        case LogicalTypeId::DOUBLE:
            slot.wire = target_decfloat ? Wire::TEXT : Wire::DOUBLE;
            break;
        case LogicalTypeId::DATE:
            slot.wire = Wire::DATE;
            break;
        case LogicalTypeId::TIME:
            slot.wire = Wire::TIME;
            break;
        case LogicalTypeId::TIMESTAMP:
        case LogicalTypeId::TIMESTAMP_SEC:
        case LogicalTypeId::TIMESTAMP_MS:
        case LogicalTypeId::TIMESTAMP_NS:
            slot.wire = Wire::TIMESTAMP;
            break;
        case LogicalTypeId::TIMESTAMP_TZ:
            // Without a zone type to receive it, the UTC wall-clock time.
            slot.wire = target_tstz ? Wire::TIMESTAMP_TZ : Wire::TIMESTAMP;
            break;
        case LogicalTypeId::TIME_TZ:
            slot.wire = target_ttz ? Wire::TIME_TZ : Wire::TIME;
            break;
        default:
            // VARCHAR, BLOB, UUID, INTERVAL, ...: as text.
            slot.wire = Wire::TEXT;
            break;
        }
    }

    if (slot.wire == Wire::WIDE && !target_text && FirebirdClientMajor() >= 4 &&
        stmt_.Connection().EngineMajor() >= 4) {
        // Firebird 4+ client and server: INT128 with the DECIMAL's own scale,
        // which the server rounds to the target as it does the literal. WIDE
        // would send a value whose digits don't fit in 64 bits as text, and
        // the server overflows converting text with more digits than the
        // target holds (1.12345678901234567891 into NUMERIC(18,4)). Firebird
        // 3 has no INT128 and refuses that literal too; a Firebird 3 client
        // can't send SQL_INT128 even to a Firebird 4 server.
        slot.wire = Wire::INT128;
        slot.sqlscale = src.id() == LogicalTypeId::DECIMAL
                            ? static_cast<int16_t>(-DecimalType::GetScale(src))
                            : static_cast<int16_t>(0);
    }

    switch (slot.wire) {
    case Wire::INT64:
        slot.sqltype = SQL_INT64;
        slot.sqllen = 8;
        break;
    case Wire::INT128:
        slot.sqltype = SQL_INT128;
        slot.sqllen = 16;
        break;
    case Wire::WIDE:
        // Type, scale and length are set per value; the buffer holds the
        // larger form (text).
        slot.sqltype = SQL_VARYING;
        slot.sqllen = kTextForOtherTypes;
        break;
    case Wire::FLOAT:
        slot.sqltype = SQL_FLOAT;
        slot.sqllen = 4;
        break;
    case Wire::DOUBLE:
        slot.sqltype = SQL_DOUBLE;
        slot.sqllen = 8;
        break;
    case Wire::BOOLEAN:
        slot.sqltype = SQL_BOOLEAN;
        slot.sqllen = 1;
        break;
    case Wire::DATE:
        slot.sqltype = SQL_TYPE_DATE;
        slot.sqllen = 4;
        break;
    case Wire::TIME:
        slot.sqltype = SQL_TYPE_TIME;
        slot.sqllen = 4;
        break;
    case Wire::TIMESTAMP:
        slot.sqltype = SQL_TIMESTAMP;
        slot.sqllen = 8;
        break;
    case Wire::TIMESTAMP_TZ:
        // ISC_TIMESTAMP_TZ: UTC date, UTC time, zone id (12 bytes aligned).
        slot.sqltype = SQL_TIMESTAMP_TZ;
        slot.sqllen = 12;
        break;
    case Wire::TIME_TZ:
        // ISC_TIME_TZ: UTC time, zone id (8 bytes aligned).
        slot.sqltype = SQL_TIME_TZ;
        slot.sqllen = 8;
        break;
    case Wire::TEXT:
        slot.sqltype = SQL_VARYING;
        if (target_text) {
            // The target's own character set (the connection's, or NONE /
            // OCTETS): the server transliterates from it as for a literal.
            slot.sqlsubtype = t.sqlsubtype;
            slot.sqllen = t.sqllen > 0 ? t.sqllen : 1;
            slot.none_target = t.character_set_id == 0;
        } else {
            slot.sqlsubtype = src.id() == LogicalTypeId::BLOB ? 1 /* OCTETS */ : 0;
            slot.sqllen = kTextForOtherTypes;
        }
        break;
    case Wire::BLOB:
        slot.sqltype = SQL_BLOB;
        slot.sqlsubtype = t.sqlsubtype;
        slot.sqlscale = t.sqlscale;
        slot.sqllen = 8;
        slot.none_target = t.sqlsubtype == 1 && t.character_set_id == 0;
        break;
    }
    const size_t size = slot.sqltype == SQL_VARYING ? static_cast<size_t>(slot.sqllen) + sizeof(ISC_USHORT)
                                                    : static_cast<size_t>(slot.sqllen);
    slot.buffer.assign(size, 0);
}

std::string FirebirdParamEncoder::TextBytes(idx_t index, const Slot &slot, const Value &value) {
    if (slot.source.id() == LogicalTypeId::BLOB) {
        return StringValue::Get(value);  // raw bytes
    }
    std::string text = slot.source.id() == LogicalTypeId::VARCHAR
                           ? StringValue::Get(value)
                           : StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
    if (slot.none_target &&
        (none_encoding_ == NoneEncoding::WIN1252 || none_encoding_ == NoneEncoding::ISO_8859_1)) {
        std::string bytes;
        if (!FirebirdEncodeNoneText(text, none_encoding_, bytes)) {
            throw InvalidInputException(
                "parameter %llu ('%s') is CHARACTER SET NONE and the value has a character %s "
                "cannot represent; set none_encoding = 'strict' to send UTF-8 bytes",
                static_cast<unsigned long long>(index + 1), slot.target.name,
                none_encoding_ == NoneEncoding::WIN1252 ? "WIN1252" : "ISO-8859-1");
        }
        return bytes;
    }
    return text;
}

// Firebird's dates run from 0001-01-01 to 9999-12-31; DuckDB's go further
// (BC dates, years past 9999), and the raw ISC_DATE of those is not a date
// Firebird can hold.
static bool InFirebirdDates(date_t d) {
    static const date_t kMin = Date::FromDate(1, 1, 1);
    static const date_t kMax = Date::FromDate(9999, 12, 31);
    return d >= kMin && d <= kMax;
}

constexpr int64_t kDayMicros = 86400LL * 1000000LL;

// utc: the date of a TIMESTAMPTZ, shifted to UTC.
static void CheckFirebirdDate(idx_t index, date_t d, bool utc = false) {
    if (!InFirebirdDates(d)) {
        throw InvalidInputException("parameter %llu: %s%s is outside Firebird's dates (0001-01-01 to 9999-12-31)",
                                    static_cast<unsigned long long>(index + 1), Date::ToString(d),
                                    utc ? " (in UTC)" : "");
    }
}

static void CheckFirebirdTime(idx_t index, int64_t micros) {
    if (micros >= kDayMicros) {
        throw InvalidInputException("parameter %llu: TIME '24:00:00' is outside Firebird's times "
                                    "(00:00:00 to 23:59:59.9999)",
                                    static_cast<unsigned long long>(index + 1));
    }
}

bool FirebirdCanBindValue(const Value &value) {
    if (value.IsNull()) {
        return true;
    }
    switch (value.type().id()) {
    case LogicalTypeId::DATE: {
        const date_t d = value.GetValue<date_t>();
        return d != date_t::infinity() && d != date_t::ninfinity() && InFirebirdDates(d);
    }
    case LogicalTypeId::TIMESTAMP: {
        const timestamp_t ts = value.GetValue<timestamp_t>();
        if (ts == timestamp_t::infinity() || ts == timestamp_t::ninfinity()) {
            return false;
        }
        date_t d;
        dtime_t t;
        Timestamp::Convert(ts, d, t);
        return InFirebirdDates(d);
    }
    case LogicalTypeId::TIME:
        return TimeMicros(value.GetValue<dtime_t>()) < kDayMicros;
    default:
        return true;
    }
}

void FirebirdParamEncoder::EncodeValue(idx_t index, Slot &slot, const Value &value) {
    XSQLVAR &x = stmt_.InputSqlda()->sqlvar[index];
    // Every field, every row: nothing from the previous row survives.
    x.sqltype = static_cast<short>(slot.sqltype | 1);
    x.sqlscale = slot.sqlscale;
    x.sqlsubtype = slot.sqlsubtype;
    x.sqllen = slot.sqllen;
    x.sqldata = slot.buffer.data();
    x.sqlind = &slot.indicator;
    if (value.IsNull()) {
        slot.indicator = -1;
        return;
    }
    slot.indicator = 0;

    switch (slot.wire) {
    case Wire::INT64: {
        int64_t v;
        if (slot.source.id() == LogicalTypeId::DECIMAL) {
            switch (slot.source.InternalType()) {
            case PhysicalType::INT16:
                v = value.GetValueUnsafe<int16_t>();
                break;
            case PhysicalType::INT32:
                v = value.GetValueUnsafe<int32_t>();
                break;
            default:
                v = value.GetValueUnsafe<int64_t>();
                break;
            }
        } else if (slot.source.id() == LogicalTypeId::BOOLEAN) {
            v = value.GetValue<bool>() ? 1 : 0;
        } else {
            v = value.GetValue<int64_t>();
        }
        Put(slot.buffer, v);
        break;
    }
    case Wire::INT128: {
        hugeint_t h = slot.source.id() == LogicalTypeId::DECIMAL
                          ? value.GetValueUnsafe<hugeint_t>()
                          : value.DefaultCastAs(LogicalType::HUGEINT).GetValue<hugeint_t>();
        Put(slot.buffer, h.lower, 0);
        Put(slot.buffer, h.upper, 8);
        break;
    }
    case Wire::WIDE: {
        hugeint_t h;
        uint8_t scale = 0;
        if (slot.source.id() == LogicalTypeId::DECIMAL) {
            h = value.GetValueUnsafe<hugeint_t>();
            scale = DecimalType::GetScale(slot.source);
        } else {
            h = value.DefaultCastAs(LogicalType::HUGEINT).GetValue<hugeint_t>();
        }
        // Trailing zero decimals carry no value.
        while (scale > 0 && h % hugeint_t(10) == hugeint_t(0)) {
            h /= hugeint_t(10);
            --scale;
        }
        int64_t small;
        if (Hugeint::TryCast(h, small)) {
            x.sqltype = SQL_INT64 | 1;
            x.sqlscale = static_cast<short>(-static_cast<int>(scale));
            x.sqlsubtype = 0;
            x.sqllen = 8;
            Put(slot.buffer, small);
        } else {
            const std::string text = Decimal::ToString(h, 38, scale);
            x.sqltype = SQL_VARYING | 1;
            x.sqlscale = 0;
            x.sqlsubtype = 0;
            x.sqllen = kTextForOtherTypes;
            const ISC_USHORT len = static_cast<ISC_USHORT>(text.size());
            Put(slot.buffer, len, 0);
            std::memcpy(slot.buffer.data() + sizeof(ISC_USHORT), text.data(), text.size());
        }
        break;
    }
    case Wire::FLOAT:
        Put(slot.buffer, value.GetValue<float>());
        break;
    case Wire::DOUBLE:
        Put(slot.buffer, value.GetValue<double>());
        break;
    case Wire::BOOLEAN: {
        const ISC_UCHAR b = value.GetValue<bool>() ? 1 : 0;
        Put(slot.buffer, b);
        break;
    }
    case Wire::DATE: {
        const date_t d = value.GetValue<date_t>();
        if (d == date_t::infinity() || d == date_t::ninfinity()) {
            throw InvalidInputException("parameter %llu: infinite dates can't be sent to Firebird",
                                        static_cast<unsigned long long>(index + 1));
        }
        CheckFirebirdDate(index, d);
        const ISC_DATE fb = static_cast<ISC_DATE>(d.days + kMjdToUnixDays);
        Put(slot.buffer, fb);
        break;
    }
    case Wire::TIME: {
        int64_t micros;
        if (slot.source.id() == LogicalTypeId::TIME_TZ) {
            const dtime_tz_t tz = value.GetValue<dtime_tz_t>();
            CheckFirebirdTime(index, TimeMicros(tz.time()));
            micros = TimeMicros(tz.time()) - static_cast<int64_t>(tz.offset()) * 1000000LL;
            micros = ((micros % kDayMicros) + kDayMicros) % kDayMicros;
        } else {
            micros = TimeMicros(value.GetValue<dtime_t>());
            CheckFirebirdTime(index, micros);
        }
        // Firebird keeps 1/10000 s: sub-100-microsecond digits are dropped.
        const ISC_TIME fb = static_cast<ISC_TIME>(micros / 100);
        Put(slot.buffer, fb);
        break;
    }
    case Wire::TIMESTAMP:
    case Wire::TIMESTAMP_TZ: {
        timestamp_t ts;
        switch (slot.source.id()) {
        case LogicalTypeId::TIMESTAMP:
        case LogicalTypeId::TIMESTAMP_TZ:
            ts = timestamp_t(value.GetValueUnsafe<int64_t>());
            break;
        default:
            ts = value.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
            break;
        }
        if (ts == timestamp_t::infinity() || ts == timestamp_t::ninfinity()) {
            throw InvalidInputException("parameter %llu: infinite timestamps can't be sent to Firebird",
                                        static_cast<unsigned long long>(index + 1));
        }
        date_t d;
        dtime_t t;
        Timestamp::Convert(ts, d, t);
        CheckFirebirdDate(index, d, slot.source.id() == LogicalTypeId::TIMESTAMP_TZ);
        const ISC_DATE fb_date = static_cast<ISC_DATE>(d.days + kMjdToUnixDays);
        const ISC_TIME fb_time = static_cast<ISC_TIME>(TimeMicros(t) / 100);
        Put(slot.buffer, fb_date, 0);
        Put(slot.buffer, fb_time, 4);
        if (slot.wire == Wire::TIMESTAMP_TZ) {
            Put(slot.buffer, kGmtZone, 8);
        }
        break;
    }
    case Wire::TIME_TZ: {
        const dtime_tz_t tz = value.GetValue<dtime_tz_t>();
        CheckFirebirdTime(index, TimeMicros(tz.time()));
        int64_t micros = TimeMicros(tz.time()) - static_cast<int64_t>(tz.offset()) * 1000000LL;
        micros = ((micros % kDayMicros) + kDayMicros) % kDayMicros;
        const ISC_TIME fb = static_cast<ISC_TIME>(micros / 100);
        Put(slot.buffer, fb, 0);
        Put(slot.buffer, kGmtZone, 4);
        break;
    }
    case Wire::TEXT: {
        const std::string bytes = TextBytes(index, slot, value);
        if (bytes.size() > static_cast<size_t>(slot.sqllen)) {
            throw InvalidInputException("parameter %llu ('%s'): the value is %llu bytes, the parameter takes "
                                        "at most %d",
                                        static_cast<unsigned long long>(index + 1), slot.target.name,
                                        static_cast<unsigned long long>(bytes.size()), slot.sqllen);
        }
        const ISC_USHORT len = static_cast<ISC_USHORT>(bytes.size());
        Put(slot.buffer, len, 0);
        if (!bytes.empty()) {
            std::memcpy(slot.buffer.data() + sizeof(ISC_USHORT), bytes.data(), bytes.size());
        }
        break;
    }
    case Wire::BLOB: {
        const std::string bytes = TextBytes(index, slot, value);
        const ISC_QUAD id =
            stmt_.Connection().CreateBlob(bytes, slot.target.sqlsubtype, slot.target.character_set_id);
        Put(slot.buffer, id);
        break;
    }
    }
}

void FirebirdParamEncoder::Encode(const vector<Value> &values) {
    if (values.size() != slots_.size()) {
        throw InvalidInputException("expected %llu parameter value(s), got %llu",
                                    static_cast<unsigned long long>(slots_.size()),
                                    static_cast<unsigned long long>(values.size()));
    }
    for (idx_t i = 0; i < slots_.size(); ++i) {
        EncodeValue(i, slots_[i], values[i]);
    }
}

void FirebirdParamEncoder::Encode(DataChunk &chunk, idx_t row) {
    for (idx_t i = 0; i < slots_.size(); ++i) {
        EncodeValue(i, slots_[i], chunk.GetValue(i, row));
    }
}

} // namespace duckdb
