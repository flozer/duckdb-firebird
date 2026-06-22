// PK-range pushdown explainability helpers.
//
// ClassifyPkRange: pure function — derives one of four normalized reasons
// from a PrimaryKeyDescriptor that was populated at ATTACH time (zero new I/O).
// The four normalized reason strings are the stable contract used by Task 5's
// SQL tests.

#include "firebird_scanner.hpp"

namespace duckdb {

PkRangeClassification ClassifyPkRange(const PrimaryKeyDescriptor &d) {
    if (!d.has_pk)
        return {false, "",           "no primary key",   PkRangeStrategy::SERIAL};
    if (d.columns.size() > 1)
        return {false, "",           "composite PK",     PkRangeStrategy::SERIAL};
    if (!d.single_numeric)
        return {false, d.columns[0], "non-numeric PK",   PkRangeStrategy::SERIAL};
    return     {true,  d.columns[0], "single numeric PK", PkRangeStrategy::PK_RANGE_PARTITIONABLE};
}

} // namespace duckdb
