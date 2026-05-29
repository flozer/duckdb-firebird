# Fase 3 - Encerramento (dbt sources generator)

Branch local: `dev/phase3-dbt-sources` (rebase em `main` pos-v0.5.6).
Sem push, sem tag, sem release, sem atualizacao da community
extension. Decisao de integracao fica com o PM apos esta revisao.

## Resumo

`firebird_generate_dbt_sources(catalog_name VARCHAR)` recebe o alias
de um `ATTACH ... (TYPE firebird)` e devolve uma unica linha
`yaml VARCHAR` com um `sources.yml` dbt-compativel. Output
deterministico, sem connection string, com testes `not_null +
unique` automaticos em PK simples.

## Historia

Chunks A/B/C/D + pause-note + Chunk D docs, todos commits locais
na `dev/phase3-dbt-sources` apos o rebase em main (`2dde4ed`,
v0.5.6):

| Chunk | Conteudo |
|-------|----------|
| A | Stub: registro da funcao + `ValidateFirebirdAttachAlias` (NULL / catalogo inexistente / nao-Firebird levanta `BinderException` com nome da funcao). |
| B | Gerador YAML real + `FirebirdMetadataLease` (RAII; pool do catalogo via friend, sem expor `conn_info`); query unica em `RDB$RELATIONS` + `LoadTableSchema` por tabela; CASE COUNT=1 para PK composta. |
| C | `test/sql/firebird_dbt_sources.test` (21 assertions: arg validation, YAML header, ordering, data_type, PK simples gera tests, PK composta NAO gera, view nao gera, no-leak); `TPK_COMPOSITE` adicionada em `setup_test_firebird.sh` + `smoke_fixture.sql`; `firebird_metadata.test` atualizada em lockstep (4 -> 5 tabelas, 16 -> 19 colunas). |
| D | `docs/pt/function_manual.md` Nivel 3 (nova entrada com o que faz, exemplo COPY, exemplo de YAML, erros, limitacoes); `docs/en/roadmap.md` item 16 flipado para *implementado (local)*; este documento. |

## Comportamento

```sql
ATTACH 'firebird://...' AS fb (TYPE firebird);

COPY (SELECT yaml FROM firebird_generate_dbt_sources('fb'))
  TO 'sources.yml' (FORMAT csv, HEADER false, QUOTE '');
```

Conteudo gerado (resumo):

- `version: 2`.
- `sources: - name: <alias>, schema: "main", description: "", tables: [...]`.
- Tabelas em ordem alfabetica por `RDB$RELATION_NAME`; views entram
  como tabelas, sem `tests`.
- Colunas em ordem `RDB$FIELD_POSITION`; cada uma com `name`,
  `data_type` (DuckDB `LogicalType::ToString()`), `description: ""`.
- PK simples: bloco `tests: [- not_null, - unique]`.
- PK composta: sem `tests` (decisao deliberada - marcar so uma
  coluna como `unique` quando a chave e multi-segmento seria
  semanticamente errado).
- YAML escape: aspas, dois-pontos, newline, backslash sempre saem
  via `YamlQuote`.
- Sem `password`, sem `firebird://`, sem caminho de banco,
  sem `SYSDBA` na saida (cross-check no-leak no teste).

## Decisoes PM consolidadas

- Funcao recebe **alias** do `ATTACH`, nao connection string.
  `firebird_scan(...)` direto fica fora do escopo (sem catalogo,
  sem pool).
- `connection_id`/`connection_reused` continuam reservados ao slot
  da Fase 1/2; nao foram tocados pela Fase 3.
- `data_type` usa **logical type DuckDB** (o que o usuario ve na
  ATTACH), nao tipo Firebird nativo.
- PK composta -> sem `tests` (finding bloqueante endereçado no
  Chunk B antes de abrir o Chunk C).
- Helper de catalogo usa **friend** em `FirebirdCatalog` para
  evitar expor `conn_info_` ou `pool_` publicamente.

