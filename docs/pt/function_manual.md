# Manual de funcoes da extensao

Este manual lista as superficies publicas da extensao `duckdb-firebird`.
Cada funcao nova, opcao nova ou mudanca de comportamento em funcao existente
deve atualizar este arquivo, ou criar/atualizar um manual especifico linkado
aqui. Esta e uma premissa do projeto: repositorio publico precisa ter how-to
facil de encontrar.

## Como ler este manual

- **Nome**: nome SQL da funcao, comando ou opcao.
- **O que faz e como funciona**: comportamento, partes usadas do Firebird e do
  DuckDB, e melhores praticas aplicadas.
- **Para que serve**: casos de uso.
- **Uso no dia a dia**: exemplos praticos.

## Nivel 1 - Leitura direta

### `firebird_scan(connection_string, table_name)`

#### O que faz e como funciona

Le uma tabela Firebird diretamente dentro do DuckDB.

Assinatura basica:

```sql
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'CLIENTES');
```

Parametros posicionais:

- `connection_string`: caminho local `.fdb` ou URI `firebird://...`.
- `table_name`: tabela ou view Firebird.

Parametros nomeados:

- `user`: usuario Firebird.
- `password`: senha Firebird.
- `charset`: charset enviado ao `libfbclient`; padrao recomendado `UTF8`.
- `role`: role Firebird.
- `dialect`: dialect Firebird.
- `partitions`: paralelismo por faixa de PK. `0` = auto, `1` = serial.
- `row_limit`: limita linhas usando `ROWS` no Firebird.
- `row_offset`: offset global; exige `row_limit`.
- `none_encoding`: estrategia para colunas `CHARACTER SET NONE`.

Internamente, a extensao usa `libfbclient` para abrir cursor no Firebird e
entrega os chunks ao DuckDB como table function. O DuckDB continua responsavel
por joins, agregacoes analiticas, materializacao em tabelas locais, Parquet,
S3/MinIO e composicao com outras fontes.

Melhores praticas aplicadas:

- Projection pushdown: so as colunas necessarias entram no `SELECT` remoto.
- Predicate pushdown conservador: filtros seguros viram `WHERE` no Firebird.
- Prepared statements: filtros com valores usam binds em vez de interpolar
  literais.
- Residual filters: quando ha duvida de equivalencia semantica, DuckDB
  revalida localmente.
- Paginacao Firebird: `row_limit` e `row_offset` viram `ROWS`.
- Paralelismo opt-in por PK: usado apenas quando a tabela tem chave apropriada.
- Protecao para `CHARACTER SET NONE`: texto bruto nao-UTF8 nao recebe pushdown
  textual quando isso pode alterar resultado.

#### Para que serve

- Consultar Firebird legado sem exportacao previa.
- Cruzar Firebird com CSV, Parquet, tabelas DuckDB locais ou dados em S3.
- Criar snapshots analiticos sem tirar o Firebird do papel OLTP.
- Investigar dados antes de modelar ETL/dbt.

#### Uso no dia a dia

Consultar uma tabela:

```sql
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'CLIENTES');
```

Consultar so colunas e linhas relevantes:

```sql
SELECT IDCLIENTE, NOME, CIDADE
FROM firebird_scan('C:/dados/empresa.fdb', 'CLIENTES')
WHERE CIDADE = 'SAO PAULO';
```

Materializar resultado em DuckDB:

```sql
CREATE OR REPLACE TABLE clientes_sp AS
SELECT IDCLIENTE, NOME, CIDADE
FROM firebird_scan('C:/dados/empresa.fdb', 'CLIENTES')
WHERE CIDADE = 'SAO PAULO';
```

Exportar para Parquet:

```sql
COPY (
  SELECT *
  FROM firebird_scan('C:/dados/empresa.fdb', 'TABENTRADASAIDA')
  WHERE DATAMOVIMENTO >= DATE '2026-01-01'
) TO 'lake/erp/tabentradasaida_2026.parquet'
(FORMAT PARQUET);
```

