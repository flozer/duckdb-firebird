# Fase 2 - Encerramento (Connection Pool)

Branch de desenvolvimento: `dev/phase2-connection-pool` (somente local
durante o ciclo). Para revisao tecnica / PR sera squashed em
`review/phase2-connection-pool`. Sem `git push`, sem tag `v*`, sem
release ate aprovacao final.

## Resumo

Fase 2 hardeniza o `FirebirdConnectionPool` que ja existia desde antes
da Fase 1 e expoe controle via `SET`. Surface publica final:

- `SET firebird_pool_enabled = true|false` (padrao `true`).
- `SET firebird_pool_max_size = N` (padrao `0` = ilimitado).
- `SET firebird_pool_idle_timeout_ms = MS` (padrao `0` = sem
  expiracao).
- `firebird_last_query().connection_id` / `connection_reused` deixam
  de ser placeholders e passam a refletir o lease real do pool.

## Historia

A historia chunk-a-chunk vive em `dev/phase2-connection-pool`:

1. Chunk A - `feat(phase2-chunkA)`: extensao da API interna do pool
   (id, `LeaseInfo`, `PoolConfig`, metricas internas). Sem settings,
   sem observability, sem testes publicos.
2. Chunk B - `feat(phase2-chunkB)`: registro dos 3 settings via
   `DBConfig::AddExtensionOption`. `FirebirdAttach` le no
   `ClientContext`, valida (`max_size >= 0`,
   `idle_timeout_ms >= 0`), monta `FirebirdConnectionPoolConfig` e
   passa para o `FirebirdCatalog`.
3. Chunk C - `feat(phase2-chunkC)`: scanner usa `AcquireWithInfo()`
   no caminho `ATTACH` e popula `connection_id` /
   `connection_reused` no slot da Fase 1. Schema publico nao mudou.
4. Chunk D - `test(phase2-chunkD)`: `test/sql/firebird_pool.test`
   (27 assertions), atualizacao de `docs/pt/function_manual.md` e
   `docs/en/observability.md` conforme premissa de `CONTRIBUTING.md`.

A branch de revisao final terah 2 commits limpos:

1. `feat(pool): add configurable ATTACH connection pool` -
   codigo + testes.
2. `docs(pool): close phase 2` - manuais + roadmap + este
   documento.

## Comportamento

### `ATTACH` com pool ligado (padrao)

- A inicializacao do catalogo abre uma conexao para descobrir
  esquema, devolve para o pool e libera o ATTACH.
- A primeira consulta SQL do usuario chega ao pool aquecido. Por
  isso o `connection_reused` ja vem `true` na primeira consulta;
  e aquecimento, nao bug.
- Consultas subsequentes reusam o mesmo `connection_id` (uma
  conexao na fila idle).
- `firebird_scan(...)` direto (sem `ATTACH`) continua fora do pool
  por design - cada chamada cria sua propria conexao one-shot.

### `ATTACH` com pool desligado (`firebird_pool_enabled = false`)

- Toda `Acquire` constroi conexao nova; todo `Release` destroi.
- Consultas consecutivas exibem `connection_id` diferentes.
- `connection_reused` permanece `false`.

### `firebird_pool_max_size`

- Limita a **fila idle**, nao os **leases ativos**: nada impede
  o scanner de paralelizar e ter mais conexoes ativas
  simultaneamente. O limite atua apenas no `Release`, dropando
  conexoes que ultrapassariam a fila.
- `0` (padrao) = sem limite.
- Valor negativo levanta `BinderException` no `ATTACH`.

### `firebird_pool_idle_timeout_ms`

- Conta tempo parado na fila idle, comecando no `Release()`.
- Nao expira conexao ativa ou em uso prolongado.
- Conexao vencida e descartada na proxima `Acquire`.
- `0` (padrao) = sem expiracao.
- Valor negativo levanta `BinderException` no `ATTACH`.

### Settings lidas no `ATTACH`

Os tres settings sao lidos no momento que o catalogo nasce.
`SET` posterior **nao retuna** pool ja existente. Para aplicar,
`DETACH` + `ATTACH` novamente.

## Aceite versus roadmap original

Roadmap interno (`docs/pt/roadmap_dev_firebird_analytics_platform.md`)
Fase 2:

