# duckdb-firebird - Roadmap

Plano vivo. A versao em ingles (`docs/en/roadmap.md`) conserva o historico
detalhado completo. Este documento e a contraparte PT obrigatoria para a
estrategia atual, decisoes de roadmap e proximos passos de PM/DEV.

## Principio de produto

Quando houver duvida entre adicionar uma nova funcionalidade ou fortalecer
o nucleo existente, fortalecer o nucleo existente.

Performance, estabilidade, compatibilidade e observabilidade tem prioridade
sobre novas funcionalidades. O valor principal da extensao esta em conectar
Firebird ao DuckDB da melhor forma possivel, nao em competir com ferramentas
do ecossistema analitico.

## Foco da extensao

A extensao conecta bancos Firebird ao DuckDB para analytics modernos.

Ela e:

- conector Firebird para DuckDB
- scanner analitico
- camada de integracao
- ferramenta de acesso a dados
- ponte entre OLTP e analytics

Ela nao e:

- ferramenta ETL
- plataforma CDC
- orquestrador
- catalogo de dados
- ferramenta de governanca
- lakehouse
- banco de dados
- framework de replicacao
- substituto do DuckDB
- substituto do dbt
- substituto do Airflow

## Estado atual consolidado

Ja esta pronto no roadmap publico:

- Firebird 3/4/5 testado em matriz Linux.
- `INT128`, `NUMERIC(38)`, `TIMESTAMP_TZ` e `TIME_TZ` verificados.
- `row_offset` + paginacao fisica.
- bind variables em prepared statements.
- LIKE/predicate pushdown adicional.
- `NOT IN` e `NOT bool` pushdown.
- testes de metadata/discovery.
- observabilidade de query remota.
- pool de conexoes configuravel.
- geracao de dbt sources.
- loader runtime de `libfbclient`.

Ainda aberto:

- DECFLOAT fallback.
- `firebird_pool_stats()`.
- diagnostics nativos Firebird.
- pushdown conservador de agregacoes.
- recomendacoes de paralelismo.

## Milestone v0.6 - Firebird Native Diagnostics

Decisao estrategica: a proxima fase **nao** e
`firebird_materialize()`.

Materializacao sai do caminho critico e vai para **v1.x** como helper
opcional de DX, caso usuarios reais ainda precisem de um wrapper fino
em volta do caminho nativo do DuckDB. O DuckDB ja resolve bem
materializacao, tabelas locais, exportacao Parquet e handoff lakehouse.

**Materialization is DuckDB's strength. Firebird-native diagnostics is
this extension's differentiator.**

Ordem de entrega para v0.6:

1. `firebird_profile_table()`
2. Diagnostico de views pesadas
3. Pushdown report / explicabilidade
4. `firebird_pool_stats()`
5. DECFLOAT fallback
6. Pushdown conservador de agregacoes
7. Recomendacoes de adaptive parallel scan

### Analyzer `firebird_profile_table()`

Status: primeira versao factual implementada em branch de desenvolvimento
e testada localmente; ainda nao publicada. Ver `docs/pt/function_manual.md`
para o comportamento entregue e as limitacoes atuais.

Formato:

```sql
SELECT *
FROM firebird_profile_table('fb.main.TABELA');
```

O analyzer retorna (entregue na primeira versao salvo nota em contrario):

- tipo do objeto: `TABLE` / `VIEW` (entregue)
- se existe primary key (entregue)
- colunas da primary key (entregue)
- indices existentes (entregue)
- colunas candidatas a watermark (entregue, por tipo)
- boas colunas para filtro (entregue, indexadas + tipo barato)
- estimativa de linhas, quando barata e segura (adiado)
- risco de full scan (entregue)
- recomendacao de `partitions=N` (entregue, recomendacao, da faixa da PK)
- recomendacoes de filtros obrigatorios (adiado; surge como warnings)