Usar paginacao para amostra:

```sql
SELECT *
FROM firebird_scan(
  'C:/dados/empresa.fdb',
  'TABENTRADASAIDA',
  row_limit = 100
);
```

## Nivel 2 - Descoberta de catalogo

### `firebird_tables(connection_string)`

#### O que faz e como funciona

Lista tabelas e views visiveis no Firebird.

Assinatura:

```sql
SELECT *
FROM firebird_tables('C:/dados/empresa.fdb');
```

Parametros nomeados:

- `user`
- `password`
- `charset`
- `role`
- `dialect`

Saida:

- `table_name`
- `kind`
- `column_count`
- `has_pk`
- `pk_column`

Internamente, consulta metadados do Firebird via tabelas de sistema e entrega
um resultado tabular para o DuckDB. A funcao nao le dados das tabelas de
negocio; ela le catalogo.

#### Para que serve

- Descobrir rapidamente quais relacoes existem.
- Identificar tabelas com PK para `partitions`.
- Separar tabelas e views antes de montar consultas.
- Preparar inventario para BI, dbt ou documentacao.

#### Uso no dia a dia

Listar tabelas com chave primaria:

```sql
SELECT table_name, pk_column
FROM firebird_tables('C:/dados/empresa.fdb')
WHERE has_pk
ORDER BY table_name;
```

Encontrar views:

```sql
SELECT table_name
FROM firebird_tables('C:/dados/empresa.fdb')
WHERE kind = 'VIEW'
ORDER BY table_name;
```

Priorizar tabelas grandes para materializacao:

```sql
SELECT table_name, column_count, has_pk
FROM firebird_tables('C:/dados/empresa.fdb')
ORDER BY column_count DESC;
```

### `information_schema` via `ATTACH`

#### O que faz e como funciona

Depois de anexar um banco Firebird com `ATTACH`, o DuckDB passa a expor
metadados do Firebird em `information_schema`.

Exemplo:

```sql
ATTACH 'C:/dados/empresa.fdb' AS fb
(TYPE firebird, user 'SYSDBA', password 'masterkey');

SELECT table_schema, table_name
FROM information_schema.tables
WHERE table_catalog = 'fb';
```

Internamente, a extensao implementa catalogo remoto para o DuckDB. O Firebird
continua sendo a fonte dos metadados; o DuckDB apresenta esses metadados no
formato familiar `information_schema`.

#### Para que serve

- Explorar colunas, tabelas e tipos sem escrever queries contra tabelas de
  sistema Firebird.
- Alimentar geradores de documentacao, dbt sources ou checks de qualidade.
- Ajudar analistas que ja conhecem SQL padrao.

#### Uso no dia a dia

Listar colunas:

```sql
SELECT table_name, column_name, data_type
FROM information_schema.columns
WHERE table_catalog = 'fb'
ORDER BY table_name, ordinal_position;
```

Checar se uma coluna existe:

```sql
SELECT table_name, column_name
FROM information_schema.columns
WHERE table_catalog = 'fb'
  AND column_name = 'DATAMOVIMENTO';
```

## Nivel 3 - Banco anexado

### `ATTACH ... (TYPE firebird)`

#### O que faz e como funciona

Anexa um banco Firebird como catalogo DuckDB. Depois disso, tabelas Firebird
podem ser consultadas como `alias.schema.tabela`.

Exemplo:

```sql
ATTACH 'C:/dados/empresa.fdb' AS fb
(TYPE firebird, user 'SYSDBA', password 'masterkey', charset 'UTF8');

SELECT *
FROM fb.main.CLIENTES
WHERE IDCLIENTE = 10;
```

Internamente, o storage extension do DuckDB resolve scans de tabelas remotas e
reusa a mesma infraestrutura do `firebird_scan`: schema remoto, pushdown,
prepared statements, particionamento e conversao de tipos.

Melhores praticas aplicadas:

