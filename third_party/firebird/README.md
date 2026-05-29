# Vendored Firebird headers

This tree contains a verbatim copy of the public Firebird client
headers (`ibase.h` + `firebird/*`) shipped with the Firebird 5.0
client SDK. The extension consumes them at **compile time only**;
the corresponding shared library (`libfbclient.so`,
`libfbclient.dylib`, `fbclient.dll`) is resolved at **runtime** via
`dlopen` / `LoadLibrary` from `src/firebird_client_loader.cpp`.

Vendoring lets the community-extensions CI build the extension on
hosts that do not have Firebird installed (and removes the
`libfbclient` vcpkg dependency that previously failed the cross-
platform build matrix).

## License

The headers are distributed under the **Interbase Public License
1.0** (IPL), a Mozilla-style license derived from the original
Inprise Corporation grant. Each file preserves the original
copyright header. The full license text is available at
<https://github.com/FirebirdSQL/firebird/blob/master/builds/install/misc/IPLicense.txt>.

## Source

Headers were copied from a Firebird 5.0 install
(`include/ibase.h`, `include/firebird/*.h`,
`include/firebird/impl/*.h`). The ISC entry-point surface they
describe is stable across Firebird 3 / 4 / 5; updates are
unnecessary unless we start consuming OO-API symbols.
