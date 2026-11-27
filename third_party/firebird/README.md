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

`ibase.h` and the other C headers are distributed under the
**Interbase Public License 1.0** (IPL), a Mozilla-style license derived
from the original Inprise Corporation grant; each preserves its original
copyright header. The full license text is available at
<https://github.com/FirebirdSQL/firebird/blob/master/builds/install/misc/IPLicense.txt>.

The OO API headers (`firebird/Interface.h`, `firebird/IdlFbInterfaces.h`),
under the IDPL, are vendored apart, in `third_party/firebird-oo/` (see its
README).

## Source

Headers were copied from a Firebird 5.0 install
(`include/ibase.h`, `include/firebird/*.h`,
`include/firebird/impl/*.h`). The ISC entry-point surface they
describe is stable across Firebird 3 / 4 / 5.

A build with `FB_SDK_ROOT` (the Windows CI builds use the Firebird 4.0.5
SDK) takes `ibase.h` and the OO API headers from that SDK instead, so the
batch code compiles against two header sets: the SDK's and the vendored
5.0.3 ones. The OO interfaces are versioned, and the batch code calls only
what Firebird 4.0 has.