## Validacao

Build local Windows (`scripts\build_windows_local.bat`) verde apos
o rebase em main (v0.5.6 runtime loader + vendored headers +
restored vcpkg manifest). Sweep com smoke fixture regenerada:

| Suite | Assertions |
|-------|------------|
| `firebird_dbt_sources` | 21 |
| `firebird_pool` | 27 |
| `firebird_observability` | 40 |
| `firebird_attach` | 79 |
| `firebird_metadata` | 84 |
| `firebird_predicates` | 13 |
| `firebird_paging` | 16 |
| `firebird_bind_params` | 20 |
| **Total** | **300, 0 fail** |

`firebird_scan.test`: flake conhecida do Firebird Embedded
(`partitions=4`, `isc sqlcode=-901`) - pre-existente, documentada
em `docs/pt/phase1_closeout.md`. Nao reproduz no CI Linux com
servico FB 3.0 real e a Fase 3 nao toca o caminho de scan direto.

## Dividas registradas

1. **Descricoes (`description: ""`)** - `RDB$DESCRIPTION` (BLOB
   SUB_TYPE 1) nao e lido nesta versao. Adicao requer caminho de
   leitura BLOB segmentada; cabe em uma fase seguinte de
   "introspecao expandida" que tambem pode incluir
   `firebird_pool_stats()` (divida da Fase 2).
2. **Named params** - `source_name`, `target_schema`,
   `include_views`. Hoje o nome do `source` coincide com o alias
   da ATTACH; `schema` e sempre `"main"`; views entram como
   tabelas. Tudo razoavel para o caso 80/20, mas inflexivel para
   projetos dbt com convencoes diferentes.
3. **PK composta sem teste de unicidade** - decisao consciente.
   Quem precisar de teste deve adicionar manualmente um
   `dbt-utils.unique_combination_of_columns` no arquivo gerado.
   Considerar emitir o bloco automaticamente se a Fase 4+
   decidir suporte explicito a dbt utils.

## Politica respeitada

- Branch local; nao foi para `main`, nao foi para origem do fork
  community, nao mexeu em PR #1980.
- Nenhum `git push`, nenhuma tag `v*`, nenhum release.
- Cada chunk validado localmente antes do proximo; cada pausa de
  PM endereçada.
- Premissa CONTRIBUTING (atualizar `docs/pt/function_manual.md`
  para toda mudanca publica) cumprida nesta fase via o Chunk D.

## Estado pos-rebase

Branch parte de `2dde4ed` (`main`, v0.5.6). 5 commits a frente:

```
HEAD  docs(phase3-chunkD): function manual + roadmap flip + phase3 closeout
      docs(phase3): record dbt sources work in progress
      test(phase3-chunkC): firebird_dbt_sources sqllogic + composite-PK fixture
      feat(phase3-chunkB): real dbt sources.yml generator
      feat(phase3-chunkA): scaffold firebird_generate_dbt_sources stub
main  fix(community): restore empty vcpkg.json manifest
```

Hashes locais foram reescritos pelo rebase; nao foram publicados.

## Proximo passo (NAO executar nesta sessao)

Revisao PM primeiro. So depois - e somente se PM autorizar
explicitamente - sequencia de integracao:

- Squash em `review/phase3-dbt-sources` (2 commits: `feat` + `docs`,
  ou granularidade que o PM preferir).
- `git push` + PR contra `main` no proprio repo. **Sem mexer no
  duckdb/community-extensions PR #1980** (continua bloqueado em
  CI da community apos a tentativa do v0.5.6).
- CI Linux FB 3/4/5 + Linux x64 + Windows x64.
- Merge.
- Tag `v0.5.7` (ou outra numeracao decidida pelo PM) se a
  Fase 3 sair como release publica.

Nenhuma dessas etapas e iniciada antes do aceite explicito.
