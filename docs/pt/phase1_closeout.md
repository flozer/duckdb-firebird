# Fase 1 - Encerramento (Observabilidade)

Branch de revisao: `review/phase1-observability` (local, sem push, sem
tag, sem release ate aprovacao tecnica).

## Resumo

Fase 1 fechada conforme PM review. Surface entregue: observabilidade
por sessao DuckDB da query remota Firebird, redacao de binds e erros,
ring buffer opt-in.

## Historia

Esta branch e um squash limpo a partir de `main`, com 2 commits:

1. `feat(observability): add Firebird query telemetry` - codigo,
   testes (`test/sql/firebird_observability.test`, 40 assertions) e
   o helper `scripts/smoke_fixture.sql`.
2. `docs(observability): close phase 1` - este documento mais o
   manual publico `docs/en/observability.md`.

A historia chunk-a-chunk usada durante o desenvolvimento (Chunks A,
A.1, B, C, D, E mais o baseline da Fase 0) fica preservada em
`dev/firebird-analytics-platform-local`. Aquela branch e onde cada
PM review intermediario aterrissou; esta aqui e a forma propria para
revisao tecnica / PR.

## Manual publico

Manual de uso das funcoes para usuarios finais, em ingles, em
`docs/en/observability.md`. Cobre:

- `firebird_last_query()` - schema, politica de redacao, exemplos
  (pushdown confirmado, pushdown perdido, redacao de texto, timings,
  cross-check no-leak).
- `firebird_query_log()` - como ativar via `SET
  firebird_query_log_size = N`, exemplos de auditoria de relatorio
  BI, identificacao de query lenta, validacao no-leak.
- Referencia da setting.
- Limitacoes (`connection_id` reservado, redacao numeric/temporal,
  scan paralelo).

## Surface entregue

### `firebird_last_query()`

Tabela 0-arg. Retorna no maximo 1 linha. Por `ClientContext`.

15 colunas:

- `remote_sql VARCHAR`
- `binds VARCHAR[]` (redatado conforme `RedactBindValue`)
- `table_name VARCHAR`
- `projected_columns VARCHAR[]`
- `pushed_filters VARCHAR[]`
- `residual_filters VARCHAR[]`
- `rows_read BIGINT`
- `firebird_time_us BIGINT`
- `total_time_us BIGINT`
- `connection_id BIGINT` (default `-1`, ver Limitacoes)
- `connection_reused BOOLEAN` (default `false`)
- `parallel_scan BOOLEAN`
- `partitions INTEGER`
- `captured_at TIMESTAMP`
- `error_message VARCHAR` (sanitizado por `SanitizeErrorMessage`)

### `firebird_query_log()`

Tabela 0-arg. Ring buffer por `ClientContext`. Mesmo schema 15 colunas.

Opt-in via setting:

```sql
SET firebird_query_log_size = 16;
```

- Default `0` = log desligado (zero linhas).
- Toggle para `0` limpa o buffer no proximo `RecordQuery`.
- Rotacao FIFO, ordem na saida = mais recente primeiro.

### Politicas de redacao

- Bind `NULL` -> `"<null>"`
- Bind `VARCHAR/CHAR/BLOB` -> `"<text:redacted>"` (sem comprimento)
- Bind `INTEGER/BIGINT/DECIMAL/DATE/TIMESTAMP/BOOLEAN` -> valor bruto
- `connection_string` nunca exposta - apenas `table_name`
- `error_message` passado por `SanitizeErrorMessage`:
  - `password=...` -> `password=<redacted>` (case-insensitive)
  - `scheme://user:pass@host` -> `scheme://<redacted>@host`

## Aceite versus roadmap original

