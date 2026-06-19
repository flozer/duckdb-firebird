#pragma once
#include "duckdb/function/table_function.hpp"

namespace duckdb {
TableFunction GetFirebirdIndexesFunction();
TableFunction GetFirebirdForeignKeysFunction();
TableFunction GetFirebirdGeneratorsFunction();
TableFunction GetFirebirdDomainsFunction();
TableFunction GetFirebirdComputedColumnsFunction();
TableFunction GetFirebirdDependenciesFunction();
TableFunction GetFirebirdCommentsFunction();
} // namespace duckdb