A aceitacao e qualidade diagnostica, nao velocidade de execucao. A funcao
deve ajudar o usuario a decidir se deve consultar ao vivo, filtrar melhor,
particionar ou materializar via DuckDB/dbt/Parquet.

### Diagnostico de views pesadas

Status: primeira versao factual implementada em branch de desenvolvimento
e testada localmente (incorporada ao `firebird_profile_table()` via coluna
`warnings`); ainda nao publicada. Ver `docs/pt/function_manual.md`.

Detectar e explicar (entregue salvo nota em contrario):

- view sem primary key (entregue)
- view com joins (entregue, deteccao de token `JOIN`)
- view com agregacao (entregue, `GROUP BY` / funcoes de agregacao)
- view sem filtro seletivo (entregue, ausencia de `WHERE` na definicao)
- casos em que materializar por DuckDB/dbt/Parquet e o fluxo mais seguro
  (entregue, exposto como warnings)

O caminho recomendado deve ser explicito: views Firebird pesadas muitas
vezes devem ser materializadas por pipelines DuckDB, dbt ou Parquet, nao
por um wrapper especifico da extensao.

Entregue como inspecao conservadora de tokens em `RDB$VIEW_SOURCE`, nao um
parser SQL nem analise de plano. O texto da view nunca e retornado;
join por virgula nao e detectado; definicoes ilegiveis emitem
`view definition not inspected`. Sem nova funcao ou mudanca de schema - a
forma de 10 colunas do `firebird_profile_table()` e sua coluna `warnings`
carregam o diagnostico.

### Pushdown report / explicabilidade

Status: primeira versao factual implementada em branch de desenvolvimento
e testada localmente; ainda nao publicada. Entregue EXPANDINDO a telemetria
existente (`firebird_last_query()` / `firebird_query_log()`), sem funcao
nova - o schema cresceu de 15 para 18 colunas. Ver
`docs/pt/function_manual.md`.

Expor o que o conector enviou ao Firebird e o que o DuckDB ainda precisou
aplicar localmente. Construido sobre a telemetria de query remota existente
em vez de uma superficie diagnostica desconectada.

O relatorio torna visivel (entregue salvo nota em contrario):

- colunas projetadas (ja na telemetria)
- filtros empurrados (ja na telemetria)
- paginacao / limites explicitos empurrados (entregue - `limit_pushed` /
  `offset_pushed`, `NULL` quando nenhum)
- scan serial vs. paralelo (ja na telemetria)
- numero de particoes (ja na telemetria)
- predicados ou operadores nao empurrados (ja na telemetria -
  `residual_filters`)
- motivo conhecido para manter operador local (entregue -
  `not_pushed_reasons`, coarse: `NONE_CHARSET` / `UNSUPPORTED_OP` /
  `ROWID_OR_INVALID_COLUMN` / `UNSUPPORTED_PROJECTION_MAPPING`)

Entregue como campos estruturados na telemetria existente, factual,
compacto e testavel - nao otimizador de custo, advisor ou substituto do
planner. As razoes sao tags coarse capturadas nos pontos de decisao de
pushdown ja existentes; `TranslateFilter` nao foi refatorado amplamente.

### `firebird_pool_stats()`

Status: primeira versao factual implementada em branch de desenvolvimento
e testada localmente; ainda nao publicada. Ver `docs/pt/function_manual.md`.

Forma: `firebird_pool_stats('fb')` - alias explicito do ATTACH, uma linha
por chamada. NAO enumera catalogos (sem forma sem-argumento), le apenas
contadores que o pool ja rastreia, e nunca faz lease de conexao.

Expor estado do pool para dar contexto (entregue salvo nota em contrario):

- tamanho maximo idle configurado (entregue - `max_idle_size`)
- timeout idle configurado (entregue - `idle_timeout_ms`)
- flag de pool habilitado (entregue - `pool_enabled`)
- conexoes ociosas (entregue - `idle_connections`)
- criadas / reusadas / descartadas ao longo da vida (entregue -
  `total_created` / `total_reused` / `total_discarded`)
