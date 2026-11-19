// Firebird 4+ batched writes through the OO API's IBatch, reached from the
// ISC handles the extension already holds (fb_get_statement_interface /
// fb_get_transaction_interface). See firebird_batch.hpp.

#include "firebird_batch.hpp"

#include <cstring>
#include <vector>

#include "firebird/Interface.h"

#include "duckdb/common/exception.hpp"

#include "firebird_client_loader.hpp"

namespace duckdb {

namespace {

// Size of the batch buffer, set explicitly (Firebird's default; max 256
// MB) so the number of messages that fit can be computed: add() fails with
// "batch too big" once the buffer is full.
constexpr unsigned kBatchBufferBytes = 16u * 1024u * 1024u;

std::string StatusErrors(Firebird::CheckStatusWrapper &status) {
    return FirebirdStatusText(reinterpret_cast<const ISC_STATUS *>(status.getErrors()));
}

bool Failed(Firebird::CheckStatusWrapper &status) {
    return (status.getState() & Firebird::IStatus::STATE_ERRORS) != 0;
}

bool IsText(int16_t sqltype) {
    return sqltype == SQL_TEXT || sqltype == SQL_VARYING;
}

} // namespace

bool FirebirdCanBatchStatement(int engine_major, int statement_type) {
    switch (statement_type) {
    case isc_info_sql_stmt_insert:
    case isc_info_sql_stmt_update:
    case isc_info_sql_stmt_delete:
        return engine_major >= 4;
    case isc_info_sql_stmt_exec_procedure:  // EXECUTE PROCEDURE and EXECUTE BLOCK
        // A Firebird 4 server (4.0.6, 4.0.7) fails the second EXECUTE BLOCK
        // message of a batch with "request synchronization error".
        return engine_major >= 5;
    default:
        return false;
    }
}

struct FirebirdBatchWriter::Impl {
    FirebirdConnection *conn = nullptr;
    FirebirdStatement *stmt = nullptr;
    Firebird::IMaster *master = nullptr;
    Firebird::IStatus *raw_status = nullptr;
    std::unique_ptr<Firebird::CheckStatusWrapper> status;
    Firebird::IStatement *istmt = nullptr;
    Firebird::IMessageMetadata *meta = nullptr;
    Firebird::IBatch *batch = nullptr;
    bool count_rows = false;
    std::vector<unsigned> offsets;
    std::vector<unsigned> null_offsets;
    std::vector<unsigned> lengths;
    std::vector<int16_t> types;
    std::vector<unsigned char> message;

