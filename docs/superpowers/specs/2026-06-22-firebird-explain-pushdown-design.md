# firebird_explain_pushdown — Design

- Data: 2026-06-22
- Frente: v0.7 Metadata & Explain (item 2 de 3)
- Status: aprovado (brainstorming), pendente plano de implementação
- Escopo: read-only, a-priori (plan-only). **Nenhuma ação em `duckdb/community-extensions` sem autorização explícita do Fernando.**

## Objetivo

`firebird_explain_pushdown(sql VARCHAR)` — explicador a-priori do pushdown. Faz
bind + otimização do SQL pelo caminho normal do DuckDB, localiza o(s) scan(s)
Firebird no plano otimizado, e reporta a query remota que o scanner
**produziria** — SQL remoto, projeção, filtros empurrados/residuais + motivo,
ROWS, e estratégia/elegibilidade de PK-range. **Nunca abre cursor; nunca envia
SQL ao Firebird alvo.** Restrito a `SELECT` / `WITH … SELECT` read-only.

Diferença vs `firebird_last_query()`: aquele é pós-execução (exige rodar a
query e pagar o custo remoto); este é a-priori e zero-custo no Firebird.

## Gate obrigatório: spike de integração com o planner

**Antes de comprometer a API**, validar que o DuckDB expõe de forma estável o
plano otimizado desta função para extração. O scanner atual já recebe, durante
a otimização, `LogicalGet` + `GetColumnIds()` + `TableFilterSet` +
`extra_predicates` + `gated_complex_reasons` + `limit_override` +
`offset_override`; o callback de filtros complexos **altera `bind_data` durante
a otimização**. Portanto a saída deve ser extraída do **plano já otimizado**,
sem reexecutar hooks manualmente e sem invocar `Build()` reentrantemente dentro
de um callback de scan.

Se o spike mostrar que o DuckDB não expõe o plano otimizado de forma estável
para esta função: **PARAR**. Não esconder um planner alternativo sob a mesma
API. O fallback "tabela + predicado" seria **outra função, com outra
semântica** — decisão separada do Fernando, fora deste spec.

## Arquitetura (quando o spike passar)

1. **Parse / validação.** Exatamente uma instrução `SELECT` ou
   `WITH … SELECT`. Rejeitar (BinderException) multi-statement, DDL, DML,
   `COPY`, `EXPLAIN`, `PRAGMA`, etc. — allow-list, não block-list.

   **Apenas tabelas de catálogo Firebird já anexado.** A função explica scans
   que vêm de um `ATTACH … (TYPE firebird)` (LogicalGet via storage extension,
   schema já cacheado). Deve **rejeitar** a table function direta
   `firebird_scan(connection_string, table)` no SQL interno: o bind dela
   carrega o schema abrindo uma **conexão Firebird** — violaria "nenhum
   SQL/cursor remoto". Detectar e rejeitar com BinderException acionável:
   `firebird_explain_pushdown: direct firebird_scan(...) is not supported here
   (it would open a Firebird connection at bind). ATTACH … (TYPE firebird) and
   reference alias.main.table instead.` A detecção ocorre **antes** de
   bindar/otimizar o SQL interno (para não disparar a conexão do
   firebird_scan): inspecionar a árvore parseada por uma table function de nome
   `firebird_scan` e abortar se presente.
2. **Bind + optimize.** Levar o SQL interno a plano lógico otimizado pelo
   caminho interno do DuckDB. Projeção / `TableFilterSet` / filtros complexos /
   limit-offset são decididos aqui — idêntico a um scan real.
3. **Travessia do plano otimizado.** Para cada `LogicalGet` ligado à table
   function Firebird, **copiar** do `bind_data` (e do LogicalGet) o que é
   necessário: `table_name`, nomes/tipos/descs de coluna, `none_encoding`,
   `column_ids`/projeção, `table_filters`, `limit_override`, `offset_override`,
   `extra_predicates`, `gated_complex_reasons`. Ordem de travessia
   determinística (uma linha por scan, na ordem em que aparecem no plano).
4. **Capture-only Build().** **Fora de qualquer callback de scan**, chamar o
   mesmo `FirebirdQueryBuilder::Build()` que o scanner usa, em modo
   capture-only: where-clause serial (sem PK bounds), sem cursor, sem conexão.
   Captura `remote_sql`, `pushed_filter_sql`, `residual_filter_indices`,
   `residual_filter_reasons`, projeção, limit/offset.
5. **PK-range (sem probe).** A partir do **descritor de PK cacheado pelo
   catálogo** (ATTACH), determinar elegibilidade + coluna + tipo + motivo +
   estratégia. Sem MIN/MAX, sem bounds, sem contagem de partições (seriam falsa
   precisão que o modo a-priori não possui).