| Item | Status |
|------|--------|
| Pool por banco anexado / connection string normalizada | ok (ja existia, validado) |
| Limite configuravel de conexoes idle | ok (`firebird_pool_max_size`) |
| Reuso seguro entre chamadas curtas | ok (LIFO preservado) |
| Fechamento limpo no detach / fim da conexao DuckDB | ok (nao mudou) |
| `max_pool_size` | ok |
| `pool_idle_timeout_ms` | ok |
| `pool_enabled` | ok |
| Metricas expostas: abertas / reutilizadas / descartadas | parcial - contadores `TotalCreated/Reused/Discarded` existem internamente; nao ha funcao SQL publica de introspecao ainda (divida) |
| Teste reuso duas queries via ATTACH | ok (`firebird_pool.test`) |
| Teste limite maximo de conexoes | parcial - opt-out coberto; teste de saturacao com `max_size` baixo nao incluido por nao caber em sqllogictest determinista |
| Teste scans paralelos respeitam `partitions=N` e limite | parcial - flake do Firebird Embedded local em `partitions=4` impede asserir saturacao do pool em ambiente Windows; CI Linux com servico real continua passando |
| Sem regressao nos testes existentes | ok (271 assertions verdes; flake do Firebird Embedded em `firebird_scan` e pre-existente) |

## Dividas registradas

1. Funcao SQL publica de introspecao do pool (`firebird_pool_stats()`
   ou similar) que exponha
   `TotalCreated`/`TotalReused`/`TotalDiscarded` + `IdleCount` por
   sessao. Tem dado interno, falta surface.
2. Teste deterministico de saturacao com `firebird_pool_max_size = 2`
   sob carga paralela. Hoje o Firebird Embedded local nao da garantia
   suficiente para assertir; ou se exercita via stress test fora do
   sqllogic, ou se aguarda um runner com servico real (mesma estrategia
   do CI).
3. `SET firebird_observability_redaction = 'strict' | 'debug'` segue
   como divida da Fase 1 - nao foi tocada nesta fase.

## Politica respeitada ao longo da Fase 2

- Branch `dev/phase2-connection-pool` ate o squash; `main` nunca
  tocada (verificado em cada chunk via
  `git status --short --branch`).
- Sem `git push`, sem tag `v*`, sem release durante o ciclo.
- Cada chunk validado localmente antes do proximo.
- Cada pausa de PM endereçada antes de avancar.

A mesma politica aplica a `review/phase2-connection-pool`: nada vai
para `main`, nada e empurrado, nenhuma tag `v*` e criada ate a
revisao tecnica aprovar.

## Validacao local

Build Windows: `scripts\build_windows_local.bat` (verde).

Sweep com `FIREBIRD_TEST_DB=C:\fbtest\smoke.fdb` (fixture extendida
mirror da CI Linux):

| Suite | Assertions |
|-------|------------|
| `firebird_pool` (novo) | 27 |
| `firebird_observability` | 40 |
| `firebird_attach` | 79 |
| `firebird_metadata` | 79 |
| `firebird_predicates` | 13 |
| `firebird_paging` | 16 |
| `firebird_bind_params` | 20 |
| Subtotal core | 274 (0 fail) |
| `firebird_scan` | flake `partitions=4` Firebird Embedded local (pre-existente, doc em `docs/pt/phase1_closeout.md`) |

CI Linux usa servico Firebird 3.0 real e nao reproduz a flake.

## Estado para revisao tecnica

- Branch `review/phase2-connection-pool` parte de `main`, 2 commits a
  frente (`feat(pool)` + `docs(pool)`).
- Worktree limpo.
- Roadmap publico (`docs/en/roadmap.md`) atualizado com itens 14
  (telemetry) e 15 (pool).
- Manual publico (`docs/pt/function_manual.md`) atualizado com os
  tres settings + caveat de catalog warm-up.
- Manual de observabilidade (`docs/en/observability.md`) deixou de
  marcar `connection_id` / `connection_reused` como reservados.
- `CONTRIBUTING.md` sem mudanca necessaria; premissa cumprida.

## Proximo passo (NAO executar nesta sessao)

Aprovacao tecnica primeiro. So depois:

- Decisao sobre release `v0.5.4`.
- Squash limpo na `review/phase2-connection-pool`.
- `git push` + abrir PR contra `main`.
- Aguardar CI Linux + Windows.
- Merge se verde.
- Criar tag annotated `v0.5.4` e confirmar assets
  `duckdb-firebird-0.5.4-{linux,windows}-x64.{tar.gz,zip}` no
  Release.

Nenhuma dessas etapas e iniciada antes do aceite da Fase 2.