- conexoes em uso / ativas (adiado - exige contador de lease novo no pool;
  reportado antes de implementar conforme escopo)
- ultimo erro, se for seguro rastrear (adiado - sem historico de erro de
  pool ainda)

Fecha a maior parte da divida de observabilidade da Fase 2 sem transformar
metricas de pool na feature principal da v0.6, e sem instrumentacao nova de
pool: a primeira versao expoe so o que o pool ja conta.

### DECFLOAT fallback

Status: primeira versao implementada em branch de desenvolvimento e testada
localmente; ainda nao publicada. Ver `docs/pt/function_manual.md`.

Resolver a lacuna restante de Firebird 4+ com fallback conservador.
Entregue como lossless text: `DECFLOAT(16)` / `DECFLOAT(34)` sao projetados
server-side como `CAST(col AS VARCHAR(64))` e expostos como VARCHAR,
encerrando o comportamento anterior de NULL silencioso (a coluna era tipada
DOUBLE mas sempre retornava NULL). Sem decoder local Decimal64/Decimal128,
sem default DOUBLE. Um caminho numerico lossy (opt-in) continua sendo
trabalho futuro possivel quando a perda de precisao for aceitavel.

Divida: a fixture de teste DECFLOAT e dedicada
(`scripts/fixture_decfloat.sql` via `FIREBIRD_DECFLOAT_DB`) e pula no CI.
Promove-la para a fixture principal `setup_test_firebird.sh` - com a
atualizacao coordenada dos testes de `metadata` / `dbt-sources` que uma
nova relacao forca - e um passo futuro deliberado.

### Pushdown conservador de agregacoes

Pushdown de agregacoes vem depois do analyzer e comeca pequeno.

Permitido inicialmente:

- `COUNT(*)`
- `MIN` / `MAX`
- `SUM` controlado
- `GROUP BY` simples

Fora de escopo:

- joins
- `HAVING` complexo
- expressoes calculadas
- `BLOB` / `TEXT`
- charset `NONE` problematico
- views pesadas
- overflow incerto

O analyzer deve proteger esse trabalho: empurrar agregacao apenas quando
o objeto Firebird for simples o suficiente e a semantica do resultado for
previsivel.

### Recomendacoes de adaptive parallel scan

Nao comecar com tuning totalmente automatico. Paralelismo automatico pode
surpreender sistemas Firebird de producao, especialmente quando o servidor
ja tem paralelismo configurado.

A primeira versao deve ser apenas diagnostico/recomendacao:

- usar fatos de catalogo e estimativas baratas quando disponiveis
- recomendar `partitions=N`
- alertar quando scan paralelo for arriscado
- considerar primary keys, indices, views pesadas e paginacao explicita
- documentar interacao com `ParallelWorkers` do Firebird 5

So considerar adaptacao automatica depois que o caminho de recomendacao
for observavel, benchmarkado e previsivel em Firebird 3/4/5.

### Helper de materializacao v1.x

`firebird_materialize()` fica deferido para v1.x, opcional e apenas DX.
Nao deve substituir os padroes nativos recomendados do DuckDB:

```sql
CREATE OR REPLACE TABLE local_table AS
SELECT * FROM fb.main.REMOTE_TABLE;

COPY local_table TO 'lake/path'
  (FORMAT parquet, COMPRESSION zstd);
```

Se for adicionado depois, deve ser um wrapper fino sobre esses padroes,
nao uma nova estrategia de storage.

## Regras para DEV

Nao iniciar implementacao sem autorizacao explicita do HUMANO/PM.

Antes de implementar qualquer item v0.6:

- confirmar escopo com CODEX/PM
- revisar `docs/en/roadmap.md` e este arquivo
- manter documentacao PT/EN alinhada
- nao fazer push/tag/release
- nao tocar `duckdb/community-extensions#1980`