Totalmente offline: só metadados já cacheados pelo ATTACH; **zero SQL novo ao
Firebird**, nenhum `OpenCursor`.

### Pré-requisito: ampliar o cache de PK do `FirebirdTableEntry`

O cache atual **não basta** para os quatro motivos normalizados offline. Hoje
o `FirebirdTableEntry` guarda `PrimaryKeyInfo` apenas para **PK numérica de
coluna única** (usado pelo particionamento por faixa); `nullptr` colapsa três
casos distintos — *sem PK*, *PK composta* e *PK não-numérica* — que
`pk_range_reason` precisa diferenciar.

Ampliar o cache, populado no ATTACH a partir dos metadados de constraint que o
catálogo **já carrega** (o `LoadUniqueConstraints` da Metadata Bridge 2.0 já lê
`RDB$RELATION_CONSTRAINTS`/`RDB$INDEX_SEGMENTS`) + os tipos de coluna já
presentes na entry — portanto **zero I/O novo**:

```
struct PrimaryKeyDescriptor {
    bool                        has_pk;        // existe PK?
    duckdb::vector<std::string> columns;       // colunas da PK, em ordem ordinal
    bool                        single_numeric;// 1 coluna E tipo numérico (INT/BIGINT)
};
```

Derivação dos quatro motivos (puramente do descritor):
- `!has_pk` → `no primary key`
- `columns.size() > 1` → `composite PK`
- `columns.size() == 1 && !single_numeric` → `non-numeric PK`
- `single_numeric` → `single numeric PK` (único caso elegível)

O `PrimaryKeyInfo` atual (usado pelo scanner para partição) permanece como
está; o descritor é metadado de catálogo adicional, não substitui a sonda de
partição em tempo de scan.

## Saída — uma linha por scan Firebird no plano

Listas paralelas (consistente com `firebird_last_query()` /
`firebird_query_log()`):

| coluna | tipo | nota |
|--------|------|------|
| `scan_ordinal` | BIGINT | preorder do plano otimizado, começando em 1 — contrato de ordem estável |
| `table_name` | VARCHAR | |
| `remote_sql` | VARCHAR | **somente placeholders `?`/binds do builder — nunca literais nem connection string** |
| `projected_columns` | LIST(VARCHAR) | |
| `pushed_filters` | LIST(VARCHAR) | inclui `extra_predicates` (LIKE-prefix, NOT IN, …) |
| `residual_filters` | LIST(VARCHAR) | |
| `not_pushed_reasons` | LIST(VARCHAR) | **invariante: `len(residual_filters) == len(not_pushed_reasons)`** |
| `limit_pushed` | BIGINT | NULL se ausente |
| `offset_pushed` | BIGINT | NULL se ausente |
| `rows_clause` | VARCHAR | o `ROWS m TO n` que aplicaria, ou NULL |
| `pk_range_eligible` | BOOLEAN | |
| `pk_range_column` | VARCHAR | NULL quando não há PK de coluna única |
| `pk_range_reason` | VARCHAR | valor **normalizado** (ver abaixo) |
| `scan_strategy` | VARCHAR | `serial` \| `pk-range-partitionable` |

Plano sem scan Firebird (ex.: `SELECT 1`, SQL puramente analítico) → **zero
linhas** (naturalmente componível; não é erro).

### Contrato de ordem

Uma linha por `LogicalGet` Firebird no plano otimizado, emitidas em **preorder
da travessia do plano**, com `scan_ordinal` 1-based atribuído nessa ordem. Num
self-join, as duas linhas têm o mesmo `table_name` mas `scan_ordinal` distinto
(1, 2) — resultado depurável e testes estáveis. Os testes asseram por
`scan_ordinal`, não por ordem implícita.

### Invariantes de cardinalidade

- `len(residual_filters) == len(not_pushed_reasons)` sempre.
- `gated_complex_reasons` entram como hoje no scanner: para cada um, adicionar
  `complex_filter[none_gated]` a `residual_filters` e o motivo a
  `not_pushed_reasons`, mantendo as duas listas com o mesmo comprimento.

### `pk_range_reason` — valores normalizados (estáveis, testáveis)

Exatamente um de:
- `no primary key`
- `composite PK`
- `non-numeric PK`
- `single numeric PK`

(Mensagens não devem depender de detalhes internos.) `pk_range_eligible = true`
e `scan_strategy = pk-range-partitionable` **somente** com `single numeric PK`;
caso contrário `false` / `serial`.

