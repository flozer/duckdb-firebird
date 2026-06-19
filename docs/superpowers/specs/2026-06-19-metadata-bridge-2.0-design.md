# Metadata Bridge 2.0 — Design

- Data: 2026-06-19
- Frente: v0.7 Metadata & Explain (item 1 de 3)
- Status: aprovado (brainstorming), pendente plano de implementação
- Escopo: read-only. **Nenhuma ação em `duckdb/community-extensions` (PR/update/merge) sem autorização explícita do Fernando.** Compatibilidade DuckDB não autoriza ação upstream.

## Objetivo

Tornar o metadata do Firebird compreensível para DuckDB, dbt e BI sem mapping
manual fora da extensão. Surfacing completo de PKs, FKs, índices, domains,
computed columns, generators/sequences, comentários, nullability real,
constraints e dependências a partir das system tables `RDB$`.

## Decisões de arquitetura

### Superfície híbrida

**information_schema padrão** — onde o mapeamento DuckDB é fiel e estável.
Populado pelo catálogo (não por função): ao carregar uma `FirebirdTableEntry`,
anexar os `Constraint` objects ao `CreateTableInfo`; o DuckDB então deriva as
views padrão automaticamente — mesma mecânica que hoje já produz
`information_schema.columns.is_nullable` a partir do `NotNullConstraint`.

Views cobertas:

- `information_schema.columns` — nullability real, tipo, ordinal (já existe; manter)
- `information_schema.table_constraints` — PRIMARY KEY, UNIQUE, FOREIGN KEY
- `information_schema.key_column_usage` — colunas das constraints, em ordem ordinal
- `information_schema.referential_constraints` — FK → unique/PK referenciada,
  regras ON UPDATE / ON DELETE

**Table functions FB-específicas** — para o que não tem equivalente fiel em
DuckDB. Schema explícito e tipado por função, `ORDER BY` determinístico
server-side, nomes/tipos Firebird preservados como string fiel
(ex.: `NUMERIC(10,2)`, `VARCHAR(60) CHARACTER SET WIN1252`). Não forçar
semântica DuckDB onde perderia informação. Estilo segue `firebird_tables` /
`firebird_types`.

Não há `firebird_constraints` standalone: PK/UNIQUE/FK saem no
information_schema; duplicar seria ruído. CHECK de tabela fica **fora do
incremento 1** (no Firebird vive em triggers/`RDB$CHECK_CONSTRAINTS`,
mapeamento sujo); reavaliar em frente futura se houver demanda.

### Por que catálogo para constraints e função para o resto

information_schema é o contrato que dbt/BI já consomem sem aprender API nova.
Generators, domains, computed columns, dependencies e comments não têm view
padrão fiel — função tipada preserva o detalhe Firebird sem distorção.

## Convenções de nome e schema (todas as funções)

- **`object_schema` / `table_schema`**: valor canônico `'main'` — a convenção
  atual da extensão (Firebird não tem schemas; o catálogo já expõe `fb.main.*`).
  Constante literal, não derivada do `RDB$`.
- **Identificadores**: os nomes em `RDB$` são `CHAR` com padding de espaços.
  Aplicar `RTRIM` (server-side ou na projeção) em todo identificador
  — relation, field, index, constraint, generator, domain — antes de devolver.
- **Tipos/códigos desconhecidos**: nunca dropar a linha. Onde há mapeamento
  código→texto (ex.: `object_type`, `depends_on_type`), código sem mapeamento
  conhecido vira `object_type = 'UNKNOWN'` e o código numérico fiel permanece
  na coluna `*_type_code` própria. Mapeamento de tipo de dado FB desconhecido
  vira a string crua do código, nunca NULL silencioso.

## Contrato das funções FB-específicas

Todas: argumento = nome do catálogo ATTACHed (ex.: `'fb'`). Read-only.
Ordenação determinística. Schema fixo (sem colunas heterogêneas).
Convenções de nome/schema acima aplicam-se a todas.

### `firebird_indexes('fb')`

Colunas: `table_schema`, `table_name`, `index_name`, `is_unique` (BOOLEAN),
`is_active` (BOOLEAN), `segment_position` (INTEGER, ordinal do segmento),
`column_name` (NULL quando índice por expressão), `expression_source`
(VARCHAR, NULL quando índice por coluna).

Regra: **preservar a distinção entre UNIQUE constraint e índice único.**
`is_unique` reflete o índice. NÃO inferir constraint UNIQUE só do índice — a
existência de constraint é apurada pelo catálogo (information_schema), não por
esta função. Origem: `RDB$INDICES` + `RDB$INDEX_SEGMENTS` (+ expression source
quando `RDB$INDICES.RDB$EXPRESSION_SOURCE` presente).

### `firebird_generators('fb')`