    ~Impl() {
        if (batch) {
            batch->release();
        }
        if (meta) {
            meta->release();
        }
        if (istmt) {
            istmt->release();
        }
        status.reset();
        if (raw_status) {
            raw_status->dispose();
        }
    }
};

std::unique_ptr<FirebirdBatchWriter> FirebirdBatchWriter::TryCreate(FirebirdConnection &conn,
                                                                    FirebirdStatement &stmt,
                                                                    FirebirdParamEncoder &encoder,
                                                                    int statement_type, bool count_rows,
                                                                    std::string *why_not) {
    auto no = [&](const std::string &why) -> std::unique_ptr<FirebirdBatchWriter> {
        if (why_not) {
            *why_not = why;
        }
        return nullptr;
    };
    const auto &api = fbapi();
    if (!api.fb_get_master_interface || !api.fb_get_statement_interface || !api.fb_get_transaction_interface) {
        return no("the Firebird client library has no fb_get_statement_interface (Firebird 4+ client needed)");
    }
    const int major = conn.EngineMajor();
    if (major < 4) {
        return no("the server is Firebird " + std::to_string(major) + "; batches need Firebird 4+");
    }
    if (!FirebirdCanBatchStatement(major, statement_type)) {
        return no("a Firebird " + std::to_string(major) + " server can't batch this statement type (" +
                  std::to_string(statement_type) + ")");
    }
    const auto layout = encoder.GetLayout();
    for (auto &p : layout) {
        if (p.blob) {
            return no("the statement has BLOB parameters");
        }
    }

    std::unique_ptr<FirebirdBatchWriter> writer(new FirebirdBatchWriter());
    writer->impl_.reset(new Impl());
    auto &d = *writer->impl_;
    d.conn = &conn;
    d.stmt = &stmt;
    // Firebird reports 0 for every EXECUTE BLOCK / EXECUTE PROCEDURE message
    // of a batch, even when it changed rows (its issue 9239): only plain DML
    // gets record counts; anything else reports none (rows_affected NULL).
    d.count_rows = count_rows && (statement_type == isc_info_sql_stmt_insert ||
                                  statement_type == isc_info_sql_stmt_update ||
                                  statement_type == isc_info_sql_stmt_delete);
    d.master = static_cast<Firebird::IMaster *>(api.fb_get_master_interface());
    d.raw_status = d.master->getStatus();
    d.status.reset(new Firebird::CheckStatusWrapper(d.raw_status));
    auto *status = d.status.get();

    ISC_STATUS isc[20] = {};
    api.fb_get_statement_interface(isc, &d.istmt, &stmt.Handle());
    if (isc[0] == 1 && isc[1] != 0) {
        return no("fb_get_statement_interface: " + FirebirdStatusText(isc));
    }

    // One message layout: the encoder's wire types.
    const unsigned n = static_cast<unsigned>(layout.size());
    Firebird::IMetadataBuilder *builder = d.master->getMetadataBuilder(status, n);
    if (Failed(*status)) {
        return no("getMetadataBuilder: " + StatusErrors(*status));
    }
    // Every interface call resets the status, so each one is checked before
    // the next.
    for (unsigned i = 0; i < n && !Failed(*status); ++i) {
        const auto &p = layout[i];
        builder->setType(status, i, static_cast<unsigned>(p.sqltype) | 1u);
        if (!Failed(*status)) {
            builder->setLength(status, i, static_cast<unsigned>(p.sqllen));
        }
        if (Failed(*status)) {
            break;
        }
        if (IsText(p.sqltype)) {
            builder->setCharSet(status, i, static_cast<unsigned>(p.sqlsubtype & 0xFF));
        } else {
            builder->setSubType(status, i, p.sqlsubtype);
            if (!Failed(*status)) {
                builder->setScale(status, i, p.sqlscale);
            }
        }
    }
    if (!Failed(*status)) {
        d.meta = builder->getMetadata(status);
    }
    builder->release();
    if (Failed(*status)) {
        return no("message metadata: " + StatusErrors(*status));
    }

    Firebird::IXpbBuilder *pb =
        d.master->getUtilInterface()->getXpbBuilder(status, Firebird::IXpbBuilder::BATCH, nullptr, 0);
    if (Failed(*status) || !pb) {
        return no("getXpbBuilder: " + StatusErrors(*status));
    }
    if (d.count_rows) {
        pb->insertInt(status, Firebird::IBatch::TAG_RECORD_COUNTS, 1);
    }
    if (!Failed(*status)) {
        pb->insertInt(status, Firebird::IBatch::TAG_BUFFER_BYTES_SIZE, static_cast<int>(kBatchBufferBytes));
    }
    if (Failed(*status)) {
        pb->dispose();
        return no("batch parameters: " + StatusErrors(*status));
    }
    const unsigned pb_length = pb->getBufferLength(status);
    const unsigned char *pb_buffer = Failed(*status) ? nullptr : pb->getBuffer(status);
    if (!Failed(*status)) {
        d.batch = d.istmt->createBatch(status, d.meta, pb_length, pb_buffer);
    }
    pb->dispose();
    if (Failed(*status)) {
        d.batch = nullptr;
        return no("createBatch: " + StatusErrors(*status));
    }

    for (unsigned i = 0; i < n; ++i) {
        d.offsets.push_back(d.meta->getOffset(status, i));
        if (!Failed(*status)) {
            d.null_offsets.push_back(d.meta->getNullOffset(status, i));
        }
        if (!Failed(*status)) {
            d.lengths.push_back(d.meta->getLength(status, i));
        }
        if (Failed(*status)) {
            return no("message layout: " + StatusErrors(*status));
        }
        d.types.push_back(layout[i].sqltype);
    }
    d.message.assign(d.meta->getMessageLength(status), 0);
    unsigned aligned = 0;
    if (!Failed(*status)) {
        aligned = d.meta->getAlignedLength(status);
    }
    if (Failed(*status)) {
        return no("message layout: " + StatusErrors(*status));
    }
    // Messages are stored aligned; leave one message of slack.
    const idx_t fit = aligned > 0 ? kBatchBufferBytes / aligned : 1;
    writer->max_messages_ = fit > 1 ? fit - 1 : 1;
    return writer;
}

FirebirdBatchWriter::~FirebirdBatchWriter() = default;

void FirebirdBatchWriter::Add() {
    auto &d = *impl_;
    XSQLDA *sqlda = d.stmt->InputSqlda();
    std::memset(d.message.data(), 0, d.message.size());
    for (size_t i = 0; i < d.offsets.size(); ++i) {
        const XSQLVAR &x = sqlda->sqlvar[i];
        const short is_null = (x.sqlind && *x.sqlind == -1) ? -1 : 0;
        std::memcpy(d.message.data() + d.null_offsets[i], &is_null, sizeof(short));
        if (is_null) {
            continue;
        }
        if (d.types[i] == SQL_VARYING) {
            ISC_USHORT len;
            std::memcpy(&len, x.sqldata, sizeof(len));
            std::memcpy(d.message.data() + d.offsets[i], x.sqldata, sizeof(len) + len);
        } else {
            std::memcpy(d.message.data() + d.offsets[i], x.sqldata, d.lengths[i]);
        }
    }
    d.batch->add(d.status.get(), 1, d.message.data());
    if (Failed(*d.status)) {
        // A lost connection marks the connection broken, as on the per-row path.
        d.conn->NoteStatus(reinterpret_cast<const ISC_STATUS *>(d.status->getErrors()));
        throw IOException("IBatch::add: " + StatusErrors(*d.status));
    }
    ++pending_;
}

FirebirdBatchWriter::Result FirebirdBatchWriter::Execute() {
    Result r;
    if (pending_ == 0) {
        r.affected = impl_->count_rows ? 0 : -1;
        return r;
    }
    auto &d = *impl_;
    auto *status = d.status.get();
    const auto &api = fbapi();
    Firebird::ITransaction *tra = nullptr;
    ISC_STATUS isc[20] = {};
    api.fb_get_transaction_interface(isc, &tra, &d.conn->tr());
    if (isc[0] == 1 && isc[1] != 0) {
        throw IOException("fb_get_transaction_interface: " + FirebirdStatusText(isc));
    }
    Firebird::IBatchCompletionState *cs = d.batch->execute(status, tra);
    tra->release();
    const idx_t sent = pending_;
    pending_ = 0;
    r.sent = sent;
    if (Failed(*status)) {
        // A lost connection marks the connection broken, as on the per-row path.
        d.conn->NoteStatus(reinterpret_cast<const ISC_STATUS *>(status->getErrors()));
        r.failed = true;
        r.whole_batch = true;
        r.error = "IBatch::execute: " + StatusErrors(*status);
        return r;
    }
    const unsigned size = cs->getSize(status);
    if (Failed(*status)) {
        cs->dispose();
        r.failed = true;
        r.whole_batch = true;
        r.error = "IBatchCompletionState::getSize: " + StatusErrors(*status);
        return r;
    }
    r.affected = d.count_rows ? 0 : -1;
    for (unsigned p = 0; p < size; ++p) {
        const int state = cs->getState(status, p);
        if (Failed(*status)) {
            r.failed = true;
            r.failed_index = p;
            r.error = "IBatchCompletionState::getState: " + StatusErrors(*status);
            break;
        }
        if (state == Firebird::IBatchCompletionState::EXECUTE_FAILED) {
            Firebird::CheckStatusWrapper err(d.master->getStatus());
            cs->getStatus(status, &err, p);
            r.failed = true;
            r.failed_index = p;
            if (Failed(*status)) {
                r.error = "IBatchCompletionState::getStatus: " + StatusErrors(*status);
            } else {
                // A message can also fail because the attachment went away
                // (a kill, a shutdown): note it as on the per-row path.
                d.conn->NoteStatus(reinterpret_cast<const ISC_STATUS *>(err.getErrors()));
                r.error = StatusErrors(err);
            }
            err.dispose();
            break;
        }
        ++r.executed;
        if (d.count_rows) {
            if (state >= 0) {
                r.affected += state;
            } else {
                r.affected = -1;  // SUCCESS_NO_INFO: no count from the server
                d.count_rows = false;
            }
        }
    }
    cs->dispose();
    if (!r.failed && r.executed != sent) {
        r.failed = true;
        r.failed_index = r.executed;
        r.error = "IBatch::execute ran " + std::to_string(r.executed) + " of " + std::to_string(sent) + " messages";
    }
    return r;
}

} // namespace duckdb
