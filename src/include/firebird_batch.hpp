#pragma once

#include <memory>
#include <string>

#include "duckdb.hpp"

#include "firebird_client.hpp"
#include "firebird_param_encoder.hpp"

namespace duckdb {

// Firebird 4+ batched execution (IBatch) of a prepared statement with
// parameters: rows are encoded by a fixed-layout FirebirdParamEncoder,
// copied into one batch message each, and sent together on Execute().
//
// INSERT/UPDATE/DELETE on Firebird 4+, EXECUTE PROCEDURE / EXECUTE BLOCK
// on Firebird 5+ (a Firebird 4 server fails the second EXECUTE BLOCK
// message of a batch with "request synchronization error"). Parameters
// that are BLOBs keep the per-row path.
class FirebirdBatchWriter {
public:
    // Null (and the reason in *why_not) when the statement can't be
    // batched here: client older than Firebird 4 (no
    // fb_get_statement_interface), server older than 4, a
    // statement type the server can't batch, BLOB parameters, or
    // createBatch failed. The encoder must have been built with
    // fixed_layout = true on `stmt`.
    static std::unique_ptr<FirebirdBatchWriter> TryCreate(FirebirdConnection &conn, FirebirdStatement &stmt,
                                                          FirebirdParamEncoder &encoder, int statement_type,
                                                          bool count_rows, std::string *why_not);
    ~FirebirdBatchWriter();

    // Appends the row the encoder encoded last.
    void Add();
    idx_t Pending() const {
        return pending_;
    }
    // True when one more message might not fit the batch buffer: the
    // caller must Execute() before the next Add(). A batch of wide rows
    // holds fewer than batch_size messages.
    bool Full() const {
        return pending_ >= max_messages_;
    }

    struct Result {
        idx_t executed = 0;         // messages that ran, the failed one excluded
        int64_t affected = -1;      // sum of record counts; -1 = not reported
        bool failed = false;
        idx_t failed_index = 0;     // 0-based within this batch
        // The batch as a whole failed (IBatch::execute itself, e.g. a lost
        // connection), not one message: failed_index names no row.
        bool whole_batch = false;
        idx_t sent = 0;             // messages in the batch
        std::string error;
    };
    // Sends the pending messages in the connection's current transaction.
    // Stops at the first failing message (no MULTIERROR).
    Result Execute();

private:
    FirebirdBatchWriter() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    idx_t pending_ = 0;
    idx_t max_messages_ = 1;
};

// The statement types a server of this major version can batch.
bool FirebirdCanBatchStatement(int engine_major, int statement_type);

} // namespace duckdb