- Connection string fica centralizada no `ATTACH`, nao repetida em cada query.
- Consultas ficam mais legiveis para BI e notebooks.
- `information_schema` passa a funcionar para o catalogo anexado.
- Pushdown segue as mesmas regras conservadoras do scanner.

#### Para que serve

- Sessao analitica com varias queries no mesmo banco Firebird.
- Explorar dados como se fossem tabelas DuckDB.
- Facilitar uso por ferramentas que esperam catalogo e schema.
- Reduzir repeticao de connection string.

#### Uso no dia a dia

Anexar uma vez e consultar varias tabelas:

```sql
ATTACH 'firebird://SYSDBA:masterkey@localhost:3050/C:/dados/empresa.fdb'
AS fb (TYPE firebird);

SELECT COUNT(*) FROM fb.main.CLIENTES;
SELECT COUNT(*) FROM fb.main.PEDIDOS;
```

Combinar Firebird com Parquet:

```sql
SELECT c.IDCLIENTE, c.NOME, p.total_2026
FROM fb.main.CLIENTES c
JOIN read_parquet('lake/pedidos_2026.parquet') p
  ON p.IDCLIENTE = c.IDCLIENTE;
```

### `firebird_attach_sql(connection_string [, schema])`

#### O que faz e como funciona

Gera SQL `CREATE VIEW` para expor tabelas Firebird via views DuckDB que chamam
`firebird_scan`. E uma funcao auxiliar para ambientes onde o usuario prefere
views locais em vez de `ATTACH`.

Assinatura:

```sql
SELECT sql
FROM firebird_attach_sql('C:/dados/empresa.fdb', 'fb');
```

Parametros nomeados:

- `user`
- `password`
- `charset`
- `role`
- `dialect`
- `schema`
- `overwrite`

Saida:

- `sql`: comando `CREATE VIEW ... AS SELECT * FROM firebird_scan(...)`.

#### Para que serve

- Gerar camada de views DuckDB para bancos Firebird.
- Revisar SQL antes de criar objetos.
- Automatizar onboarding em scripts.

#### Uso no dia a dia

Gerar comandos:

```sql
SELECT sql
FROM firebird_attach_sql('C:/dados/empresa.fdb', schema = 'fb');
```

Executar os comandos gerados em script de inicializacao:

```sql
CREATE SCHEMA IF NOT EXISTS fb;
-- cole/revise o SQL retornado por firebird_attach_sql()
```

## Nivel 4 - Observabilidade

### `firebird_last_query()`

#### O que faz e como funciona

Mostra a ultima query Firebird tentada pela conexao DuckDB atual. Retorna zero
linhas se nenhum scan Firebird foi executado naquele `ClientContext`.

Exemplo:

```sql
SELECT *
FROM firebird_last_query();
```

Saida compartilhada com `firebird_query_log()`:

- `remote_sql`: SQL remoto enviado/tentado no Firebird.
- `binds`: binds redatados.
- `table_name`: nome da tabela, nunca connection string.
- `projected_columns`: colunas projetadas.
- `pushed_filters`: filtros empurrados ao Firebird.
- `residual_filters`: filtros mantidos para o DuckDB revalidar.
- `rows_read`: linhas lidas.
- `firebird_time_us`: tempo de chamadas Firebird em microssegundos.
- `total_time_us`: tempo total desde a captura.
- `connection_id`: reservado; `-1` enquanto o pool nao expuser ID barato.
- `connection_reused`: reservado; `false` enquanto o pool nao expuser dado
  barato.
- `parallel_scan`: `true` quando `partitions > 1`.
- `partitions`: numero de particoes.
- `captured_at`: timestamp de captura.
- `error_message`: erro sanitizado, vazio quando a query terminou limpa.

Seguranca:

- Texto e blob em binds viram `<text:redacted>`.
- `NULL` vira `<null>`.
- `error_message` remove padroes obvios como `password=...` e
  `scheme://user:pass@host`.
- Numeric/temporal ainda aparecem raw; ha divida registrada para
  `firebird_observability_redaction = strict|debug`.