Colunas: `generator_name`, `initial_value` (BIGINT, NULL se indisponível),
`current_value` (BIGINT, NULL se indisponível).

Regras:
- **Não expor "increment" como metadado geral** — generator Firebird não tem
  incremento fixo (o passo é por chamada `GEN_ID`).
- `initial_value` de `RDB$GENERATORS.RDB$INITIAL_VALUE` quando disponível.
- `current_value` lido via `GEN_ID(<nome>, 0)` — passo 0 não altera o valor
  (sem efeito colateral).
- **Quoting seguro**: o nome do generator é interpolado como identificador
  Firebird entre aspas duplas, com escape de aspas internas
  (`"` → `""`). Nunca como literal/string concatenada. Defesa contra nome com
  caractere especial ou aspas.
- **Isolamento por generator**: cada `current_value` é lido numa execução
  isolada. Falha de privilégio ou erro em um generator → `current_value = NULL`
  **somente naquela linha**; a função não aborta nem perde as demais linhas.
  `initial_value` (lido de `RDB$GENERATORS`) permanece independente disso.
- Origem: `RDB$GENERATORS` (filtrar `RDB$SYSTEM_FLAG = 0`).

### `firebird_domains('fb')`

Colunas: `domain_name`, `base_type` (string fiel FB), `length`, `scale`,
`is_nullable` (BOOLEAN), `charset_name` (NULL quando não aplicável),
`check_source` (VARCHAR, NULL), `default_source` (VARCHAR, NULL).
Origem: `RDB$FIELDS` (domains nomeados, `RDB$SYSTEM_FLAG = 0`, nome sem
prefixo `RDB$`).

### `firebird_computed_columns('fb')`

Colunas: `table_schema`, `table_name`, `column_name`, `expression_source`
(VARCHAR). Origem: `RDB$RELATION_FIELDS` join `RDB$FIELDS` onde
`RDB$COMPUTED_SOURCE`/`RDB$COMPUTED_BLR` presente.

### `firebird_dependencies('fb')`

Colunas legíveis + códigos Firebird fiéis (não perder fidelidade):
`object_name`, `object_type` (VARCHAR legível), `object_type_code` (INTEGER,
`RDB$DEPENDENT_TYPE`), `depends_on_name`, `depends_on_type` (VARCHAR legível),
`depends_on_type_code` (INTEGER, `RDB$DEPENDED_ON_TYPE`), `field_name` (NULL
quando dependência não é a nível de coluna).
Origem: `RDB$DEPENDENCIES`.

### `firebird_comments('fb')`

Colunas: `object_schema`, `object_name`, `object_type`, `column_name`
(NULL **somente** quando o comentário não é de coluna), `comment` (VARCHAR).

Tipos de comentário suportados no incremento 1 (enumerados, sem
"demais objetos"):

- `TABLE` — `RDB$RELATIONS.RDB$DESCRIPTION` onde `RDB$VIEW_BLR IS NULL`, `column_name = NULL`
- `VIEW` — `RDB$RELATIONS.RDB$DESCRIPTION` onde `RDB$VIEW_BLR IS NOT NULL`, `column_name = NULL`
- `COLUMN` — `RDB$RELATION_FIELDS.RDB$DESCRIPTION`, `column_name` preenchido

Comentário em domain/generator/index/constraint fica fora do incremento 1
(frente futura se houver demanda). Apenas linhas com `RDB$DESCRIPTION` não-nulo
(`RDB$SYSTEM_FLAG = 0`).

## Mecanismo de implementação

1. **Catálogo** (`src/firebird_storage.cpp`, `FirebirdTableEntry`):
   no load da tabela, além das colunas + `NotNullConstraint` já existentes,
   ler constraints de `RDB$RELATION_CONSTRAINTS` (+ `RDB$INDEX_SEGMENTS` para
   colunas, + `RDB$REF_CONSTRAINTS` para FK rules) e anexar ao
   `CreateTableInfo.constraints`:
   - PK / UNIQUE → `UniqueConstraint` (flag `is_primary_key` no PK)
   - FK → `ForeignKeyConstraint` (colunas, tabela/constraint referenciada,
     ON UPDATE/ON DELETE)
   O DuckDB gera `table_constraints`/`key_column_usage`/`referential_constraints`.

   **Verificação empírica (Task 4, 2026-06-19) — CONCLUÍDA:**
   - **ATTACH**: OK. `ForeignKeyConstraint` anexado ao `CreateTableInfo` de
     `FirebirdTableEntry` em catálogo read-only é aceito sem erro.
   - **`information_schema.table_constraints` FK rows**: PRESENTE. EMPLOYEE (1)
     e TCHILD (1) aparecem com `constraint_type = 'FOREIGN KEY'`. Test GREEN.
   - **`information_schema.referential_constraints` rules**: PRESENTE mas regras
     são PLACEHOLDER. DuckDB gera a linha (constraint_name, unique_constraint_name
     preenchidos), mas `update_rule`/`delete_rule` ficam sempre `'NO ACTION'`
     independentemente das regras Firebird reais (CASCADE, SET NULL etc.).
     `match_option` = `'NONE'`. A `ForeignKeyInfo` não transporta actions; o DuckDB
     não tem campo para recebê-las na parser layer.
   - **Conclusão para Task 5**: `firebird_foreign_keys('fb')` é obrigatória para
     expor as regras ON UPDATE/ON DELETE reais. O information_schema cobre
     presença de FK e referência à constraint pai; as regras são domínio exclusivo
     da função FB. `table_constraints` e `key_column_usage` permanecem fonte
     primária. `firebird_foreign_keys('fb')` cobre **apenas os campos que o DuckDB
     não propagar** — especialmente `update_rule`/`delete_rule` — e nunca substitui
     o information_schema. Origem: `RDB$REF_CONSTRAINTS` + `RDB$RELATION_CONSTRAINTS`
     + `RDB$INDEX_SEGMENTS`.
