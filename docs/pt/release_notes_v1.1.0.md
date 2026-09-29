# Notas de release — v1.1.0

Release de features que fecha a fila de dividas pos-v1.0.2 (F1–F5).
Publicada a partir do `main`; a atualizacao do catalogo DuckDB Community
e um passo separado, com autorizacao explicita.

## Destaques

### `firebird_profile_table`: estimativa de linhas + alertas estruturados de filtro obrigatorio

- Colunas novas `estimated_rows` / `row_estimate_method`: por padrao, um
  limite superior barato pela faixa da PK (`MAX - MIN + 1`, assume chave
  densa); `NULL` para tabelas sem PK numerica de coluna unica utilizavel.
- Parametro nomeado novo `exact_row_count=true`: `COUNT(*)` opt-in no
  servidor (metodo `exact_count`), observavel pelo alerta novo
  `exact_count_executed` (LOW). A estimativa nunca roda `COUNT(*)` por
  conta propria.
- Recomendacoes de filtro obrigatorio estruturadas para tabelas base de
  risco HIGH: `filter_before_scan` (MEDIUM, nomeia as colunas filtraveis)
  e `materialize_before_scan` (HIGH, quando nao existe candidato a
  filtro/watermark). Views mantem a orientacao propria.

### `firebird_pool_stats`: leases ativos + ultimo erro

- Colunas novas `active_connections` (leases entregues e ainda nao
  devolvidos) e `last_error` (mensagem sanitizada da criacao de conexao
  com falha mais recente; senha redigida, comprimento limitado; so
  falhas posteriores ao `ATTACH` sao registradas). Saida cresce de 8
  para 10 colunas.

### Dividas de CI/testes fechadas

- `firebird_decfloat.test` promovido ao passo da suite principal (roda
  nas pernas FB4/FB5; FB3 pula explicitamente); `setup_test_firebird.sh`
  provisiona um `decfloat.fdb` dedicado e exporta a variavel em
  servidores Firebird 4+.
- Novo `firebird_partitions_scan.test` (nas tres pernas do Firebird) com
  fixture dedicada: um span de PK esparso de ~12M recomenda
  particionamento real e um scan real com `partitions=4` devolve cada
  linha exatamente uma vez; cobre tambem o caso
  `materialize_before_scan` do profile.

### Compatibilidade

- **DuckDB v1.5.6 validada** (lancada em 2026-09-28): build limpo, suite
  completa 21/21 arquivos (903 assercoes), identica ao baseline v1.5.3.
  Sem drift de API na superficie usada pela extensao. O pin de build
  permanece v1.5.3 de proposito, ate o catalogo DuckDB Community
  Extensions mover o proprio alvo (hoje v1.5.5). Veja
  [docs/pt/duckdb_1_5_compatibility_plan.md](duckdb_1_5_compatibility_plan.md).

## Validacao

- Suite completa de sqllogictest verde na CI (Linux x64, Windows x64,
  matriz FB 3/4/5) e localmente via `scripts/build_matrix.ps1` em v1.5.3
  e v1.5.6.
- Bateria de maturidade read-only em um restore real de ERP Firebird
  5.0.3 de ~66GB (apenas metadados e contagens agregadas): 3.174 tabelas
  descobertas, 17.866 findings `none_charset` do `firebird_type_audit`,
  291 FKs, 9.842 indices; `firebird_profile_table` em uma tabela com
  span de PK de ~20M recomendou `partitions=10` com os dois alertas de
  particionamento, e o superdimensionamento documentado de PK esparsa
  foi reproduzido em escala (estimativa por faixa 20.107.392 vs
  `COUNT(*)` exato 967.387); `firebird_pool_stats` permaneceu coerente
  (idle estacionado, ativas 0, `last_error` NULL) durante todo o uso.

## Notas de compatibilidade

- Consumidores de `firebird_pool_stats` com `SELECT *` veem duas colunas
  novas; nada foi removido ou renomeado.
- Consumidores de `firebird_profile_table` com `SELECT *` veem duas
  colunas novas e um parametro nomeado novo; nada foi removido ou
  renomeado.
- Tres codigos de alerta estaveis novos: `filter_before_scan`,
  `materialize_before_scan`, `exact_count_executed`.