## Segurança / read-only

- Allow-list SELECT/CTE no parse; nenhuma execução, nenhum cursor, nenhum SQL
  remoto → inerentemente read-only e zero-custo no Firebird.
- `remote_sql` carrega só placeholders do builder — explain **não** pode virar
  vazamento de texto sensível (sem literais inline, sem connection string).

## Testes (determinísticos, sobre as fixtures existentes)

Novo `test/sql/firebird_explain_pushdown.test`:

- **WHERE empurrado:** `firebird_explain_pushdown('SELECT EMP_ID FROM fb.EMPLOYEE WHERE EMP_ID = 4')`
  → `pushed_filters` não-vazio, `remote_sql` contém o predicado como `?`,
  `projected_columns = [EMP_ID]`, `pk_range_eligible = true`,
  `pk_range_reason = 'single numeric PK'`, `scan_strategy = 'pk-range-partitionable'`.
- **Projeção (prune):** selecionar subconjunto → `projected_columns` subconjunto.
- **LIMIT:** `… LIMIT 100` → `rows_clause` / `limit_pushed`.
- **Filtro residual + motivo:** um filtro não empurrado → par
  `residual_filters` + `not_pushed_reasons` (comprimentos iguais).
- **CHARACTER SET NONE / NOT IN:** garante o par residual+reason e o caminho
  `complex_filter[none_gated]` (usar `none.fdb` / coluna NONE).
- **Quatro motivos `pk_range_reason`** (cobertos pela fixture existente, todos
  offline a partir do descritor de PK):
  - `EMPLOYEE` (EMP_ID INTEGER) → `single numeric PK`, `eligible = true`, `pk-range-partitionable`
  - `DEPT` (DEPT_NO VARCHAR(3) PK) → `non-numeric PK`, `eligible = false`, `serial`
  - `TPK_COMPOSITE` (PK A,B) → `composite PK`, `eligible = false`, `serial`
  - `TCHILD` (sem PK) → `no primary key`, `eligible = false`, `serial`
- **Dois scans Firebird no mesmo SQL** — incluir um **self-join** (mesma
  tabela duas vezes): duas linhas, mesmo `table_name`, `scan_ordinal` 1 e 2;
  asserir por `scan_ordinal`.
- **`SELECT 1`** (sem scan Firebird) → zero linhas.
- **Rejeição de `firebird_scan(...)` direto:**
  `firebird_explain_pushdown('SELECT * FROM firebird_scan(''...'', ''EMPLOYEE'')')`
  → erro acionável (matcher), sem abrir conexão.
- **`WITH` válido** (`WITH x AS (SELECT … FROM fb.EMPLOYEE) SELECT … FROM x`)
  → explica o scan dentro do CTE.
- **Guarda read-only:** `WITH … DELETE` e `UPDATE …` → erro (matcher de erro).
- **Nenhum cursor aberto:** preferencialmente instrumentar/mng `OpenCursor`
  (ou, se estabilizável, fixture com credencial de leitura inválida após o
  ATTACH já carregado — a função deve ter sucesso mesmo assim, provando que não
  toca o Firebird). Se nenhuma das duas for estável no harness, registrar como
  limitação e cobrir por inspeção de código no review.
- **`remote_sql` sem literais:** assertar que o SQL emitido contém `?` e **não**
  contém o valor literal do filtro nem a connection string.

Validação cross-version: `scripts/build_matrix.ps1` (v1.5.2 / v1.5.3 / v1.5.4),
adicionando `firebird_explain_pushdown.test`.

## Arquivos

- Novo `src/firebird_explain_pushdown.cpp` + `src/include/firebird_explain_pushdown.hpp`.
- Registrar a table function em `src/firebird_extension.cpp`.
- `src/firebird_storage.cpp` (+ `firebird_scanner.hpp`): ampliar o cache de PK
  do `FirebirdTableEntry` para `PrimaryKeyDescriptor` (cardinalidade + colunas +
  `single_numeric`), populado no ATTACH a partir das constraints já carregadas.
- Reusar `FirebirdQueryBuilder::Build()` (capture-only) + descritor de PK do catálogo.
- Separado do scanner para não inchá-lo.

## Fora de escopo

- Bounds reais de PK / contagem de partições (exigiria probe MIN/MAX = SQL ao Firebird).
- Não-SELECT / multi-statement / COPY / EXPLAIN / DML / DDL.
- Execução / timings / rows_read (isso é `firebird_last_query()`).
- Fallback "tabela + predicado" (outra função, outra semântica — só se o spike falhar e o Fernando autorizar).
- Qualquer ação community/upstream.