2. **Funções FB**: cada uma é uma table function read-only que executa a query
   `RDB$` correspondente pelo client existente, projeta o schema tipado fixo e
   ordena no servidor. Reaproveitar infra de `firebird_tables`/`firebird_types`.

## Expansão de fixture (idempotente, determinística)

`test.fdb` — via `scripts/setup_test_firebird.sh` (Linux/CI, recria o DB do
zero) **e** `scripts/smoke_fixture.sql` (Windows). Manter os dois alinhados.
Adicionar:

- Tabela pai `DEPT(DEPT_NO VARCHAR(3) PRIMARY KEY, DEPT_NAME VARCHAR(40))`
- FK `EMPLOYEE.DEPT_NO → DEPT(DEPT_NO)` com `ON UPDATE`/`ON DELETE` explícitos
- FK composta dedicada para cobrir colunas compostas + posição ordinal
  (ex.: tabela filha referenciando `TPK_COMPOSITE(A, B)`)
- `DOMAIN D_SALARY` (NUMERIC(10,2) + CHECK)
- `GENERATOR GEN_EMP_ID` com `INITIAL VALUE` conhecido
- `COMMENT ON TABLE EMPLOYEE` + `COMMENT ON COLUMN EMPLOYEE.EMP_NAME`
- 1 computed COLUMN (além do índice computed `EMP_UPPER_NAME_IDX` já presente)

Idempotência: ambos os scripts recriam o conteúdo do zero (sem estado
acumulado). Aplicar a provisão local em `C:\fbtest\test.fdb`.

`decfloat.fdb` / `none.fdb`: **inalterados** — nenhum teste novo os exige;
manter determinístico.

Cascata aceita e determinística: contagem de tabelas 9 → 10 (+ filha composta),
contagens de coluna e ordering de `firebird_dbt_sources.test` atualizados.

## Plano de teste

Arquivo único `test/sql/firebird_metadata_bridge.test`, organizado em blocos
por superfície (contrato completo visível, sem fragmentação):

- Bloco `information_schema`:
  - `table_constraints`: PK simples, PK composta (`TPK_COMPOSITE`), UNIQUE, FK
  - `key_column_usage`: ordinal correto, incl. FK composta
  - `referential_constraints` (ou `firebird_foreign_keys` se o info_schema não
    carregar as regras — ver risco acima): **FK composta — colunas, posição
    ordinal, constraint referenciada, ON UPDATE/ON DELETE explícitos**
- Bloco por função FB (linhas determinísticas):
  `firebird_indexes` (incl. distinção unique-index vs UNIQUE-constraint e
  índice por expressão), `firebird_generators` (initial_value; current_value
  ou NULL documentado), `firebird_domains`, `firebird_computed_columns`,
  `firebird_dependencies` (códigos FB + nomes), `firebird_comments`
  (column_name NULL só sem coluna)

Atualizar em cascata: `firebird_metadata.test` (counts) e
`firebird_dbt_sources.test` (ordering com `DEPT`/novos objetos).

Validação cross-version: rodar `scripts/build_matrix.ps1` (v1.5.2/v1.5.3/v1.5.4,
já verde) — comportamento deve permanecer idêntico nas três.

## Fora de escopo

- Qualquer escrita no Firebird: CREATE/ALTER seguem `Unsupported`.
- CHECK constraint de tabela (mapeamento via trigger — frente futura se houver demanda).
- `firebird_explain_pushdown` e `firebird_type_audit`: specs separados, próximos da fila v0.7.
- Qualquer ação community/upstream.
