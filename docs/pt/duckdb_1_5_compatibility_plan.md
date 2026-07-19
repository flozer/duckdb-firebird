# Plano de compatibilidade DuckDB 1.5.x

Objetivo: verificar `duckdb-firebird` contra DuckDB `v1.5.2`, `v1.5.3` e `v1.5.4`, sem publicar release, sem atualizar community e sem abrir pedido de merge/update upstream.

## Trava de escopo

- Nao alterar `duckdb/community-extensions` nem abrir PR na community sem pedido explicito do Fernando.
- Nao criar tag, release, asset publico ou comentario upstream durante esta verificacao.
- Usar `D:\Dados\duckdb-salesforce` apenas como referencia de fluxo. Nao copiar resultado, versao ou metadata sem adaptar para Firebird.

## Referencia conhecida

`D:\Dados\duckdb-salesforce\scripts\build_matrix.ps1` ja valida uma matriz local por tag DuckDB. O padrao util para Firebird:

- uma pasta de build por tag;
- checkout do submodulo `duckdb` na tag testada;
- build local contra `extension_config.cmake`;
- testes locais depois do build;
- saida final em tabela com `PASS`, `BUILD-FAIL` ou `TESTS-FAIL`;
- nenhum efeito em tags/releases/community.

## Pre-requisitos

- Windows com Visual Studio Build Tools 2022, CMake, Ninja e vcpkg.
- Firebird 5 client em `C:\Program Files\Firebird\Firebird_5_0` ou `FB_SDK_ROOT` apontando para SDK valido.
- Bases/fixtures locais configuradas para os testes SQL:
  - `FIREBIRD_TEST_DB`
  - `FIREBIRD_DECFLOAT_DB`
  - `FIREBIRD_NONE_DB`
- Credenciais locais somente para banco de teste. Nao usar ambiente produtivo.

## Matriz obrigatoria

Testar exatamente:

- `v1.5.2`
- `v1.5.3`
- `v1.5.4`

`v1.5.3` e baseline atual do script Windows local. `v1.5.4` e a nova versao que precisa entrar na verificacao antes de qualquer conversa de update upstream.

## Fluxo Windows recomendado

Para cada tag:

```powershell
git -C D:\Dados\duckdb-firebird\duckdb fetch --depth 1 origin tag v1.5.2
git -C D:\Dados\duckdb-firebird\duckdb checkout -q v1.5.2
cd D:\Dados\duckdb-firebird
.\scripts\build_windows_local.bat
```

Repetir trocando a tag para `v1.5.3` e `v1.5.4`.

Depois de cada build, rodar os testes SQL registrados no `unittest.exe` da build correspondente. Se o build ficar em `build\release`, executar a partir da raiz do repo:

```powershell
.\build\release\test\unittest.exe test/sql/firebird_attach.test
.\build\release\test\unittest.exe test/sql/firebird_scan.test
.\build\release\test\unittest.exe test/sql/firebird_metadata.test
.\build\release\test\unittest.exe test/sql/firebird_predicates.test
.\build\release\test\unittest.exe test/sql/firebird_bind_params.test
.\build\release\test\unittest.exe test/sql/firebird_paging.test
.\build\release\test\unittest.exe test/sql/firebird_pool.test
.\build\release\test\unittest.exe test/sql/firebird_pool_stats.test
.\build\release\test\unittest.exe test/sql/firebird_observability.test
.\build\release\test\unittest.exe test/sql/firebird_profile_table.test
.\build\release\test\unittest.exe test/sql/firebird_dbt_sources.test
.\build\release\test\unittest.exe test/sql/firebird_decfloat.test
.\build\release\test\unittest.exe test/sql/firebird_none_charset.test
```

Se algum teste exigir fixture ausente, registrar como `ENV-MISSING`, nao como falha de compatibilidade.

## Fluxo Linux/WSL opcional

O script Linux ja aceita versao DuckDB por ambiente:

```bash
DUCKDB_VERSION=v1.5.2 scripts/build_linux_local.sh --clean
DUCKDB_VERSION=v1.5.3 scripts/build_linux_local.sh --clean
DUCKDB_VERSION=v1.5.4 scripts/build_linux_local.sh --clean
```

Usar `--fb-sdk-root PATH` se o cliente Firebird nao for descoberto por `pkg-config`, `ldconfig` ou headers padrao.

## Evidencia minima

Registrar para cada tag:

- commit exato de `duckdb` (`git -C duckdb rev-parse HEAD`);
- comando de build usado;
- caminho do artefato `firebird.duckdb_extension`;
- resultado por teste SQL;
- erro completo em arquivo local se houver falha;
- classificacao final: `PASS`, `BUILD-FAIL`, `TESTS-FAIL` ou `ENV-MISSING`.

## Criterio de aceite

Compatibilidade aprovada somente se:

- as tres tags compilam;
- artefato `firebird.duckdb_extension` existe em cada build;
- testes SQL locais passam ou falhas sao explicadas como ambiente;
- nenhuma API drift do DuckDB exige patch pendente;
- submodulo `duckdb` volta para o baseline esperado ao final.

## Proximo passo recomendado