A semantica e "ultima query tentada": a captura ocorre antes/de volta das
chamadas de cursor. Se o Firebird falhar, a funcao ainda ajuda a ver qual SQL
foi tentada.

#### Para que serve

- Entender qual SQL foi enviada ao Firebird.
- Confirmar projection pushdown e predicate pushdown.
- Diagnosticar queries lentas ou full scans acidentais.
- Provar que senha e literais de texto nao aparecem no diagnostico.

#### Uso no dia a dia

Ver o SQL remoto depois de uma consulta:

```sql
SELECT EMP_ID, EMP_NAME
FROM firebird_scan('C:/dados/empresa.fdb', 'EMPLOYEE')
WHERE EMP_ID > 2;

SELECT remote_sql, binds, pushed_filters, residual_filters
FROM firebird_last_query();
```

Checar tempos e linhas:

```sql
SELECT rows_read, firebird_time_us, total_time_us
FROM firebird_last_query();
```

Checar se houve erro sanitizado:

```sql
SELECT error_message
FROM firebird_last_query()
WHERE error_message <> '';
```

### `firebird_query_log()`

#### O que faz e como funciona

Mostra um historico curto das queries Firebird tentadas na conexao DuckDB
atual. O log e desligado por padrao.

Ativar:

```sql
SET firebird_query_log_size = 16;
```

Consultar:

```sql
SELECT *
FROM firebird_query_log();
```

Desligar e limpar no proximo scan:

```sql
SET firebird_query_log_size = 0;
```

Internamente, usa um ring buffer `std::deque` por `ClientContext`. A funcao
retorna as linhas em ordem most-recent-first. Quando o tamanho configurado e
atingido, entradas antigas saem. Tamanho `0` limpa/desliga para reduzir
overhead e evitar retencao acidental de dados diagnosticos.

#### Para que serve

- Debugar uma sequencia de queries curtas.
- Comparar pushdown entre consultas parecidas.
- Validar que uma ferramenta BI esta mandando os filtros esperados.
- Investigar regressao sem ativar log global.

#### Uso no dia a dia

Ativar log para uma sessao de debug:

```sql
SET firebird_query_log_size = 5;

SELECT COUNT(*) FROM fb.main.CLIENTES WHERE CIDADE = 'SAO PAULO';
SELECT COUNT(*) FROM fb.main.CLIENTES WHERE CIDADE = 'RIO DE JANEIRO';

SELECT captured_at, table_name, remote_sql, binds, rows_read
FROM firebird_query_log();
```

Ver as queries mais lentas da janela:

```sql
SELECT table_name, rows_read, firebird_time_us, total_time_us, remote_sql
FROM firebird_query_log()
ORDER BY total_time_us DESC;
```

Confirmar que texto foi redatado:

```sql
SELECT binds
FROM firebird_query_log()
WHERE array_to_string(binds, ',') LIKE '%<text:redacted>%';
```

## Nivel 5 - Opcoes de sessao

### `SET firebird_query_log_size = N`

#### O que faz e como funciona

Configura o tamanho do ring buffer usado por `firebird_query_log()` na sessao
DuckDB atual.

Padrao:

```sql
SET firebird_query_log_size = 0;
```

`0` significa desligado. Valores positivos ativam o log ate `N` entradas.

#### Para que serve

- Controlar custo e retencao de diagnostico.
- Ativar observabilidade so durante investigacao.
- Evitar que sessoes normais guardem historico sem necessidade.

#### Uso no dia a dia

Ativar durante debug:

```sql
SET firebird_query_log_size = 10;
```

Limpar/desligar:

```sql
SET firebird_query_log_size = 0;
```

## Premissa de documentacao

Toda mudanca publica precisa manter o manual atualizado:

- funcao nova;
- parametro novo;
- coluna nova em funcao de diagnostico;
- mudanca de semantica;
- opcao `SET` nova;
- mudanca relevante de seguranca, redacao ou pushdown.

Se o detalhe for grande demais para este arquivo, crie um manual especifico em
`docs/pt/` e adicione link nesta pagina.

