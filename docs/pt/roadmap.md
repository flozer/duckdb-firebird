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

Em fechamento para v0.6:

- diagnostics nativos Firebird implementados localmente.
- `firebird_pool_stats()` implementado localmente.
- DECFLOAT fallback implementado localmente.
- recomendacoes de paralelismo implementadas localmente.
- pushdown conservador de agregacoes investigado e adiado por limite da API
  DuckDB v1.5.3.
- release gate, changelog/tag e atualizacao community extension ainda
  pendentes.

## Milestone v0.6 - Firebird Native Diagnostics

Decisao estrategica: a proxima fase **nao** e
`firebird_materialize()`.

Materializacao nao faz parte do roadmap core. O DuckDB ja resolve bem
materializacao, tabelas locais, exportacao Parquet e handoff lakehouse.
Se um helper fino de conveniencia voltar a ser considerado, ele sera uma
sugestao de melhoria fora do roadmap core da extensao e precisara de nova
aprovacao PM/HUMANO conforme o ACTION_GUIDE.

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

Alvo de release v0.6: fechar a branch de desenvolvimento, validar o
conjunto completo de diagnosticos com o HUMANO, preparar release
notes/metadados de tag e atualizar o caminho da submissao community
extension para o PR #1980. Nenhuma feature nova deve entrar na v0.6, exceto
correcao de defeito bloqueante no core do conector, diagnosticos,
compatibilidade, observabilidade ou empacotamento.

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

Status: **adiado** (investigado na branch de desenvolvimento v0.6, nao
implementado). Decisao registrada para que o mesmo caminho nao seja
reaberto as cegas.

Achado da investigacao (DuckDB v1.5.3): nao existe hook de
aggregate-pushdown exposto a extensao no `TableFunction`. O caminho limpo
para um atalho de `COUNT(*)` e `get_partition_stats` - o optimizer
(`StatisticsPropagator::TryExecuteAggregates`) ja dobra `COUNT(*)` sem
`GROUP BY` em constante quando o scan retorna um `PartitionStatistics` com
contagem exata. O bloqueio: `GetPartitionStatsInput` expoe somente
`{table_function, bind_data}` e **nao** da ao callback acesso a
`table_filters`. O optimizer aplica o filtro *depois* de chamar o callback.
Entao dentro de `get_partition_stats` nao da para distinguir um `COUNT(*)`
puro de um `COUNT(*) ... WHERE ...`. Implementar assim mesmo rodaria um
`SELECT COUNT(*)` remoto numa consulta filtrada, so para o optimizer dar
bail no filtro e cair em scan normal - um round-trip extra invisivel e
telemetria enganosa numa consulta filtrada. Nao aceitamos esse custo
surpresa em Firebird de producao.

Adiado ate o DuckDB expor os table filters ao callback de partition-stats,
ou aparecer outro hook limpo, ou decidirmos explicitamente aceitar o
tradeoff do round-trip na consulta filtrada. `bind_operator` /
`bind_replace` nao servem aqui: rodam antes do bind regular e nao enxergam
o `LogicalAggregate` acima do scan.

Quando desbloqueado, a primeira versao continua pequena.

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

Status: primeira versao implementada em branch de desenvolvimento e testada
localmente; ainda nao publicada. Ver `docs/pt/function_manual.md`.

Apenas diagnostico/recomendacao - **nao** autotuning. O `firebird_scan`
permanece inalterado: nada e paralelizado automaticamente e nenhum ganho de
performance e prometido. O trabalho refina o `recommended_partitions` e os
`warnings` do `firebird_profile_table()` ja existente (schema de 10 colunas
inalterado):

- recomendar `partitions=N` apenas pela largura da faixa `MIN`/`MAX` da PK
  numerica de coluna unica (sem contagem de linhas, sem `COUNT(*)`, sem full
  scan), com teto 8 (entregue)
- recomendar `partitions=1` mesmo para PK numerica quando a faixa e pequena,
  com nota explicita (entregue)
- recomendar `partitions=1` com warning para views, sem-PK, PK composta e PK
  nao-numerica (entregue)
- emitir caveat de paralelismo server-side quando `partitions > 1` for
  sugerido, apontando para `ParallelWorkers` do Firebird 5 - um caveat
  generico, ja que a config de paralelismo do servidor nao e consultada
  (entregue)