Criar um `scripts/build_matrix.ps1` para Firebird, adaptado do Salesforce, antes de repetir a validacao muitas vezes. Esse script deve:

- receber `-Tags v1.5.2,v1.5.3,v1.5.4`;
- usar `D:\Dados\duckdb-firebird` como root;
- criar `build\matrix\<tag>`;
- copiar `fbclient.dll` para a build;
- rodar a lista de testes Firebird;
- imprimir uma tabela final;
- restaurar o checkout original do submodulo `duckdb`;
- sair com codigo diferente de zero se build/test falhar por compatibilidade.

## Resultado da verificacao (2026-06-19)

Script criado: `scripts/build_matrix.ps1` (adaptado do Salesforce; sem vcpkg/OpenSSL,
usa `FB_SDK_ROOT`, copia `fbclient.dll` para a build, roda os 13 `firebird_*.test`,
restaura o pin ao final). Bases de teste provisionadas em `C:\fbtest\`:
`test.fdb` (fixture EMPLOYEE + views + TPK_COMPOSITE + TQUOTES), `decfloat.fdb`
(DECVALS, 8 linhas), `none.fdb` (TXT, 3 linhas). Credenciais locais SYSDBA/masterkey.

| Versao  | Commit DuckDB | Build | Artefato                    | Testes            | Status | Observacoes |
|---------|---------------|-------|-----------------------------|-------------------|--------|-------------|
| v1.5.2  | `8a5851971f`  | ok    | firebird.duckdb_extension   | 13/13 (605 asserts) | PASS | 22.6 MB |
| v1.5.3  | `14eca11bd9`  | ok    | firebird.duckdb_extension   | 13/13 (605 asserts) | PASS | baseline / pin do submodulo; 22.6 MB |
| v1.5.4  | `08e34c447b`  | ok    | firebird.duckdb_extension   | 13/13 (605 asserts) | PASS | 22.7 MB |

### API drift

Nenhum. As tres versoes compilam sem alteracao de codigo e os testes produzem
resultado identico (605 assercoes em cada). Nao foi necessario nenhum patch.

## Resultado da re-verificacao (2026-07-19, Runtime/ABI Compatibility Gate)

Reexecutado com a suite atual (19 arquivos `firebird_*.test`, apos as frentes
#39/#31/#26/#47/#48 da Production Stability Gate terem unificado a fixture
canonica e fechado as lacunas de cobertura CI). Rodado a partir de um worktree
isolado (`C:/tmp/fbwt-rabi`, nunca o worktree principal sujo), com uma copia
local de `scripts/build_matrix.ps1` apontando `$root` para esse worktree — o
script commitado continua fixo em `d:/Dados/duckdb-firebird` por design (ver
"Proximo passo recomendado" acima; nao alterado nesta verificacao).

| Versao  | Commit DuckDB | Build | Testes                   | Assertions | Status | Artefato |
|---------|---------------|-------|---------------------------|------------|--------|----------|
| v1.5.2  | `8a5851971f`  | ok    | 19 pass / 0 env-missing   | 854        | PASS   | firebird.duckdb_extension |
| v1.5.3  | `14eca11bd9`  | ok    | 19 pass / 0 env-missing   | 854        | PASS   | firebird.duckdb_extension (baseline / pin do submodulo) |
| v1.5.4  | `08e34c447b`  | ok    | 19 pass / 0 env-missing   | 854        | PASS   | firebird.duckdb_extension |

### API drift (re-verificacao)

Nenhum. As mesmas tres tags compilam sem alteracao de codigo desde a
verificacao de 2026-06-19; a suite cresceu de 13 para 19 arquivos (mais
`firebird_blob_lossless`, `firebird_decfloat`, `firebird_dbt_sources`,
`firebird_metadata_bridge`, `firebird_none_charset`, `firebird_explain_pushdown`)
e de 605 para 854 assercoes, com resultado identico e zero `env-missing` nas
tres versoes. Ver `docs/pt/runtime_abi_compatibility_gate.md` para o
relatorio consolidado da gate.

### Notas de ambiente

- Falha inicial em `firebird_metadata`, `firebird_bind_params` e `firebird_dbt_sources`
  (identica nas tres versoes) era lacuna de fixture: faltava a tabela `TQUOTES`.
  O `scripts/setup_test_firebird.sh` esta desatualizado; a definicao correta de
  `TQUOTES` esta em `scripts/smoke_fixture.sql`. Apos criar `TQUOTES`
  (ID PK, LABEL VARCHAR(60); linhas `D'Agua`, `O'Brien & Co`) os tres testes passam.
  Classificacao: ambiente/fixture, nao compatibilidade DuckDB.
- O submodulo `duckdb` emite `packfile ... does not match index` em alguns comandos
  git (residuo de fetches concorrentes durante a verificacao). Nao bloqueia: todos os
  objetos resolvem, checkout limpo, HEAD restaurado ao pin `14eca11bd9` (v1.5.3).
  Reparo opcional: `git -C duckdb gc` (nao executado nesta verificacao).

### Escopo

Nenhuma acao community/upstream. Nenhum PR, tag, release ou asset. Submodulo restaurado.
