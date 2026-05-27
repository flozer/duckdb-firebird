# Security Policy

## Reporting a Vulnerability

Please report security issues privately. Do not open a public GitHub issue
with exploit details, credentials, database paths, or sample data.

Use GitHub private vulnerability reporting if it is available on this
repository. Otherwise, contact the maintainer listed in
`community-extensions/description.yml` and include:

- affected version or commit;
- operating system and Firebird client/server version;
- minimal reproduction steps;
- whether the issue involves credential exposure, SQL injection,
  unsafe file access, or malformed Firebird data.

## Scope

Security-sensitive areas include:

- Firebird connection-string parsing;
- SQL generation for predicate pushdown;
- handling of credentials in examples, scripts, logs, and error messages;
- file paths used by local databases and Parquet exports;
- GitHub Actions workflows and release artifacts.

## Supported Versions

Only the latest tagged release is actively reviewed for fixes. Older tags
may receive documentation-only clarifications, but security fixes target
the current release line first.

## Data Handling

This repository must not contain real Firebird database files, backups,
customer data, private keys, or production credentials. Use synthetic
fixtures under `scripts/` and `test/sql/`.
