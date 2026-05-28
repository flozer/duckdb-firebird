# duckdb-firebird - Guia de uso para analistas

Este guia mostra como usar a extensao `firebird` para consultar bases
Firebird a partir do [DuckDB](https://github.com/duckdb/duckdb), criar uma camada local de analytics em
arquivo `.duckdb`, materializar dados em tabelas rapidas e exportar
resultados para Parquet/CSV.

O publico-alvo e o analista de dados que conhece SQL, mas ainda nao usa
DuckDB no dia a dia. A ideia principal e simples:

1. O Firebird continua sendo o sistema transacional.
2. O DuckDB consulta o Firebird quando voce precisa de dados vivos.
3. Para analises pesadas e repetidas, voce cria tabelas locais no DuckDB.
4. Dashboards, notebooks e queries exploratorias passam a bater no
   arquivo `.duckdb`, que e muito mais rapido para analytics.

## Referencias oficiais DuckDB

Este guia segue os conceitos documentados pelo DuckDB:

- [`CREATE VIEW`](https://duckdb.org/docs/stable/sql/statements/create_view):
  views sao consultas salvas; elas nao sao materializadas fisicamente.
- [`CREATE TABLE`](https://duckdb.org/docs/stable/sql/statements/create_table):
  `CREATE TABLE ... AS SELECT` cria uma tabela fisica a partir de uma
  consulta.
- [`INSERT`](https://duckdb.org/docs/stable/sql/statements/insert):
  `INSERT INTO ... SELECT` adiciona o resultado de uma consulta em uma
  tabela existente.
- [`COPY`](https://duckdb.org/docs/stable/sql/statements/copy):
  importa/exporta dados entre DuckDB e arquivos como CSV, Parquet e JSON.
- [`ATTACH`](https://duckdb.org/docs/current/sql/statements/attach.html):
  anexa outro catalogo ao DuckDB. Esta extensao usa o mesmo modelo para
  expor Firebird como catalogo read-only.
- [`Indexes`](https://duckdb.org/docs/current/sql/indexes.html) e
  [performance de indexes](https://duckdb.org/docs/current/guides/performance/indexing.html):
  DuckDB cria zonemaps automaticamente; indices ART ajudam em filtros
  muito seletivos.
- [`EXPLAIN` / `EXPLAIN ANALYZE`](https://duckdb.org/docs/stable/sql/statements/profiling):
  inspeciona plano e tempo real de uma consulta.
- [DuckDB CLI](https://duckdb.org/docs/current/clients/cli/overview.html)
  e [dot commands](https://duckdb.org/docs/current/clients/cli/dot_commands.html):
  comandos como `.open`, `.read`, `.tables`, `.schema`, `.timer`.

## 1. Conceitos antes de comecar

### DuckDB em memoria vs arquivo `.duckdb`

Quando voce abre apenas `duckdb`, sem caminho de arquivo, pode trabalhar
em memoria. Isso e bom para testes rapidos, mas os objetos somem ao
fechar a sessao.

Para criar um pequeno data mart local, abra um arquivo persistente:

```bash
duckdb analytics.duckdb
```

ou, no Windows:

```powershell
duckdb.exe analytics.duckdb
```

Tudo que voce criar sem qualificar outro catalogo sera salvo nesse
arquivo: tabelas, views, schemas, indices e macros.

### View nao e tabela materializada

No DuckDB, `CREATE VIEW` salva a query. A view nao guarda os dados. Cada
vez que voce consulta a view, a query roda novamente.

```sql
CREATE OR REPLACE VIEW vw_clientes_firebird AS
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'CLIENTES');
```

Isso e excelente para uma camada semantica fina, mas nao acelera uma
query pesada se ela continuar lendo o Firebird toda vez.

Para materializar, crie uma tabela:

```sql
CREATE TABLE clientes AS
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'CLIENTES');
```

Essa tabela vive dentro do `analytics.duckdb`. Depois disso, suas
analises leem o DuckDB local, nao o Firebird.

### Firebird via scan vs Firebird via ATTACH

A extensao oferece dois jeitos principais de uso.

Use `firebird_scan(...)` quando quiser consultar uma tabela especifica:

```sql
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABPESSOAS')
LIMIT 10;
```

Use `ATTACH ... (TYPE firebird)` quando quiser navegar a base Firebird
como se fosse um catalogo:

```sql
ATTACH 'C:/dados/empresa.fdb' AS fb
    (TYPE firebird, user 'SYSDBA', password 'masterkey');

SELECT *
FROM fb.main.TABPESSOAS
LIMIT 10;
```

O catalogo Firebird e read-only nesta extensao. Crie tabelas analiticas
no catalogo DuckDB local.

## 2. Instalacao e carregamento

### Status atual

A versao publica atual do projeto e `v0.5.1`. O pedido de publicacao no
catalogo DuckDB Community esta aberto em
[`duckdb/community-extensions#1980`](https://github.com/duckdb/community-extensions/pull/1980)
e aponta para essa tag.

### Quando a extensao estiver publicada no catalogo community

Depois que a extensao estiver disponivel no catalogo community do DuckDB:

```sql
INSTALL firebird FROM community;
LOAD firebird;
```

Use isso em cada ambiente novo. O `INSTALL` baixa a extensao; o `LOAD`
carrega a extensao na sessao atual.

### Enquanto estiver usando build local

Se voce esta usando um arquivo `.duckdb_extension` gerado localmente:

```sql
LOAD 'D:/01_Projetos/duckdb-firebird/build/release/extension/firebird/firebird.duckdb_extension';
```

Em algumas builds locais, voce pode precisar iniciar o DuckDB com
`-unsigned`:

```bash
duckdb analytics.duckdb -unsigned
```

### Validar se carregou

```sql
SELECT *
FROM duckdb_extensions()
WHERE extension_name = 'firebird';
```

Depois teste a listagem de tabelas:

```sql
SELECT *
FROM firebird_tables('C:/dados/empresa.fdb');
```

## 3. Conectar ao Firebird

### Caminho local

```sql
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS')
LIMIT 10;
```

### Servidor remoto

```sql
SELECT *
FROM firebird_scan(
    'firebird://usuario:senha@servidor:3050/C:/dados/empresa.fdb?charset=UTF8',
    'TABPESSOAS'
)
LIMIT 10;
```

### Parametros nomeados

Prefira parametros nomeados quando quiser separar caminho, usuario e
senha:

```sql
SELECT *
FROM firebird_scan(
    'C:/legacy/erp.fdb',
    'TABPESSOAS',
    user='SYSDBA',
    password='masterkey',
    charset='UTF8'
)
LIMIT 10;
```

Nota de seguranca: connection strings e SQL podem ir para historico do
terminal, notebooks e logs. Em producao, use usuario Firebird
somente-leitura e nao commite senhas reais em scripts.

## 4. Charset e bases legadas brasileiras

Muitas bases Firebird antigas foram criadas com `CHARACTER SET NONE`.
Nessas bases, os bytes de texto podem ter sido gravados como Windows-1252
mesmo sem o banco declarar isso formalmente.

A extensao usa `none_encoding='win1252'` por padrao porque esse e o caso
mais comum em ERPs legados brasileiros e ocidentais.

Na maioria dos casos, nao passe nada:

```sql
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS')
LIMIT 10;
```

Se precisar ser explicito:

```sql
SELECT *
FROM firebird_scan(
    'C:/legacy/erp.fdb',
    'TABPESSOAS',
    none_encoding='win1252'
);
```

Alternativas:

```sql
-- Falha se encontrar bytes que nao sejam UTF-8 valido.
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='strict');

-- Latin-1 puro.
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='iso8859_1');

-- Preserva bytes brutos como BLOB.
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='blob');
```

Importante: `charset=` e `none_encoding=` nao sao a mesma coisa.
`charset=` controla o charset do cliente Firebird na conexao.
`none_encoding=` controla como a extensao interpreta bytes de colunas
Firebird declaradas como `CHARACTER SET NONE`.

## 4.1 Paginacao por linhas (v0.5)

Para fatias grandes onde voce nao quer trazer a tabela inteira,
combine `row_limit` e `row_offset`:

```sql
-- Primeira pagina: 1000 linhas.
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA',
                   row_limit=1000);

-- Pagina seguinte: pula 1000, traz proximas 1000.
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA',
                   row_limit=1000, row_offset=1000);
```

Internamente a extensao emite `ROWS M+1 TO M+N` no Firebird (1-based,
inclusivo). O scan vira serial automaticamente quando ha paginacao,
porque a ordem so faz sentido com um unico produtor. Pedir
`partitions > 1` junto com paging explicito e rejeitado no bind.

Caveat importante: isto e paginacao **fisica** (offset/limit), nao
**keyset pagination**. Sem `ORDER BY` server-side, paginas sucessivas
podem repetir ou pular linhas se o Firebird reordenar leituras entre
chamadas. Para resultados estaveis em uma serie de paginas, prefira
materializar uma tabela local e paginar la, ou aplique um
`ORDER BY <chave estavel>` na consulta.

## 4.2 Filtros e busca de texto (v0.5)

A extensao empurra para o Firebird os predicados que ela sabe
traduzir com seguranca, incluindo (v0.5):

- `col NOT IN (...)` — vira uma clausula `NOT IN` server-side.
- `NOT bool_col` / `bool_col = FALSE` — empurrado como `NOT col`.
- `col LIKE 'prefixo%'` com texto estatico — empurrado.
- `BETWEEN a AND b` — DuckDB decompoe em `>= AND <=`, ambos empurrados.

Exemplos:

```sql
-- NOT IN
SELECT COUNT(*)
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE CODIGOEMPRESA NOT IN (99, 100, 101);

-- LIKE com prefixo (apostrofo no literal e seguro):
SELECT IDMASTER, OBSERVACOES
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE OBSERVACOES LIKE 'CONTA PARA%'
LIMIT 5;
```

Detalhe interno (v0.5): para tipos seguros e valores nao-literais
(parametros, datas, strings com acento ou apostrofo), o filtro vai
para o Firebird como **bind variable** via XSQLDA de entrada, em vez
de inline no SQL. Isto evita escape errado e permite que o Firebird
reutilize plano. Voce nao precisa fazer nada — a extensao cuida. Em
colunas `CHARACTER SET NONE` com `none_encoding != 'strict'`, filtros
de texto sao mantidos como residual em DuckDB (a comparacao precisa
do decode da extensao para ser correta).

## 5. Explorar a base Firebird

Comece descobrindo as tabelas:

```sql
SELECT *
FROM firebird_tables('C:/legacy/erp.fdb')
ORDER BY table_name;
```

Veja algumas linhas:

```sql
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS')
LIMIT 20;
```

Conte linhas antes de materializar:

```sql
SELECT count(*) AS linhas
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');
```

Olhe tipos inferidos pelo DuckDB:

```sql
DESCRIBE
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');
```

Quando usar `ATTACH`, navegue com nomes qualificados:

```sql
ATTACH 'C:/legacy/erp.fdb' AS fb
    (TYPE firebird, user 'SYSDBA', password 'masterkey');

SHOW DATABASES;

SELECT *
FROM fb.main.TABPESSOAS
LIMIT 20;
```

## 5.1 Descoberta de catalogo via ATTACH (v0.5)

Quando voce anexa a base como catalogo, as ferramentas padrao do
DuckDB enxergam tudo:

```sql
ATTACH 'C:/legacy/erp.fdb' AS fb
    (TYPE firebird, user 'SYSDBA', password 'masterkey');

-- Listagem de tabelas (e views) do catalogo Firebird:
SHOW TABLES FROM fb;

-- Forma da tabela:
DESCRIBE fb.main.TABPESSOAS;

-- Catalogo padrao information_schema, util para BI / dbt / clients ADBC:
SELECT table_catalog, table_schema, table_name, table_type
  FROM information_schema.tables
 WHERE table_catalog = 'fb'
 ORDER BY table_name;

SELECT column_name, data_type, ordinal_position, is_nullable
  FROM information_schema.columns
 WHERE table_catalog = 'fb' AND table_name = 'TABPESSOAS'
 ORDER BY ordinal_position;
```

Pontos a saber:

- Apenas um schema, `main`, e exposto, independente de `RDB$OWNER_NAME`
  na origem. Consultar `fb.public.X` ou outro schema retorna erro.
- A extensao normaliza identificadores para upper-case interno, igual
  ao Firebird; `fb.main.tabpessoas`, `fb.main.Tabpessoas` e
  `fb.main."TABPESSOAS"` resolvem para a mesma entrada.
- `is_nullable` em `information_schema.columns` reflete a definicao
  Firebird (`RDB$NULL_FLAG` no campo ou no dominio). Colunas
  declaradas `NOT NULL` aparecem como `NO`.
- Views Firebird (`RDB$RELATION_TYPE = 1`) aparecem junto com tabelas
  e podem ser consultadas igual. No `information_schema.tables`, todas
  vem hoje como `BASE TABLE` — esta simplificacao do catalog layer
  esta registrada nos testes para ser revisitada.

## 6. Criar uma camada de views

Views sao boas para padronizar nomes, esconder colunas tecnicas e
documentar regras de negocio. Elas continuam lendo o Firebird ao serem
consultadas.

```sql
CREATE SCHEMA IF NOT EXISTS bronze;

CREATE OR REPLACE VIEW bronze.pessoas AS
SELECT
    CODIGO,
    NOME,
    APELIDO,
    CNPJCPF,
    CIDADE,
    UF
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');
```

Crie views para as tabelas principais:

```sql
CREATE OR REPLACE VIEW bronze.entrada_saida AS
SELECT
    CODIGOEMPRESA,
    CODIGOFILIAL,
    CODIGO,
    CODIGOPESSOA,
    DATAMOVIMENTO,
    VALORTOTAL,
    TIPO
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA');
```

Agora o analista consulta objetos com nomes mais amigaveis:

```sql
SELECT *
FROM bronze.pessoas
WHERE UF = 'SP'
LIMIT 100;
```

Como a view nao e materializada, use filtros e projecoes sempre que
possivel. A extensao tenta empurrar filtros e colunas para o Firebird
quando isso e seguro.

## 7. Materializar snapshots em tabelas DuckDB

Para performance, crie uma tabela local. Esse e o substituto pratico de
"view materializada" no DuckDB.

```sql
CREATE SCHEMA IF NOT EXISTS silver;

CREATE OR REPLACE TABLE silver.pessoas AS
SELECT *
FROM bronze.pessoas;
```

Se sua versao/ambiente nao aceitar `CREATE OR REPLACE TABLE`, use:

```sql
DROP TABLE IF EXISTS silver.pessoas;

CREATE TABLE silver.pessoas AS
SELECT *
FROM bronze.pessoas;
```

Crie uma tabela de fatos local:

```sql
CREATE OR REPLACE TABLE silver.entrada_saida AS
SELECT *
FROM bronze.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2023-01-01';
```

Depois disso:

```sql
SELECT count(*)
FROM silver.entrada_saida;
```

Essa query le dados do arquivo `analytics.duckdb`, nao do Firebird.

## 8. Padrao bronze, silver e gold

Um desenho simples para analises:

- `bronze`: views ou tabelas quase iguais ao Firebird.
- `silver`: tabelas locais limpas, tipadas e filtradas.
- `gold`: tabelas agregadas para dashboard e indicadores.

Exemplo:

```sql
CREATE SCHEMA IF NOT EXISTS gold;

CREATE OR REPLACE TABLE gold.vendas_mes AS
SELECT
    date_trunc('month', DATAMOVIMENTO)::DATE AS mes,
    CODIGOEMPRESA,
    CODIGOFILIAL,
    count(*) AS qtd_movimentos,
    sum(VALORTOTAL) AS valor_total
FROM silver.entrada_saida
WHERE TIPO = 'S'
GROUP BY 1, 2, 3
ORDER BY 1, 2, 3;
```

Agora o dashboard consulta `gold.vendas_mes`, que costuma ser muito menor
e muito mais rapido que a tabela transacional original.

## 9. Atualizacao completa simples

Para bases pequenas ou rotinas noturnas, o refresh completo e o mais
simples e confiavel.

Crie um arquivo `refresh.sql`:

```sql
LOAD firebird;

CREATE SCHEMA IF NOT EXISTS bronze;
CREATE SCHEMA IF NOT EXISTS silver;
CREATE SCHEMA IF NOT EXISTS gold;

CREATE OR REPLACE VIEW bronze.pessoas AS
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');

CREATE OR REPLACE VIEW bronze.entrada_saida AS
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA');

CREATE OR REPLACE TABLE silver.pessoas AS
SELECT *
FROM bronze.pessoas;

CREATE OR REPLACE TABLE silver.entrada_saida AS
SELECT *
FROM bronze.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2023-01-01';

CREATE OR REPLACE TABLE gold.vendas_mes AS
SELECT
    date_trunc('month', DATAMOVIMENTO)::DATE AS mes,
    CODIGOEMPRESA,
    CODIGOFILIAL,
    TIPO,
    count(*) AS qtd_movimentos,
    sum(VALORTOTAL) AS valor_total
FROM silver.entrada_saida
GROUP BY 1, 2, 3, 4;
```

Execute:

```bash
duckdb analytics.duckdb -unsigned -init refresh.sql
```

Ou dentro do CLI:

```sql
.read refresh.sql
```

## 10. Atualizacao incremental por data

Quando a tabela Firebird e grande, evite recarregar tudo. Um padrao
simples e carregar por janela de data.

Primeiro crie a tabela local uma vez:

```sql
CREATE TABLE IF NOT EXISTS silver.entrada_saida (
    CODIGOEMPRESA INTEGER,
    CODIGOFILIAL INTEGER,
    CODIGO INTEGER,
    CODIGOPESSOA INTEGER,
    DATAMOVIMENTO DATE,
    VALORTOTAL DECIMAL(18, 2),
    TIPO VARCHAR
);
```

Depois, em cada refresh, remova e recarregue uma janela recente:

```sql
BEGIN TRANSACTION;

DELETE FROM silver.entrada_saida
WHERE DATAMOVIMENTO >= current_date - INTERVAL 30 DAY;

INSERT INTO silver.entrada_saida
SELECT
    CODIGOEMPRESA,
    CODIGOFILIAL,
    CODIGO,
    CODIGOPESSOA,
    DATAMOVIMENTO,
    VALORTOTAL,
    TIPO
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE DATAMOVIMENTO >= current_date - INTERVAL 30 DAY;

COMMIT;
```

Esse modelo funciona bem quando movimentos antigos nao mudam. Se o ERP
altera registros antigos, use uma janela maior ou faca refresh completo
periodico.

## 11. Ordenacao e indices para acelerar analises

DuckDB cria zonemaps automaticamente para tipos comuns. Eles funcionam
melhor quando os dados estao mais ordenados pelas colunas usadas em
filtros.

Ao materializar uma tabela de fatos, ordene por data e chaves comuns:

```sql
CREATE OR REPLACE TABLE silver.entrada_saida AS
SELECT *
FROM bronze.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2023-01-01'
ORDER BY DATAMOVIMENTO, CODIGOEMPRESA, CODIGOFILIAL;
```

Para filtros muito seletivos por uma coluna, um indice ART pode ajudar:

```sql
CREATE INDEX idx_pessoas_codigo
ON silver.pessoas (CODIGO);

CREATE INDEX idx_entrada_saida_pessoa
ON silver.entrada_saida (CODIGOPESSOA);
```

Nao crie indices por reflexo. Eles aceleram alguns filtros pontuais, mas
tambem consomem memoria e deixam carga/atualizacao mais lenta. Para
analises agregadas grandes, ordenacao e colunas bem escolhidas costumam
dar mais retorno.

## 12. Juntar Firebird com arquivos locais

Um ponto forte do DuckDB e consultar varias fontes no mesmo SQL.

Exemplo: tabela de metas em Parquet + vendas vindas do Firebird:

```sql
CREATE OR REPLACE TABLE silver.metas AS
SELECT *
FROM read_parquet('C:/dados/metas/*.parquet');

CREATE OR REPLACE TABLE gold.realizado_vs_meta AS
SELECT
    v.mes,
    v.CODIGOEMPRESA,
    v.CODIGOFILIAL,
    v.valor_total AS realizado,
    m.meta_valor AS meta,
    v.valor_total / NULLIF(m.meta_valor, 0) AS pct_meta
FROM gold.vendas_mes v
LEFT JOIN silver.metas m
  ON m.mes = v.mes
 AND m.codigoempresa = v.CODIGOEMPRESA
 AND m.codigofilial = v.CODIGOFILIAL;
```

## 13. Exportar resultados

Para entregar um dataset para BI, Python, R ou outro time, use `COPY`.

Parquet e a melhor opcao para analytics:

```sql
COPY gold.vendas_mes
TO 'C:/exports/vendas_mes.parquet'
(FORMAT parquet, COMPRESSION zstd);
```

CSV para usuarios finais:

```sql
COPY (
    SELECT *
    FROM gold.vendas_mes
    WHERE mes >= DATE '2024-01-01'
)
TO 'C:/exports/vendas_mes_2024.csv'
(HEADER, DELIMITER ';');
```

Tambem e possivel criar um arquivo por particao:

```sql
COPY gold.vendas_mes
TO 'C:/exports/vendas_mes_partitioned'
(FORMAT parquet, PARTITION_BY (CODIGOEMPRESA));
```

## 14. Usar pelo Python

Exemplo minimo para notebooks:

```python
import duckdb

con = duckdb.connect("analytics.duckdb")
con.execute("LOAD firebird")

df = con.execute("""
    SELECT *
    FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS')
    LIMIT 1000
""").df()

print(df.head())
```

Depois que voce materializa tabelas no `.duckdb`, os notebooks podem
consultar somente o arquivo local:

```python
import duckdb

con = duckdb.connect("analytics.duckdb", read_only=True)

df = con.execute("""
    SELECT mes, sum(valor_total) AS total
    FROM gold.vendas_mes
    GROUP BY 1
    ORDER BY 1
""").df()
```

## 15. Conferir plano e performance

Use `EXPLAIN` para ver o plano sem executar:

```sql
EXPLAIN
SELECT
    CODIGOEMPRESA,
    sum(VALORTOTAL)
FROM silver.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2024-01-01'
GROUP BY 1;
```

Use `EXPLAIN ANALYZE` para executar e medir:

```sql
EXPLAIN ANALYZE
SELECT
    CODIGOEMPRESA,
    sum(VALORTOTAL)
FROM silver.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2024-01-01'
GROUP BY 1;
```

No CLI, ative timer:

```sql
.timer on
```

Compare a mesma consulta lendo Firebird ao vivo e lendo a tabela
materializada:

```sql
-- Ao vivo: bom para dados recentes e exploracao inicial.
SELECT count(*)
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE DATAMOVIMENTO >= DATE '2024-01-01';

-- Local: melhor para analise repetida.
SELECT count(*)
FROM silver.entrada_saida
WHERE DATAMOVIMENTO >= DATE '2024-01-01';
```

## 16. Boas praticas

- Use `firebird_scan` para exploracao e queries pontuais.
- Use `ATTACH` quando quiser navegar varias tabelas Firebird como
  catalogo.
- Materialize no DuckDB as tabelas usadas repetidamente.
- Crie views para semantica, nao para performance.
- Prefira `CREATE TABLE AS SELECT` para snapshots completos.
- Prefira `INSERT INTO ... SELECT` para cargas incrementais.
- Filtre e selecione colunas antes de materializar tabelas grandes.
- Ordene tabelas de fatos por data e chaves de filtro.
- Crie indices apenas para buscas pontuais muito seletivas.
- Exporte datasets analiticos em Parquet quando possivel.
- Use usuario Firebird somente-leitura para analytics.
- Nao rode cargas pesadas contra o Firebird em horario critico sem
  validar impacto.

## 17. Checklist rapido para um projeto novo

```sql
-- 1. Abrir um arquivo DuckDB persistente:
--    duckdb analytics.duckdb -unsigned

LOAD firebird;

-- 2. Explorar origem.
SELECT *
FROM firebird_tables('C:/legacy/erp.fdb')
ORDER BY table_name;

-- 3. Criar schemas.
CREATE SCHEMA IF NOT EXISTS bronze;
CREATE SCHEMA IF NOT EXISTS silver;
CREATE SCHEMA IF NOT EXISTS gold;

-- 4. Criar view da origem.
CREATE OR REPLACE VIEW bronze.pessoas AS
SELECT *
FROM firebird_scan('C:/legacy/erp.fdb', 'TABPESSOAS');

-- 5. Materializar local.
CREATE OR REPLACE TABLE silver.pessoas AS
SELECT *
FROM bronze.pessoas;

-- 6. Criar agregado de negocio.
CREATE OR REPLACE TABLE gold.pessoas_por_uf AS
SELECT
    UF,
    count(*) AS qtd_pessoas
FROM silver.pessoas
GROUP BY 1
ORDER BY 2 DESC;

-- 7. Exportar.
COPY gold.pessoas_por_uf
TO 'C:/exports/pessoas_por_uf.parquet'
(FORMAT parquet, COMPRESSION zstd);
```

## 18. Troubleshooting

### `Extension "firebird" could not be loaded`

Verifique se a extensao foi instalada ou se o caminho do `LOAD` aponta
para o arquivo correto. Em build local, rode o CLI com `-unsigned`.

### `fbclient.dll` errado no Windows

Garanta que o `fbclient.dll` correto esta no `PATH` ou ao lado do
`duckdb.exe`. Misturar cliente Firebird antigo com banco novo pode causar
erro de tipo desconhecido.

### Acentos quebrados

Se a base usa `CHARACTER SET NONE`, comece sem parametros: o default e
`none_encoding='win1252'`. Se ainda houver problema, teste:

```sql
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='iso8859_1')
LIMIT 20;
```

Se voce precisa investigar bytes brutos:

```sql
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'TABELA',
                   none_encoding='blob')
LIMIT 20;
```

### Consulta ao vivo lenta

Leia menos colunas, aplique filtros seletivos e materialize as tabelas
usadas repetidamente.

```sql
CREATE OR REPLACE TABLE silver.movimentos_2024 AS
SELECT
    CODIGOEMPRESA,
    CODIGOFILIAL,
    CODIGOPESSOA,
    DATAMOVIMENTO,
    VALORTOTAL
FROM firebird_scan('C:/legacy/erp.fdb', 'TABENTRADASAIDA')
WHERE DATAMOVIMENTO >= DATE '2024-01-01';
```

### Preciso compartilhar a analise

Compartilhe o arquivo `.duckdb` quando o consumidor tambem usa DuckDB, ou
exporte tabelas `gold` para Parquet/CSV com `COPY`.
