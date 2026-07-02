#include "firebird_view_analysis.hpp"

#include <string>

namespace duckdb {

// SQL-quote a single-quoted string literal (doubles embedded quotes). Local
// copy — this codebase's established convention is small per-file helper
// duplication over cross-file coupling for this exact helper (see
// firebird_scanner.cpp, firebird_profile_table.cpp, firebird_index_profile.cpp,
// each with their own copy).
static std::string SqlLiteral(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out.push_back('\'');
        }
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

bool LookupObjectType(FirebirdConnection &conn,
                     const std::string &upper_table,
                     std::string &out_type) {
    auto cur = conn.OpenCursor(
        "SELECT CASE WHEN r.RDB$VIEW_BLR IS NOT NULL "
        "            OR r.RDB$RELATION_TYPE = 1 THEN 'VIEW' "
        "            ELSE 'TABLE' END "
        "  FROM RDB$RELATIONS r "
        " WHERE r.RDB$RELATION_NAME = " + SqlLiteral(upper_table));
    if (!cur->Fetch()) {
        return false;
    }
    out_type = cur->GetText(0);
    return true;
}

ViewAnalysis AnalyzeViewSource(FirebirdConnection &conn,
                               const std::string &upper_table) {
    ViewAnalysis va;
    std::string src;
    try {
        auto cur = conn.OpenCursor(
            "SELECT RDB$VIEW_SOURCE FROM RDB$RELATIONS "
            " WHERE RDB$RELATION_NAME = " + SqlLiteral(upper_table));
        if (!cur->Fetch() || cur->IsNull(0)) {
            return va; // inspected = false
        }
        src = cur->ReadBlob(0);
    } catch (...) {
        return va; // inspected = false
    }
    if (src.empty()) {
        return va;
    }

    // Normalize: upper-case, and blank out anything that could carry
    // keyword-shaped noise — single-quoted string literals, line comments
    // (-- ...), and block comments (/* ... */) — so a literal like
    // 'fake JOIN and WHERE' or a commented-out clause can't trip the token
    // search. We don't need a real tokenizer for this — replacing those
    // bytes with spaces is enough to keep keyword matching honest.
    //
    // SQL escapes a single quote inside a literal by doubling it ('') —
    // that pair is NOT a close-then-reopen, it's one embedded quote. We
    // must consume both characters while staying in-string, otherwise a
    // literal like 'O''Brien JOIN' would be seen as closing after "O",
    // exposing "Brien JOIN" to the scanner. Same care for the comment
    // forms (only honored outside a string literal).
    std::string norm;
    norm.reserve(src.size());
    enum { CODE, STR, LINE_COMMENT, BLOCK_COMMENT } state = CODE;
    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
        switch (state) {
        case CODE:
            if (c == '\'') {
                state = STR;
                norm.push_back(' ');
            } else if (c == '-' && n == '-') {
                state = LINE_COMMENT;
                norm.push_back(' ');
                norm.push_back(' ');
                ++i; // consume second '-'
            } else if (c == '/' && n == '*') {
                state = BLOCK_COMMENT;
                norm.push_back(' ');
                norm.push_back(' ');
                ++i; // consume '*'
            } else {
                norm.push_back(static_cast<char>(std::toupper(
                    static_cast<unsigned char>(c))));
            }
            break;
        case STR:
            if (c == '\'' && n == '\'') {
                // Escaped quote: stay in string, blank both bytes.
                norm.push_back(' ');
                norm.push_back(' ');
                ++i; // consume the second quote
            } else if (c == '\'') {
                state = CODE; // real closing quote
                norm.push_back(' ');
            } else {
                norm.push_back(' '); // string body — blanked
            }
            break;
        case LINE_COMMENT:
            if (c == '\n') {
                state = CODE;
                norm.push_back('\n');
            } else {
                norm.push_back(' ');
            }
            break;
        case BLOCK_COMMENT:
            if (c == '*' && n == '/') {
                state = CODE;
                norm.push_back(' ');
                norm.push_back(' ');
                ++i; // consume '/'
            } else {
                norm.push_back(' ');
            }
            break;
        }
    }

    // Collapse every run of whitespace (space, tab, newline, CR, form feed)
    // into a single space. Stored RDB$VIEW_SOURCE keeps the author's
    // original formatting, so keywords can be split by newlines/tabs
    // ("GROUP\nBY", "WHERE\t", "INNER\n  JOIN"). Without this, the
    // single-space token search below would miss them. After collapsing,
    // every keyword boundary is exactly one space.
    std::string flat;
    flat.reserve(norm.size());
    bool prev_ws = false;
    for (char c : norm) {
        const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                         c == '\f' || c == '\v');
        if (ws) {
            if (!prev_ws) {
                flat.push_back(' ');
            }
            prev_ws = true;
        } else {
            flat.push_back(c);
            prev_ws = false;
        }
    }

    // Pad with spaces so word-boundary checks at the ends are uniform.
    std::string hay = " " + flat + " ";

    auto contains = [&](const std::string &needle) {
        return hay.find(needle) != std::string::npos;
    };

    va.inspected = true;
    // JOIN in any spelling (INNER/LEFT/RIGHT/FULL/CROSS JOIN all contain
    // "JOIN"; a comma-join is not detected here — conservative, we only
    // flag the explicit keyword).
    va.has_join = contains(" JOIN ");
    va.has_group_by = contains(" GROUP BY ");
    va.has_where = contains(" WHERE ");
    // Common aggregate calls. The "(" guards against matching a column
    // named e.g. SUMMARY or MAXVALUE. Both "FUNC(" and "FUNC (" spellings
    // are checked since whitespace between the name and "(" is legal SQL.
    va.has_aggregate = contains("COUNT(") || contains("SUM(") ||
                       contains("AVG(") || contains("MIN(") ||
                       contains("MAX(") || contains("LIST(") ||
                       contains("COUNT (") || contains("SUM (") ||
                       contains("AVG (") || contains("MIN (") ||
                       contains("MAX (") || contains("LIST (");
    return va;
}

} // namespace duckdb
