# Hotfix v0.5.6 - Encerramento (restaurar manifest vcpkg vazio)

Branch de desenvolvimento: `hotfix/restore-empty-vcpkg-manifest`
(parte direto de `main`, 1 commit). Sem push/tag/release durante o
ciclo.

## Motivo

`v0.5.5` resolveu o bloqueio principal (`libfbclient` so existir no
runtime, sem dependencia de build em vcpkg), mas removeu o
`vcpkg.json` em vez de zera-lo. A community CI do
`duckdb/community-extensions` sempre invoca o build com:

```text
-DVCPKG_BUILD=1
-DVCPKG_MANIFEST_DIR=/duckdb_build_dir/
```

O toolchain do vcpkg le `vcpkg.json` daquele diretorio antes do CMake
configurar o projeto. Sem o arquivo, o build aborta com "manifest not
found" antes mesmo de chegar nas mensagens uteis. O sintoma muda de
"libfbclient nao existe" para "manifest nao existe", mas a CI
continua vermelha em PR #1980.

## Correcao

- `vcpkg.json` **restaurado**, agora com `"dependencies": []`. O
  toolchain encontra um manifest valido e nao tenta instalar nada.
- `name`, `version-string`, `description` repintados para v0.5.6 +
  texto que reflete o caminho de runtime loader.
- `community-extensions/description.yml` ja sem
  `requires_toolchains: vcpkg` (heranca da v0.5.5); `version` e
  `repo.ref` bumpam para v0.5.6.
- README badge + linha "Release: **vX.Y.Z**" + linha community-ref
  bumpadas para v0.5.6. Linha "Starting with **v0.5.5**, the
  extension loads the Firebird client library at runtime" fica como
  esta - o runtime loader entrou em v0.5.5 e essa e a historia
  correta; v0.5.6 e correcao puramente de empacotamento.

## Restou intacto

- Runtime loader (`src/firebird_client_loader.{hpp,cpp}`), tabela
  `FirebirdClientApi` com os 16 ponteiros de funcao, lazy + mutex,
  `DUCKDB_FIREBIRD_CLIENT_LIBRARY` autoritativo.
- Vendored Firebird headers sob `third_party/firebird/include/`
  (IPL) + `THIRD_PARTY_NOTICES.md`.
- TU-local macros em `src/firebird_client.cpp`.
- `CMakeLists.txt` sem link `libfbclient`, headers vendorizados
  por default.

## Aceite

- Build local Windows (`scripts\build_windows_local.bat`) verde.
- `git grep "libfbclient" vcpkg.json` nao acha dependencia (so
  string em `description`).
- Sweep regressao local: as 8 suites firebird_* continuam verdes
  (e a community CI agora tem manifest legivel para passar).

## Politica

Branch local. Sem push/tag/release ate aprovacao PM e merge limpo.

## Proximo passo

Aprovacao tecnica primeiro. So depois:

- Squash em `review/restore-empty-vcpkg-manifest` (commit unico).
- PR contra `main`.
- CI Linux FB 3/4/5 + Linux x64 + Windows x64.
- Merge.
- Tag annotated `v0.5.6` -> `release-assets.yml` publica
  `duckdb-firebird-0.5.6-{linux,windows}-x64.{tar.gz,zip}`.
- Atualizar PR #1980 (community-extensions) para `repo.ref:
  v0.5.6` e tirar de draft.

Recomendacao operacional do PM ja aplicada: PR #1980 voltou para
draft ate o v0.5.6 publicar, para nao queimar o turno do maintainer
no mesmo erro.
