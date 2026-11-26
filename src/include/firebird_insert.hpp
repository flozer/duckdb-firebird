#pragma once

#include "duckdb.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"

#include "firebird_client.hpp"

namespace duckdb {

// Physical plan for INSERT INTO <firebird ATTACH>.main.T: one parameterized
// INSERT per row in the DuckDB transaction's Firebird write transaction.
// Refuses RETURNING, ON CONFLICT and DEFAULT VALUES, and an ATTACH without
// READ_WRITE.
PhysicalOperator &PlanFirebirdInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                     optional_ptr<PhysicalOperator> plan, Catalog &catalog, bool write_enabled,
                                     NoneEncoding none_encoding);

} // namespace duckdb
