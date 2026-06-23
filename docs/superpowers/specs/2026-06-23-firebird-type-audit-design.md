# firebird_type_audit — Design

- Data: 2026-06-23
- Frente: v0.7 Metadata & Explain (item 3 de 3)
- Status: aprovado (brainstorming), pendente plano de implementação
- Escopo: read-only, catálogo. **Nenhuma ação em `duckdb/community-extensions` / upstream sem autorização explícita do Fernando.**
- Base de implementação: branch a partir de `origin/main` (estado merged: itens 1 e 2 da v0.7).

## Objetivo

`firebird_type_audit('fb')` — auditoria de **fidelidade de tipos e charsets** entre o tipo nativo Firebird e a projeção DuckDB. Foco em **risco operacional de conversão**, não inventário. Emite **uma linha apenas para colunas com ressalva** (findings-only); colunas triviais (INTEGER, VARCHAR UTF8, DATE, NUMERIC/DECIMAL lossless ≤38) não aparecem.

Mesma família read-only das funções da Metadata Bridge 2.0 (`firebird_domains`, etc.): lê só catálogo `RDB$`, zero leitura de dados, `ORDER BY` determinístico, reaproveita a máquina de tipos de `firebird_types.cpp` / `FirebirdColumnDesc`.

## Superfície e schema

`firebird_type_audit('fb')` (alias de catálogo ATTACHed). Saída:

| coluna | tipo | nota |
|---|---|---|
| `table_schema` | VARCHAR | `'main'` |
| `table_name` | VARCHAR | |
| `column_name` | VARCHAR | |
| `firebird_type` | VARCHAR | string fiel FB (`VARCHAR(60) CHARACTER SET NONE`, `DECFLOAT(34)`, `BLOB SUB_TYPE 1`, `INT128`, `NUMERIC(38,4)`, `TIMESTAMP WITH TIME ZONE`) |
| `duckdb_type` | VARCHAR | projeção DuckDB (`VARCHAR`, `HUGEINT`, `TIMESTAMP WITH TIME ZONE`, `TIME WITH TIME ZONE`) |
| `finding` | VARCHAR | código normalizado (abaixo) |
| `detail` | VARCHAR | ressalva de 1 linha |

## Códigos `finding` normalizados (exatamente 6; um por linha)

1. **`none_charset`** — coluna texto `CHARACTER SET NONE`. `detail`: a semântica de decodificação **depende da setting `none_encoding` do scan** (padrão documentado `win1252`); a função **não** lê essa setting, então **não afirma** um encoding efetivo — apenas registra a dependência. Strict pode rejeitar; round-trip não garantido.
2. **`decfloat_as_varchar`** — `DECFLOAT(16|34)` projetado como `VARCHAR` via CAST server-side (sem decoder decimal-float nativo); semântica textual. (FB4+.)
3. **`int128`** — **um código único** para: `INT128` nativo **e** `NUMERIC`/`DECIMAL` com precisão 19–38. Ambos projetam em `HUGEINT` (lossless, 128-bit). `detail`: lossless, mas 128-bit — ressalva p/ ferramentas BI sem int128. (FB4+ para INT128; NUMERIC(19-38) também.)
4. **`blob_text`** — `BLOB SUB_TYPE 1` (texto) projetado como `VARCHAR`; ressalva de charset + tamanho.
5. **`time_tz`** — `TIME WITH TIME ZONE` → `TIME WITH TIME ZONE` DuckDB; ressalva de offset de sessão / zone-id. (FB4+.)
6. **`timestamp_tz`** — `TIMESTAMP WITH TIME ZONE` → idem. (FB4+.)

**Removido do escopo (decisão):**
- `numeric_precision` — Firebird suporta precisão decimal até 38, limite também aceito pelo DuckDB → seria código morto. Fora.
- `blob_binary` — `BLOB SUB_TYPE 0` já preserva fidelidade como `BLOB`; o escopo é **risco de conversão**, não inventário de tipos. Fora.

## Mecanismo

- Valida alias Firebird ATTACHed (`ValidateFirebirdAttachAlias`).
- Uma query `RDB$RELATION_FIELDS ⋈ RDB$FIELDS` por todas as tabelas de usuário (`RDB$SYSTEM_FLAG = 0`), trazendo `RDB$FIELD_TYPE`, `RDB$FIELD_SUB_TYPE`, `RDB$FIELD_PRECISION`, `RDB$FIELD_SCALE`, `RDB$FIELD_LENGTH`, `RDB$CHARACTER_SET_ID`.
- Para cada coluna, classifica via a mesma lógica de `firebird_types.cpp` (a que produz o `LogicalType` projetado + detecta NONE / DECFLOAT / INT128 / TZ / BLOB-text). Se a coluna casa um dos 6 findings → emite linha; senão omite.
- `firebird_type` montado como string fiel FB; `duckdb_type` = `LogicalType::ToString()` da projeção.
- Catálogo já cacheado pelo ATTACH; **nenhum cursor de dados**, nenhum SQL na query do usuário. `ORDER BY` determinístico (table, column).

## Testes (determinísticos)

Split por compatibilidade de versão Firebird (DECFLOAT/INT128/TZ são **FB4+**):

- **`test/sql/firebird_type_audit.test`** (FB-agnóstico, roda em todas as versões):
  - `none_charset`: `none.fdb` (`TXT.LABEL`/`NOTE` CHARACTER SET NONE).
  - `blob_text`: `test.fdb` (`EMPLOYEE.NOTE` BLOB SUB_TYPE 1).
  - Asserta findings exatos + que colunas triviais (EMP_ID etc.) **não** aparecem.
- **`test/sql/firebird_type_audit_fb4.test`** (condicional FB4+; roda em fb4/fb5 + local; `require-env` da fixture FB4+):
  - `decfloat_as_varchar`, `int128` (INT128 nativo **e** NUMERIC/DECIMAL 19–38), `time_tz`, `timestamp_tz` — contra a fixture FB4+ expandida.

**Fixture FB4+:** expandir a fixture FB4+ **já existente** (`scripts/fixture_biz4.sql` / `FB4_TYPES`, usada no smoke FB4+ do fb-matrix) com colunas para INT128, `NUMERIC/DECIMAL(38)`, `DECFLOAT`, `TIME WITH TIME ZONE`, `TIMESTAMP WITH TIME ZONE` conforme faltarem. **Não contaminar** a fixture canônica compatível com FB3 (`setup_test_firebird.sh` / inline do fb-matrix / `smoke_fixture.sql`). O teste FB4+ é separado e condicional.

Matriz cross-version `scripts/build_matrix.ps1` (v1.5.2/3/4): adicionar `firebird_type_audit.test`; o FB4+ entra no caminho condicional (matrix FB4+ / local FB5).

## Arquivos

- Função em `src/firebird_metadata_functions.cpp` (reusa scaffold + helpers `TextOrNull`/`ShortOrNull`; classificação via `firebird_types.cpp`), declarada no header, registrada em `src/firebird_extension.cpp`.
- `test/sql/firebird_type_audit.test` + `test/sql/firebird_type_audit_fb4.test`.
- Expansão de `scripts/fixture_biz4.sql` (FB4+).
- Docs PT/EN (`function_manual.md`), Nível 4 (diagnóstico), com os 6 findings e exemplo.

## Fora de escopo

- Reescrever o mapeamento de tipos ou adicionar conversões lossy novas.
- `numeric_precision`, `blob_binary` (acima).
- Ler a setting `none_encoding` do scan (a função só registra a dependência + default documentado).
- Qualquer ação community/upstream.
