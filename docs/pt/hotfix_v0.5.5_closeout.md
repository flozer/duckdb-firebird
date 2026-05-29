# Hotfix v0.5.5 - Encerramento (runtime libfbclient)

Branch de desenvolvimento: `hotfix/community-runtime-fbclient` (parte
direto de `main`, 1 commit). Squash final cai em `review/...` antes do
PR.

## Motivo

A tag `v0.5.4` carregava `vcpkg.json` com dependencia em `libfbclient`.
O catalogo vcpkg oficial usado pelo `duckdb/community-extensions` nao
tem porta para `libfbclient`, entao o build da extensao community
falhava antes do CMake em `vcpkg install`. PR #1980 ficou bloqueado
nas duas plataformas Linux (amd64 + arm64).

Remover apenas `linux_arm64` mascararia o problema: o mesmo gap atinge
`linux_amd64` e qualquer outra plataforma sem libfbclient pre-
instalado. O caminho correto e tirar `libfbclient` do build-time.

## O que mudou

- `vcpkg.json` removido.
- `community-extensions/description.yml` sem `requires_toolchains:
  vcpkg`; `version` e `repo.ref` apontam para `v0.5.5`.
- `CMakeLists.txt` nao linka mais `libfbclient`. Headers Firebird sao
  consumidos via `third_party/firebird/include/` (vendorizado;
  Interbase Public License); `FB_SDK_ROOT` continua como override
  opt-in pra quem quer mirar um SDK Firebird local. `-ldl` adicionado
  no Linux pro loader dinamico.
- `src/include/firebird_client_loader.hpp` + `src/firebird_client_loader.cpp`
  novos. `FirebirdClientApi` tem 16 ponteiros de funcao (uma entrada
  por simbolo ISC que a extensao chama), tipados por
  `decltype(&::nome)` contra `ibase.h` vendorizado.
- `fbapi()` resolve o cliente uma vez por processo:
  1. `$DUCKDB_FIREBIRD_CLIENT_LIBRARY` se for nao-vazio (autoritativo:
     falha levanta excecao em vez de fallback);
  2. defaults da plataforma (`libfbclient.so.2`, `libfbclient.so`,
     `libfbclient.dylib`, `fbclient.dll`, `fbclient_ms.dll`).
- `src/firebird_client.cpp` ganha 16 macros locais ao TU que
  redirecionam cada simbolo ISC para `(::duckdb::fbapi().<sym>)`.
  Strings nao sao expandidas pelo preprocessador, entao mensagens de
  erro como `Check(status, "isc_attach_database(...")` ficam intactas.
- `third_party/firebird/include/` recebeu copia verbatim de `ibase.h`,
  `firebird/ibase.h`, `firebird/iberror.h`, `firebird/impl/*.h` e
  `firebird/impl/msg/*.h` da SDK Firebird 5.0. Cada arquivo preserva
  o cabecalho IPL original. `third_party/firebird/README.md` registra
  provenance e licenca; `THIRD_PARTY_NOTICES.md` na raiz consolida o
  aviso.
- `README.md` ganha uma secao "Runtime requirement: Firebird client
  library" com a tabela por plataforma + a variavel de ambiente +
  a mensagem de erro esperada.
- `CONTRIBUTING.md` documenta que o build local nao precisa mais de
  `libfbclient` instalado; so de runtime quando os testes ou o
  desenvolvedor invocam funcoes `firebird_*`.
- `docs/pt/function_manual.md` ganha o bloco "Requisito de runtime"
  com a mesma tabela + override em portugues.
- `docs/en/roadmap.md` recebe a entrada de hotfix.

## Aceite

- Build local Windows (`scripts\build_windows_local.bat`) verde sem
  alteracao no entorno do desenvolvedor.
- Caminho de erro `DUCKDB_FIREBIRD_CLIENT_LIBRARY` invalida levanta
  excecao acionavel com o caminho problematico ecoado.
- Sweep regressao local: `firebird_pool` 27, `firebird_observability`
  40, `firebird_scan` 124, `firebird_attach` 79, `firebird_metadata`
  79, `firebird_predicates` 13, `firebird_paging` 16,
  `firebird_bind_params` 20 -> 398 assertions, 0 fail. Smoke fixture
  reconstruida a partir do `scripts/smoke_fixture.sql` em `main` (sem
  TPK_COMPOSITE da Fase 3 ainda pausada).
- `git log main..HEAD` mostra 2 commits locais sobre `main`
  (`5e41644` feat + `c53f0d7` docs), confirmando que a branch nasceu
  direto de `main` atual e nao carrega trabalho da Fase 3.

## Licencas

- Codigo da extensao: MIT (`LICENSE`).
- Headers vendorizados: Interbase Public License 1.0
  (`third_party/firebird/include/**`).
- Binario publicado nao embute codigo Firebird; cliente Firebird so
  e usado em runtime via `dlopen` / `LoadLibrary`.
- `community-extensions/description.yml` mantem `license: MIT`
  (corresponde a distribuicao binaria). `THIRD_PARTY_NOTICES.md`
  cobre a redistribuicao dos headers IPL no codigo fonte.

## Proximo passo (NAO executar nesta sessao)

Aprovacao tecnica primeiro. So depois:

- Squash em `review/community-runtime-fbclient` (2 commits: feat +
  docs).
- `git push` + abrir PR para `main`.
- Aguardar CI Linux FB 3/4/5 + Linux x64 + Windows x64.
- Merge se verde.
- Criar tag annotated `v0.5.5` -> aciona `release-assets.yml`,
  publica `duckdb-firebird-0.5.5-{linux,windows}-x64.{tar.gz,zip}`.
- Atualizar PR #1980 (`duckdb/community-extensions`) para `repo.ref:
  v0.5.5`. Esperava-se que o build community passe agora que vcpkg
  nao precisa instalar libfbclient.
