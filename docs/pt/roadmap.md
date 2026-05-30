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

Detectar e explicar:

- view sem primary key
- view com joins
- view com agregacao
- view sem filtro seletivo
- casos em que materializar por DuckDB/dbt/Parquet e o fluxo mais seguro

O caminho recomendado deve ser explicito: views Firebird pesadas muitas
vezes devem ser materializadas por pipelines DuckDB, dbt ou Parquet, nao
por um wrapper especifico da extensao.

### Pushdown report / explicabilidade

Expor o que o conector enviou ao Firebird e o que o DuckDB ainda precisou
aplicar localmente. Deve aproveitar a telemetria de query remota existente
em vez de criar uma superficie diagnostica desconectada.

O relatorio deve tornar visivel:

- colunas projetadas
- filtros empurrados
- paginacao / limites explicitos empurrados
- scan serial vs. paralelo
- numero de particoes
- predicados ou operadores nao empurrados
- motivo conhecido para manter operador local

A superficie de usuario sera decidida durante o design: funcao, view de
observabilidade ou campos estruturados na telemetria existente. A primeira
versao deve ser factual, compacta e testavel. Nao deve virar otimizador de
custo, advisor ou substituto do planner.

### `firebird_pool_stats()`

Expor estado do pool depois do analyzer, para que os numeros tenham
contexto:

- tamanho maximo configurado
- conexoes abertas
- conexoes ociosas
- conexoes em uso
- contador de reuso
- contador de descarte
- ultimo erro, se for seguro rastrear

Fecha a divida de observabilidade da Fase 2 sem transformar metricas de
pool na feature principal da v0.6.

### DECFLOAT fallback

Resolver a lacuna restante de Firebird 4+ com fallback conservador. O
default preferido continua lossless text, a menos que testes provem um
mapeamento melhor que preserve valor e expectativa do usuario. Caminhos
numericos rapidos devem ser opt-in quando houver risco de perda de precisao.

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
