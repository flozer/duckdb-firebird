// Standalone helper (not part of the shipped extension): writes
// TBLOB_MULTISEG's NOTE/DATA columns as genuinely separate 80-byte
// isc_put_segment calls. A plain SQL literal INSERT does NOT create
// multiple physical Firebird segments regardless of the column's
// declared SEGMENT SIZE -- confirmed empirically (a 20,020-byte literal
// round-tripped correctly through the unfixed ReadBlob, because the
// engine stored it as one segment). SEGMENT SIZE only advises a
// low-level writer calling isc_put_segment directly; it does not
// retroactively chunk a value the engine received as a single string.
// This is the only way to reproduce -- or verify the fix for -- the
// ReadBlob segment-loop bug (issue #35).
#include <ibase.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static void check(ISC_STATUS *status, const char *what) {
    if (status[0] == 1 && status[1]) {
        ISC_LONG sqlcode = isc_sqlcode(status);
        char buf[512];
        isc_sql_interprete(sqlcode, buf, sizeof(buf));
        fprintf(stderr, "%s failed: %s (sqlcode=%ld)\n", what, buf, (long)sqlcode);
        exit(1);
    }
}

static int write_column(isc_db_handle &db_handle, long id, const char *column,
                         const std::string &content) {
    ISC_STATUS status[20] = {0};
    isc_tr_handle tr_handle = 0;
    isc_start_transaction(status, &tr_handle, 1, &db_handle, 0, nullptr);
    check(status, "isc_start_transaction");

    isc_blob_handle blob_handle = 0;
    ISC_QUAD blob_id = {0, 0};
    isc_create_blob2(status, &db_handle, &tr_handle, &blob_handle, &blob_id, 0, nullptr);
    check(status, "isc_create_blob2");

    size_t off = 0;
    int segments = 0;
    while (off < content.size()) {
        size_t chunk = content.size() - off;
        if (chunk > 80) chunk = 80;
        isc_put_segment(status, &blob_handle, (unsigned short)chunk,
                         const_cast<char *>(content.data() + off));
        check(status, "isc_put_segment");
        off += chunk;
        segments++;
    }
    isc_close_blob(status, &blob_handle);
    check(status, "isc_close_blob");

    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE TBLOB_MULTISEG SET %s = ? WHERE ID = %ld", column, id);

    isc_stmt_handle stmt = 0;
    isc_dsql_allocate_statement(status, &db_handle, &stmt);
    check(status, "isc_dsql_allocate_statement");

    char sqlda_buf[sizeof(XSQLDA) + sizeof(XSQLVAR)];
    XSQLDA *in_sqlda = reinterpret_cast<XSQLDA *>(sqlda_buf);
    memset(in_sqlda, 0, sizeof(sqlda_buf));
    in_sqlda->version = SQLDA_VERSION1;
    in_sqlda->sqln = 1;

    isc_dsql_prepare(status, &tr_handle, &stmt, 0, sql, 3, in_sqlda);
    check(status, "isc_dsql_prepare");

    in_sqlda->sqld = 1;
    in_sqlda->sqlvar[0].sqltype = SQL_BLOB;
    in_sqlda->sqlvar[0].sqllen = sizeof(ISC_QUAD);
    in_sqlda->sqlvar[0].sqldata = reinterpret_cast<char *>(&blob_id);
    in_sqlda->sqlvar[0].sqlind = nullptr;

    isc_dsql_execute(status, &tr_handle, &stmt, 1, in_sqlda);
    check(status, "isc_dsql_execute (update)");

    isc_dsql_free_statement(status, &stmt, DSQL_drop);
    isc_commit_transaction(status, &tr_handle);
    check(status, "isc_commit_transaction");

    fprintf(stderr, "%s: wrote %d segments, %zu bytes\n", column, segments, content.size());
    return segments;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: mkblob_fixture <db> <user> <password> <table_id>\n");
        return 2;
    }
    const char *db = argv[1];
    const char *user = argv[2];
    const char *pass = argv[3];
    long id = atol(argv[4]);

    char dpb[256];
    char *p = dpb;
    *p++ = isc_dpb_version1;
    *p++ = isc_dpb_user_name; *p++ = (char)strlen(user); memcpy(p, user, strlen(user)); p += strlen(user);
    *p++ = isc_dpb_password;  *p++ = (char)strlen(pass); memcpy(p, pass, strlen(pass)); p += strlen(pass);
    *p++ = isc_dpb_sql_dialect; *p++ = 1; *p++ = 3;

    isc_db_handle db_handle = 0;
    ISC_STATUS status[20] = {0};
    isc_attach_database(status, (short)strlen(db), (char *)db, &db_handle, (short)(p - dpb), dpb);
    check(status, "isc_attach_database");

    std::string note = "START-NOTE-"; note.append(4000, 'N'); note.append("-END-NOTE");
    std::string data = "START-DATA-"; data.append(4000, 'D'); data.append("-END-DATA");

    write_column(db_handle, id, "NOTE", note);
    write_column(db_handle, id, "DATA", data);

    isc_detach_database(status, &db_handle);
    printf("OK\n");
    return 0;
}
