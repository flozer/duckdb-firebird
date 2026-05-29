# Third-party notices

`duckdb-firebird` itself is licensed under the **MIT License**
(`LICENSE`). The source tree additionally redistributes the
third-party material listed below. Their original copyright headers
and license terms are preserved inside the corresponding files.

The published `.duckdb_extension` binary does **not** statically
embed any of this third-party code: the Firebird client library is
resolved at runtime via `dlopen` / `LoadLibrary`, and the headers
listed below are consumed only at compile time.

## Firebird client headers (Interbase Public License 1.0)

| Source | License | Where in this repo |
|---|---|---|
| Firebird 5.0 client headers (`ibase.h`, `firebird/*.h`, `firebird/impl/*.h`, `firebird/impl/msg/*.h`) | Interbase Public License 1.0 (IPL) | `third_party/firebird/include/` |

The vendored tree mirrors the layout shipped by the Firebird 5.0
client SDK. Each individual header file preserves its original
Inprise Corporation copyright notice. The IPL is a Mozilla-style
license; its full text lives at
<https://github.com/FirebirdSQL/firebird/blob/master/builds/install/misc/IPLicense.txt>.

These headers are required at compile time so the extension can
type the function pointers in `src/firebird_client_loader.hpp`. They
are **not** linked into the binary; the runtime resolution path uses
the user-supplied Firebird client library
(`libfbclient.so.2` / `libfbclient.dylib` / `fbclient.dll`) instead.

See `third_party/firebird/README.md` for provenance details.

## DuckDB headers (MIT)

The extension build pulls DuckDB's own headers via the
`extension-ci-tools` build infrastructure. DuckDB itself is
distributed under the MIT License; refer to the DuckDB repository
for its `LICENSE` file.