Nao comecar com tuning totalmente automatico. Paralelismo automatico pode
surpreender sistemas Firebird de producao, especialmente quando o servidor
ja tem paralelismo configurado. Adaptacao automatica fica fora de escopo ate
o caminho de recomendacao ser observavel, benchmarkado e previsivel em
Firebird 3/4/5.

Limitacao: o ramo `recommended_partitions > 1` nao e exercido por fixture de
CI - uma tabela de PK numerica com faixa larga forcaria uma nova relacao na
fixture principal e cascataria atualizacoes nos testes de `metadata` /
`dbt-sources`. A heuristica de span e deterministica e coberta por codigo;
os caminhos `= 1` e os warnings sao cobertos por
`firebird_profile_table.test`.

## Fechamento da release v0.6

Intencao atual: publicar a v0.6 para uso real em producao assim que o gate
de release estiver verde. A partir deste ponto ate a tag v0.6, o roadmap
aceita apenas trabalho core de fechamento:

- defeitos bloqueantes de build, portabilidade, empacotamento ou CI;
- regressoes em scan Firebird, ATTACH, mapeamento de tipos, pushdown,
  telemetria, pool ou diagnosticos;
- documentacao necessaria para usuarios executarem a extensao com seguranca;
- metadados da community extension necessarios para atualizar o PR #1980.

Dividas tecnicas conhecidas e nao bloqueantes ficam registradas em vez de
atrasar a v0.6:

- estimativa de linhas e recomendacoes estruturadas de filtros obrigatorios
  no `firebird_profile_table()`;
- conexoes ativas/em uso e `last_error` no `firebird_pool_stats()`;
- promover a fixture DECFLOAT para a fixture principal de CI;
- exercitar `recommended_partitions > 1` no CI;
- pushdown de agregacoes, bloqueado por limite da API DuckDB v1.5.3.

## Checklist de release (rodar antes de qualquer push/tag/release)

Obrigatorio antes da publicacao v0.6:

1. teste humano final do conjunto completo v0.6;
2. build Windows nativo e grupo sqllogictest Firebird verdes;
3. simulacao Docker/Linux gcc do caminho community verde a partir de clone
   recursivo fresh do HEAD commitado;
4. opcional, mas recomendado: smoke Docker carregando a extensao e rodando
   ao menos uma query Firebird sintetica end-to-end;
5. `git status` limpo na branch de release;
6. paridade docs PT/EN revisada para toda mudanca visivel ao usuario;
7. changelog / release notes preparados;
8. versao e tag aprovadas pelo HUMANO;
9. `community-extensions/description.yml` atualizado para a tag/ref
   aprovada;
10. plano explicito para tratar `duckdb/community-extensions#1980`.

Notas do teste de release:

- a extensao nao deve linkar `libfbclient` em build time; deve continuar
  carregando a client library em runtime;
- testes contra `C:\Athenas\restaurado.fdb` podem ler apenas metadata e
  contagens agregadas. Nao salvar, exportar, materializar, logar, commitar
  ou enviar dados reais desse banco;
- artefatos de release, arquivos temporarios de Docker e simulacoes locais
  devem permanecer ignorados salvo quando forem parte intencional do codigo
  publico.

## Sugestoes fora do roadmap core

As ideias abaixo podem voltar como sugestoes de melhoria, mas nao entram no
roadmap enquanto nao provarem que fortalecem o nucleo da extensao:

- `firebird_materialize()` - DuckDB ja resolve materializacao, tabelas
  locais, exportacao Parquet e handoff lakehouse. Um wrapper de conveniencia
  futuro precisa de nova aprovacao PM/HUMANO.
- CDC, ETL, orquestracao e governanca - pertencem a dbt, Airflow, Dagster,
  dlt, Meltano, ferramentas CDC ou plataformas de governanca. A extensao
  permanece focada em acesso Firebird seguro, performatico e observavel a
  partir do DuckDB.

## Regras para DEV

Nao iniciar implementacao sem autorizacao explicita do HUMANO/PM.

Antes de implementar qualquer item v0.6:

- confirmar escopo com CODEX/PM
- revisar `docs/en/roadmap.md` e este arquivo
- manter documentacao PT/EN alinhada
- nao fazer push/tag/release
- nao tocar `duckdb/community-extensions#1980`