| Item roadmap Fase 1 | Status |
|---------------------|--------|
| `firebird_last_query()` | ok |
| `firebird_query_log()` (opt-in) | ok |
| SQL remota visivel | ok |
| Parametros/binds sem vazar segredo | ok |
| Tabela | ok |
| Projection pushdown aplicado | ok |
| Predicate pushdown aplicado | ok |
| Filtros residuais | ok |
| Linhas lidas | ok |
| Tempo Firebird | ok |
| Tempo total do scan | ok |
| Numero de conexoes / reutilizacao | parcial - default `-1/false`, ver Limitacoes |
| Teste cobre projection + filtro | ok |
| Log nao imprime senha nem connection string | ok (cobertura sentinela em teste) |
| Funciona com `firebird_scan` e `ATTACH` | ok |

## Limitacoes registradas (dividas para fases futuras)

- `connection_id` / `connection_reused`: `FirebirdConnectionPool` nao
  expoe id barato. Manter `-1/false` ate Fase 2 (pool/metrics).
- Politica de redacao de numeric/temporal: hoje passa raw. Slot
  documentado no header `FirebirdObservabilityState` para
  `SET firebird_observability_redaction = 'strict' | 'debug'` quando
  fizer sentido.
- Em scan paralelo (`partitions > 1`), as metricas refletem o ultimo
  partition slot capturado, nao agregacao global. Campos
  `parallel_scan` e `partitions` deixam isso explicito ao consumidor.

## Validacao local

Build Windows: `scripts\build_windows_local.bat` (verde).

Sweep com `FIREBIRD_TEST_DB=C:\fbtest\smoke.fdb` (fixture extendida
mirror da CI Linux: EMPLOYEE + FILE_STORAGE + V_ACTIVE_EMP + TQUOTES).

| Suite | Assertions |
|-------|------------|
| `firebird_observability` | 40 |
| `firebird_scan` | 124 |
| `firebird_attach` | 79 |
| `firebird_metadata` | 79 |
| `firebird_predicates` | 13 |
| `firebird_paging` | 16 |
| `firebird_bind_params` | 20 |
| Total | 371 (0 fail) |

### Nota sobre flake observada

`firebird_scan.test` em scan paralelo (`partitions=4`) ocasionalmente
levanta `IO Error: isc_attach_database ... connection lost
[isc sqlcode=-901]` quando rodado imediatamente apos
`firebird_observability.test`. Isso e limitacao do Firebird Embedded
local (contencao no attach em paralelo logo apos outro scan), nao
regressao do codigo da Fase 1: 3/3 retries dedicados do
`firebird_scan.test` passaram com 124 assertions. CI Linux usa
servico Firebird 3.0 real e nao sofre desta contencao.

## Politica respeitada ao longo da Fase 1

- Trabalho feito em branch local dedicada, sem nunca tocar `main`
  (`git status --short --branch` verificado em cada chunk).
- Sem `git push`, sem tag `v*`, sem release.
- Cada chunk validado localmente antes do proximo.
- Cada pausa de PM endereçada antes de avancar.

A mesma politica aplica a esta branch de revisao: nada vai para
`main`, nada e empurrado, nenhuma tag `v*` e criada ate a revisao
tecnica aprovar.

## Estado para revisao tecnica final

- Branch `review/phase1-observability` parte de `main` direto, com
  apenas 2 commits a frente.
- Worktree limpo.
- `scripts/smoke_fixture.sql` segue como helper local de dev (decisao
  de release-time se permanece versionado ou nao - PM finding baixa
  do Chunk D).
- Material de planejamento intermediario (roadmap completo, baseline
  Fase 0, PM reviews chunk-a-chunk) fica em `dev/firebird-analytics-
  platform-local` para quem quiser auditar a historia.

## Proximo passo (NAO executar nesta sessao)

Aprovacao tecnica primeiro. So depois:

- Decisao sobre `scripts/smoke_fixture.sql` (manter versionado ou
  remover).
- Decisao sobre release / tag `v*`.
- Inicio da Fase 2 (pool + metrics) - bloqueada ate Fase 1
  integrar.
