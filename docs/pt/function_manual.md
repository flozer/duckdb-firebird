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

## Requisito de runtime

A partir da v0.5.5 a extensao nao embute o cliente Firebird no binario
publicado e o build nao linka `libfbclient`. Para qualquer funcao
`firebird_*` funcionar e necessario ter, na maquina que executa as
consultas, uma biblioteca cliente Firebird acessivel pelo loader
dinamico do sistema:

- Linux: `libfbclient.so.2` ou `libfbclient.so` (pacotes
  `libfbclient2` / `firebird*-server`).
- macOS: `libfbclient.dylib` (de uma instalacao Firebird).
- Windows: `fbclient.dll` ou `fbclient_ms.dll` (de uma instalacao
  Firebird ou do pacote Firebird ODBC client).

Quando a biblioteca esta em caminho nao-padrao, defina
`DUCKDB_FIREBIRD_CLIENT_LIBRARY` com o caminho absoluto antes da
primeira chamada `firebird_*`. O override e autoritativo: se o caminho
nao puder ser carregado, a extensao levanta um erro claro em vez de
cair em fallback silencioso.

Mensagem de erro quando nenhum cliente esta acessivel:

```text
IO Error: Firebird client library not found. Install the Firebird
client (libfbclient on Linux/macOS, fbclient.dll on Windows) or set
DUCKDB_FIREBIRD_CLIENT_LIBRARY=/path/to/library. Tried: ...
```

## Nivel 1 - Leitura direta

### `firebird_scan(connection_string, table_name)`

#### O que faz e como funciona

Le uma tabela Firebird diretamente dentro do DuckDB.

Assinatura basica:

```sql
SELECT *
FROM firebird_scan('C:/dados/empresa.fdb', 'CLIENTES');
```

Conexao remota:

```sql
SELECT *
FROM firebird_scan(
  'firebird://APP_READONLY:secret@db.example.com:3050/path/to/database.fdb?charset=UTF8',
  'CUSTOMER'
);
```

Formato equivalente do libfbclient:

```sql
SELECT *
FROM firebird_scan(
  'database=db.example.com/3050:/path/to/database.fdb;user=APP_READONLY;password=secret;charset=UTF8',
  'CUSTOMER'
);
```

Para conexoes remotas, use `firebird://USUARIO:SENHA@HOST:PORTA/caminho`
ou `database=HOST/PORTA:/caminho;user=USUARIO;password=SENHA`. Nao use
`HOST:PORTA://caminho`.

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
(TYPE firebird, user 'APP_READONLY', password 'secret');

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
(TYPE firebird, user 'APP_READONLY', password 'secret', charset 'UTF8');

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
ATTACH 'firebird://APP_READONLY:secret@db.example.com:3050/path/to/database.fdb'
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

### `firebird_generate_dbt_sources(catalog_name)`

#### O que faz e como funciona

Gera um arquivo `sources.yml` para dbt a partir de um catalogo
Firebird ja anexado via `ATTACH ... (TYPE firebird)`. Retorna uma
unica linha com a coluna `yaml VARCHAR` contendo todo o documento.

```sql
SELECT yaml FROM firebird_generate_dbt_sources('fb');
```

Para escrever direto em disco:

```sql
COPY (SELECT yaml FROM firebird_generate_dbt_sources('fb'))
  TO 'sources.yml' (FORMAT csv, HEADER false, QUOTE '');
```

Como funciona internamente:

- Valida que `catalog_name` existe no DuckDB e que e um catalogo
  Firebird (`GetCatalogType() == "firebird"`). Caso contrario,
  levanta `BinderException` com mensagem acionavel.
- Adquire uma conexao do pool ja existente do catalogo via lease
  RAII (`FirebirdMetadataLease`), sem expor `conn_info`.
- Faz uma query unica em `RDB$RELATIONS` para enumerar tabelas e
  views do usuario (`RDB$SYSTEM_FLAG = 0`,
  `RDB$RELATION_TYPE IN (NULL, 0, 1)`), com sub-query
  `RDB$INDICES`/`RDB$RELATION_CONSTRAINTS`/`RDB$INDEX_SEGMENTS` para
  detectar PK simples. PK composta retorna `NULL` (CASE COUNT(*) =
  1) e nao gera testes enganosos.
- Para cada tabela, chama `LoadTableSchema` que ja conhece
  `RDB$RELATION_FIELDS` e o mapeamento `RDB$FIELD_TYPE -> SQL_* ->
  LogicalType`.
- Compoe YAML deterministico: tabelas em ordem alfabetica por
  `RDB$RELATION_NAME`, colunas em ordem por `RDB$FIELD_POSITION`.
  Strings passam por `YamlQuote` (escape de `\\`, `\"`, `\n`,
  `\r`, `\t`, controles via `\xNN`); o nome da tabela nunca vira
  connection string.

#### Para que serve

- Bootstrap rapido de modelagem dbt a partir de uma base legada
  Firebird: rodar uma vez, salvar `sources.yml`, copiar para o
  projeto dbt-duckdb.
- Padronizar nomes de coluna e tipos esperados por
  `dbt deps`/`dbt run` sem digitar tabela por tabela.
- Habilitar testes dbt automaticos (`not_null`, `unique`) em
  chaves primarias detectadas.

#### Saida do YAML

Estrutura entregue:

```yaml
version: 2

sources:
  - name: "fb"
    schema: "main"
    description: ""
    tables:
      - name: "EMPLOYEE"
        description: ""
        columns:
          - name: "EMP_ID"
            data_type: "INTEGER"
            description: ""
            tests:
              - not_null
              - unique
          - name: "EMP_NAME"
            data_type: "VARCHAR"
            description: ""
          # ...
      - name: "TPK_COMPOSITE"
        description: ""
        columns:
          - name: "A"
            data_type: "INTEGER"
            description: ""
          - name: "B"
            data_type: "INTEGER"
            description: ""
          - name: "LABEL"
            data_type: "VARCHAR"
            description: ""
```

`data_type` segue o `LogicalType::ToString()` do DuckDB
(`INTEGER`, `DECIMAL(18,2)`, `DATE`, `BOOLEAN`, `VARCHAR`, `BLOB`,
etc) - bate com o que o usuario ve ao consultar via `ATTACH`.

#### Uso no dia a dia

```sql
ATTACH 'firebird://...' AS fb (TYPE firebird);
COPY (SELECT yaml FROM firebird_generate_dbt_sources('fb'))
  TO 'sources.yml' (FORMAT csv, HEADER false, QUOTE '');
DETACH fb;
```

#### Erros

- `catalog_name` ausente / NULL:
  `Binder Error: firebird_generate_dbt_sources(catalog_name VARCHAR):
  catalog_name is required (the alias from ATTACH '...' AS <alias>
  (TYPE firebird)).`
- Catalogo nao anexado:
  `Binder Error: firebird_generate_dbt_sources: no attached catalog
  named '...'. ATTACH first: ATTACH '<path-or-uri>' AS ... (TYPE
  firebird);`
- Catalogo anexado mas nao Firebird (ex.: DuckDB nativo):
  `Binder Error: firebird_generate_dbt_sources: catalog '...' is
  not a Firebird ATTACH (GetCatalogType() = '...'). Use the alias
  of an ATTACH created with (TYPE firebird).`

#### Limitacoes atuais

- Descricoes (`description: ""`): `RDB$DESCRIPTION` (BLOB SUB_TYPE
  1) de tabelas e colunas nao e lido nesta versao - sempre vazio.
  Captura via stream BLOB fica para uma fase futura.
- Sem named params (`source_name`, `target_schema`,
  `include_views`). O `name` do source bate com o `catalog_name`
  passado; `schema` e sempre `"main"`; views entram no YAML como
  tabelas (sem `tests`, ja que nao tem PK).
- PK composta nao gera bloco `tests`. Decisao deliberada: marcar
  qualquer coluna individual com `unique` seria semanticamente
  errado para uma PK multi-segmento. Se o usuario precisar do
  teste, deve adicionar manualmente um `tests:` customizado no
  arquivo gerado ou usar `dbt-utils.unique_combination_of_columns`.

## Nivel 3b - Metadata Bridge 2.0

A partir da v0.7, a extensao popula views padrao do `information_schema` com
dados de restricoes e fornece funcoes de catalogo adicionais para inspecao
aprofundada de metadados do Firebird.

### Views `information_schema` populadas

Apos um `ATTACH ... (TYPE firebird)`, as seguintes views sao populadas:

#### `information_schema.table_constraints`

Expoe restricoes PK, UNIQUE e FK para todas as tabelas do usuario.

```sql
SELECT constraint_name, constraint_type, table_name
FROM information_schema.table_constraints
WHERE table_catalog = 'fb'
ORDER BY table_name, constraint_type;
```

#### `information_schema.key_column_usage`

Mapeia colunas que participam de restricoes PK, UNIQUE e FK.

```sql
SELECT constraint_name, table_name, column_name, ordinal_position
FROM information_schema.key_column_usage
WHERE table_catalog = 'fb'
ORDER BY table_name, constraint_name, ordinal_position;
```

#### `information_schema.referential_constraints`

Expoe restricoes FK com informacoes de referencia.

**Nota**: as colunas `update_rule` e `delete_rule` sempre retornam
`'NO ACTION'` por limitacao do DuckDB. Para obter as regras reais
(`CASCADE`, `SET NULL`, etc.) use `firebird_foreign_keys` (descrito abaixo).

```sql
SELECT constraint_name, unique_constraint_name
FROM information_schema.referential_constraints
WHERE constraint_catalog = 'fb';
```

### `firebird_foreign_keys(catalog_name)`

Lista todas as restricoes de chave estrangeira com regras de referencia reais
do Firebird.

Colunas de saida:

| Coluna | Tipo | Descricao |
|---|---|---|
| `fk_schema` | VARCHAR | Sempre `'main'` |
| `fk_table` | VARCHAR | Tabela que declara a FK |
| `fk_constraint` | VARCHAR | Nome da restricao FK |
| `ordinal_position` | INTEGER | Posicao da coluna na chave (0-based) |
| `fk_column` | VARCHAR | Coluna na tabela filha |
| `pk_table` | VARCHAR | Tabela referenciada |
| `pk_constraint` | VARCHAR | Nome da restricao PK/UNIQUE referenciada |
| `update_rule` | VARCHAR | Regra real do Firebird (`CASCADE`, `SET NULL`, `NO ACTION`, etc.) |
| `delete_rule` | VARCHAR | Regra real do Firebird (`CASCADE`, `SET NULL`, `NO ACTION`, etc.) |

```sql
SELECT * FROM firebird_foreign_keys('fb');
```

**Nota**: `firebird_foreign_keys.ordinal_position` é 0-based (o `RDB$FIELD_POSITION` bruto do Firebird), enquanto `information_schema.key_column_usage.ordinal_position` é 1-based. Leve em conta a diferença de um ao juntar as duas superficies.

### `firebird_indexes(catalog_name)`

Lista todos os indices de usuario com seus segmentos.

Colunas de saida:

| Coluna | Tipo | Descricao |
|---|---|---|
| `table_schema` | VARCHAR | Sempre `'main'` |
| `table_name` | VARCHAR | Tabela dona do indice |
| `index_name` | VARCHAR | Nome do indice |
| `is_unique` | BOOLEAN | `true` se o indice e unico |
| `is_active` | BOOLEAN | `true` se o indice esta ativo |
| `segment_position` | INTEGER | Posicao da coluna no indice (0-based) |
| `column_name` | VARCHAR | Nome da coluna no segmento (`NULL` para indices de expressao) |
| `expression_source` | VARCHAR | Expressao (so para indices de expressao, `NULL` caso contrario) |

```sql
SELECT * FROM firebird_indexes('fb');
```

### `firebird_generators(catalog_name)`

Lista geradores/sequencias de usuario com valores inicial e atual.

Colunas de saida:

| Coluna | Tipo | Descricao |
|---|---|---|
| `generator_name` | VARCHAR | Nome do gerador |
| `initial_value` | BIGINT | Valor inicial configurado |
| `current_value` | BIGINT | Valor atual (lido via `GEN_ID(name, 0)`; `NULL` se sem privilegio). Lido por gerador individualmente (um round-trip cada) para preservar isolamento por gerador; em bancos com muitos geradores, isso implica N+1 round-trips. |

```sql
SELECT * FROM firebird_generators('fb');
```

### `firebird_domains(catalog_name)`

Lista dominios de usuario com tipo, nullable, charset e restricoes.

Colunas de saida:

| Coluna | Tipo | Descricao |
|---|---|---|
| `domain_name` | VARCHAR | Nome do dominio |
| `base_type` | VARCHAR | Tipo Firebird como string (ex.: `VARCHAR(100)`, `NUMERIC(10,2)`) |
| `length` | INTEGER | Comprimento em bytes (para tipos de texto) |
| `scale` | INTEGER | Escala decimal (valor absoluto) |
| `is_nullable` | BOOLEAN | `true` se o dominio permite `NULL` |
| `charset_name` | VARCHAR | Charset (so para tipos de texto) |
| `check_source` | VARCHAR | Clausula `CHECK` do dominio (`NULL` se nao ha) |
| `default_source` | VARCHAR | Clausula `DEFAULT` do dominio (`NULL` se nao ha) |

```sql
SELECT * FROM firebird_domains('fb');
```

**Nota**: para dominios `CHAR`/`VARCHAR`, o comprimento relatado é o comprimento em bytes declarado pelo Firebird (`RDB$FIELD_LENGTH`). Em charsets multibyte (ex.: UTF8) este é maior que o comprimento em caracteres (um `VARCHAR(10)` em UTF8 reporta `VARCHAR(40)`).

### `firebird_computed_columns(catalog_name)`

Lista colunas computadas (`COMPUTED BY`) de todas as tabelas de usuario.

Colunas de saida:

| Coluna | Tipo | Descricao |
|---|---|---|
| `table_schema` | VARCHAR | Sempre `'main'` |
| `table_name` | VARCHAR | Tabela que contem a coluna |
| `column_name` | VARCHAR | Nome da coluna computada |
| `expression_source` | VARCHAR | Expressao `COMPUTED BY` |

```sql
SELECT * FROM firebird_computed_columns('fb');
```

### `firebird_dependencies(catalog_name)`

Lista dependencias entre objetos do banco (tabelas, procedures, triggers, etc.).

Colunas de saida:

| Coluna | Tipo | Descricao |
|---|---|---|
| `object_name` | VARCHAR | Objeto dependente |
| `object_type` | VARCHAR | Tipo legivel do objeto (ex.: `TABLE`, `TRIGGER`, `PROCEDURE`) |
| `object_type_code` | INTEGER | Codigo `RDB$DEPENDENT_TYPE` bruto |
| `depends_on_name` | VARCHAR | Objeto do qual depende |
| `depends_on_type` | VARCHAR | Tipo legivel do objeto referenciado |
| `depends_on_type_code` | INTEGER | Codigo `RDB$DEPENDED_ON_TYPE` bruto |
| `field_name` | VARCHAR | Coluna especifica referenciada (`NULL` quando dependencia e de nivel de objeto) |

```sql
SELECT * FROM firebird_dependencies('fb');
```

### `firebird_comments(catalog_name)`

Lista comentarios (`RDB$DESCRIPTION`) de tabelas, views e colunas de usuario.

Colunas de saida:

| Coluna | Tipo | Descricao |
|---|---|---|
| `object_schema` | VARCHAR | Sempre `'main'` |
| `object_name` | VARCHAR | Nome da tabela ou view |
| `object_type` | VARCHAR | `'TABLE'`, `'VIEW'` ou `'COLUMN'` |
| `column_name` | VARCHAR | Nome da coluna (so para linhas de coluna; `NULL` para objeto) |
| `comment` | VARCHAR | Texto do comentario |

```sql
SELECT * FROM firebird_comments('fb');
```

## Nivel 4 - Diagnostico e observabilidade

### `firebird_profile_table(qualified_name)`

#### O que faz e como funciona

Retorna uma unica linha com diagnostico factual de uma relacao Firebird
acessivel por um catalogo Firebird ja anexado via `ATTACH ... (TYPE
firebird)`. O argumento e um nome qualificado no formato
`catalog.schema.table`; a parte de schema so e aceita como `main` (o
caminho ATTACH expoe exatamente um schema) e pode ser omitida.

```sql
ATTACH 'C:/dados/empresa.fdb' AS fb
(TYPE firebird, user 'APP_READONLY', password 'secret');

SELECT *
FROM firebird_profile_table('fb.main.CLIENTES');
-- forma curta equivalente:
SELECT *
FROM firebird_profile_table('fb.CLIENTES');
```

Colunas de saida:

- `table_name`
- `object_type`: `TABLE` ou `VIEW`.
- `has_primary_key`: booleano.
- `primary_key_columns`: lista de colunas da PK.
- `indexes`: lista de descricoes `NOME (COL, ...) [UNIQUE] [PK]`.
- `watermark_candidates`: colunas cujo tipo as torna candidatas plausiveis
  a watermark (familia inteira e date/timestamp).
- `filter_candidates`: colunas indexadas cujo tipo recebe pushdown barato.
- `full_scan_risk`: `LOW`, `MEDIUM` ou `HIGH`.
- `recommended_partitions`: valor `partitions=N` apenas como recomendacao.
- `warnings`: lista de ressalvas explicitas (strings legíveis por humanos).
- `alerts`: forma estruturada de `warnings`: `LIST(STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR))`.

Como funciona internamente:

- Valida que `catalog_name` existe e e um catalogo Firebird; senao,
  levanta `BinderException` acionavel (mesma checagem usada por
  `firebird_generate_dbt_sources`).
- Adquire conexao do pool do catalogo via lease RAII
  (`FirebirdMetadataLease`), sem expor `conn_info`.
- Le apenas tabelas de sistema do Firebird (`RDB$RELATIONS`,
  `RDB$RELATION_FIELDS`/`RDB$FIELDS`, `RDB$INDICES`,
  `RDB$INDEX_SEGMENTS`, `RDB$RELATION_CONSTRAINTS`) mais um probe
  best-effort de `MIN`/`MAX` da PK. Nunca le linhas de negocio.

Este e um diagnostico factual, nao um advisor de custo. As heuristicas
sao simples e explicitas: candidato a watermark e julgado pelo tipo, nao
por monotonicidade comprovada; `recommended_partitions` deriva da faixa
`MIN`/`MAX` da PK, nao de contagem de linhas, e e apenas recomendacao. A
coluna `warnings` carrega essas ressalvas inline.

#### `recommended_partitions` - como e calculado

`recommended_partitions` e um numero **consultivo (advisory)**. Ele **nao**
muda como o `firebird_scan` roda; nada e paralelizado automaticamente e nao
ha promessa de ganho de performance. Ele so diz qual `partitions=N` voce
*poderia* tentar.

Deriva apenas da largura da faixa `MIN`/`MAX` da PK numerica de coluna
unica (sem contagem de linhas, sem `COUNT(*)`, sem full scan):

- **Sem PK numerica de coluna unica** (view, sem PK, PK composta, PK
  nao-numerica): `recommended_partitions = 1`, com entrada em `warnings`
  explicando por que a alavanca de particionamento por faixa de PK nao esta
  disponivel.
- **PK numerica mas faixa pequena** (span `MIN`/`MAX` < 10000): ainda
  `recommended_partitions = 1`, com nota de que particionar adicionaria
  overhead sem paralelismo significativo. Uma PK numerica **nao** significa
  automaticamente que paralelo e recomendado.
- **PK numerica com faixa larga**: um `partitions=N` conservador entre 2 e 8
  (escalado pela largura da faixa, teto 8). Os `warnings` deixam claro que e
  consultivo, deriva da largura da faixa (entao pode ser desigual se a PK
  for esparsa) e deve ser validado contra o servidor ao vivo.

Quando `partitions > 1` e recomendado, `warnings` tambem carrega um
**caveat de paralelismo server-side**: se o paralelismo server-side do
Firebird ja estiver habilitado/configurado (ex.: `ParallelWorkers` do
Firebird 5), prefira comecar com `partitions=1` ou faca benchmark antes de
combinar paralelismo server-side e client-side. A extensao nao consulta a
configuracao de paralelismo do servidor, entao isso e um caveat generico,
nao uma condicao detectada.

Para `object_type = VIEW`, a funcao tambem faz uma inspecao rasa e
conservadora da definicao da view (`RDB$VIEW_SOURCE`) e emite `warnings`
quando detecta:

- um `JOIN` na definicao;
- agregacao (`GROUP BY` ou `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`/`LIST`);
- ausencia de `WHERE` na definicao.

A deteccao e busca de tokens, nao um parser SQL, e o texto da view nunca
e retornado - apenas os flags de forma alimentam os warnings. Literais de
string (incluindo aspas duplas escapadas do SQL) e comentarios sao
apagados antes da busca, entao texto com cara de keyword dentro de um
literal ou comentario nao gera falso-positivo. O espacamento e colapsado
antes, entao keywords quebradas por quebra de linha ou tab no texto da
view (ex.: `GROUP` e `BY` em linhas separadas) ainda sao detectadas. Quando a
definicao nao pode ser lida, emite o warning `view definition not
inspected`. Esses avisos apontam para materializar views pesadas via
DuckDB/dbt/Parquet em vez de varrer repetidamente.

#### Para que serve

- Decidir, antes de um scan, se vale consultar ao vivo, filtrar melhor,
  particionar ou materializar via DuckDB/dbt/Parquet.
- Descobrir rapidamente PK, indices, e boas colunas para filtro/watermark.
- Entender o risco de full scan sem precisar ler dados de negocio.

#### Uso no dia a dia

Perfilar uma tabela:

```sql
SELECT object_type, has_primary_key, recommended_partitions, full_scan_risk
FROM firebird_profile_table('fb.main.CLIENTES');
```

Ver candidatas a watermark e ressalvas:

```sql
SELECT watermark_candidates, warnings
FROM firebird_profile_table('fb.main.TABENTRADASAIDA');
```

#### Limitacoes atuais

- Sem estimativa de linhas: `recommended_partitions` usa a faixa
  `MIN`/`MAX` da PK, nao contagem real. Validar contra o servidor antes de
  paralelizar em producao.
- Watermark e candidato por tipo, nao por monotonicidade comprovada.
- Views nao tem PK, indices nem alavanca de particao: risco sempre `HIGH`,
  serial, com aviso de materializacao. A inspecao de view e rasa (busca de
  tokens em `RDB$VIEW_SOURCE`): detecta `JOIN`, agregacao e ausencia de
  `WHERE`, mas nao analisa plano nem profundidade de subconsultas. Join por
  virgula (sem a palavra `JOIN`) nao e detectado. Quando a fonte nao pode
  ser lida, emite `view definition not inspected`.
- Nome qualificado nao suporta identificadores com aspas/pontos embutidos
  nesta versao.

#### Coluna `alerts` — catalogo estruturado de avisos

`alerts` e a forma legivel por maquina de `warnings`. As duas colunas sao
produzidas a partir da mesma lista interna de alertas, portanto sao
**1:1 e na mesma ordem**: `alerts[i].message == warnings[i]` para todo indice `i`.

Cada elemento de `alerts` e um `STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR)`:

- `code` — identificador estavel (veja catalogo abaixo); contrato de API publica.
- `severity` — um dos valores `LOW`, `MEDIUM` ou `HIGH`, reutilizando o
  vocabulario de `full_scan_risk`: `LOW` = consultivo/informativo, `MEDIUM` =
  degradado mas funcional, `HIGH` = risco operacional real.
- `message` — a mesma string legivel por humanos que aparece em `warnings`.

**Contrato de codigos estaveis:** os codigos de alerta sao API publica. Uma vez
publicado, um codigo nunca e reutilizado para uma condicao diferente e seu
significado nunca muda. Novas condicoes sempre recebem novos codigos.

Catalogo de codigos de alerta:

| Codigo | Severidade | Significado |
| --- | --- | --- |
| `view_no_scan_lever` | HIGH | Objeto e uma VIEW: sem PK/indice/alavanca de particao |
| `view_definition_not_inspected` | MEDIUM | Fonte da view ilegivel; forma desconhecida |
| `view_contains_join` | HIGH | Definicao da view contem JOIN |
| `view_contains_aggregation` | HIGH | Definicao da view tem GROUP BY ou funcao de agregacao |
| `view_no_filter` | MEDIUM | Definicao da view nao tem filtro WHERE |
| `partition_advisory` | LOW | Particoes recomendadas (derivadas da faixa de PK) sao consultivas |
| `server_parallelism_caveat` | LOW | Caveat de paralelismo server-side (Firebird 5) quando `partitions > 1` |
| `pk_range_small_serial` | LOW | Faixa MIN/MAX da PK e pequena — scan serial recomendado |
| `no_primary_key` | HIGH | Sem chave primaria — apenas full scan, nao particionavel por faixa |
| `composite_pk_serial` | LOW | PK composta — apenas scan serial |
| `numeric_pk_no_range_serial` | LOW | PK numerica de coluna unica sem faixa MIN/MAX utilizavel — scan serial |
| `non_numeric_pk_serial` | LOW | PK nao-numerica de coluna unica — apenas scan serial |
| `no_indexed_filter_columns` | MEDIUM | Nenhuma coluna de filtro indexada barata encontrada |
| `none_charset_text_columns` | MEDIUM | Uma ou mais colunas texto usam CHARACTER SET NONE |

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
- `connection_id`: ID monotonico global ao processo, atribuido pela extensao
  no momento em que a `FirebirdConnection` e construida. Nao e um attachment
  id do Firebird; serve apenas para correlacionar reuso de pool entre
  consultas. Vale tanto no caminho `ATTACH` (vai do pool) quanto no
  `firebird_scan(...)` direto (sem pool).
- `connection_reused`: `true` quando a conexao veio reciclada da fila idle
  do pool no `Acquire`; `false` quando foi recem-construida. `firebird_scan(...)`
  direto nunca usa pool, entao fica em `false`. No caminho `ATTACH`, a
  *primeira* consulta SQL do usuario pode ja aparecer como `true` porque a
  inicializacao do catalogo (descoberta de esquema) abriu e devolveu uma
  conexao ao pool antes de qualquer consulta SQL chegar. E aquecimento de
  catalogo, nao bug.
- `parallel_scan`: `true` quando `partitions > 1`.
- `partitions`: numero de particoes.
- `captured_at`: timestamp de captura.
- `error_message`: erro sanitizado, vazio quando a query terminou limpa.
- `limit_pushed`: limite `ROWS` realmente empurrado ao Firebird
  (`row_limit`), ou `NULL` quando nenhum foi empurrado. `NULL` (nao `0`)
  para que um limite real de `0` nunca fique ambiguo.
- `offset_pushed`: offset `ROWS m TO n` empurrado (`row_offset`), ou `NULL`
  quando nenhum.
- `not_pushed_reasons`: uma razao coarse por entrada de `residual_filters`,
  mesma ordem/tamanho. Uma de `NONE_CHARSET`, `UNSUPPORTED_OP`,
  `ROWID_OR_INVALID_COLUMN`, `UNSUPPORTED_PROJECTION_MAPPING`.

As tres ultimas (`limit_pushed` / `offset_pushed` / `not_pushed_reasons`)
sao as colunas de explicabilidade de pushdown (Fase 4 #3). Deixam explicito
o que de paginacao chegou ao Firebird e por que um filtro ficou local, sem
funcao nova - o schema agora tem 18 colunas, compartilhado por
`firebird_last_query()` e `firebird_query_log()`. As razoes sao factuais e
coarse, nao um trace de planner:

- `NONE_CHARSET`: a coluna e texto `CHARACTER SET NONE` e o pushdown e
  bloqueado para que literais UTF-8 nao sejam comparados errado contra os
  bytes brutos no servidor. Registrado para predicados complexos liftados
  (`NOT IN`, e `LIKE` quando chega ao caminho de filtro complexo) que o
  scanner empurraria; o filtro bloqueado aparece como
  `complex_filter[none_gated]` em `residual_filters`.

  Limitacao conhecida: uma comparacao *simples* (`col = 'x'`, `col > 'x'`)
  numa coluna texto `CHARACTER SET NONE` em geral e aplicada pelo DuckDB
  acima do scan e nunca e oferecida ao conector como filtro empurravel,
  entao nao aparece em `residual_filters` / `not_pushed_reasons`. Um
  `LIKE 'x%'` de prefixo tambem e reescrito pelo DuckDB em comparacao de
  range antes e segue o mesmo caminho invisivel. Hoje o caso de gate NONE
  capturado de forma confiavel e o `NOT IN` complexo (e `LIKE` complexo).
  Tornar o gate de comparacao simples observavel fica para o futuro.
- `UNSUPPORTED_OP`: a forma/operador/tipo de constante do filtro nao e algo
  que o builder traduz para um predicado Firebird.
- `ROWID_OR_INVALID_COLUMN`: o filtro mira o rowid virtual ou uma coluna
  fora do schema resolvido.
- `UNSUPPORTED_PROJECTION_MAPPING`: o indice de coluna projetada do filtro
  nao pode ser mapeado de volta para uma coluna de origem.

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

### `firebird_explain_pushdown(sql)`

#### O que faz e como funciona

Retorna uma analise a priori, baseada apenas no plano logico, de o que um
`SELECT` empurraria ao Firebird — **sem executar a query** e **sem abrir cursor
de dados na query do usuario**. E o complemento de `firebird_last_query()`:
explain e prospectivo (antes de qualquer execucao), enquanto
`firebird_last_query()` e post-hoc (apos o ultimo scan real).

Explain nunca abre cursor de DADOS na query do usuario e nunca envia o SQL da
query ao Firebird. Em catalogo "frio" (primeira referencia a uma tabela), o
bind via `ExtractPlan` pode carregar metadados de catalogo (schema) e rodar o
probe de PK (MIN/MAX) — exatamente como o bind de qualquer query ATTACH — e
isso fica memoizado. Nao ha cursor de dados da query nem envio do SQL do
usuario.

**Nota sobre `remote_sql`:** a coluna mostra o SELECT remoto base que o scanner
enviaria; predicados complexos (LIKE-prefixo, NOT IN, BETWEEN) aparecem na
lista `pushed_filters` em vez de inline no `remote_sql` — o mesmo split de
telemetria usado por `firebird_last_query()`.

Como explain nunca executa a query, nao deixa registro no telemetro de
`firebird_last_query()`.

#### Entrada aceita

- Um unico `SELECT` referenciando tabelas Firebird via alias `ATTACH`
  (ex.: `fb.main.EMPLOYEE`).
- Um `WITH ... SELECT` (CTE) cujo corpo e um scan Firebird.

Entrada rejeitada (levanta erro):

- DML (`UPDATE`, `DELETE`, `INSERT`, `MERGE`).
- Chamadas diretas a `firebird_scan(...)` — use a forma de alias `ATTACH`.
- `WITH ... DELETE` ou outros CTEs nao-SELECT.

#### Colunas de saida (14)

| Coluna | Tipo | Notas |
| --- | --- | --- |
| `scan_ordinal` | BIGINT | Ordinal 1-based; distinto por no `LogicalGet`, inclusive self-joins |
| `table_name` | VARCHAR | Nome da tabela Firebird (nunca a connection string) |
| `remote_sql` | VARCHAR | SQL que seria enviado ao Firebird; valores de bind aparecem como `?` — sem literais, sem fragmentos de connection string |
| `projected_columns` | VARCHAR[] | Colunas selecionadas do Firebird apos projection pruning |
| `pushed_filters` | VARCHAR[] | Predicados que o scanner empurraria ao Firebird |
| `residual_filters` | VARCHAR[] | Predicados que o DuckDB revalidaria acima do scan; sempre `len(residual_filters) == len(not_pushed_reasons)` |
| `not_pushed_reasons` | VARCHAR[] | Uma razao coarse por entrada em `residual_filters`, mesma ordem; valores: `NONE_CHARSET`, `UNSUPPORTED_OP`, `ROWID_OR_INVALID_COLUMN`, `UNSUPPORTED_PROJECTION_MAPPING` |
| `limit_pushed` | BIGINT | Valor do parametro nomeado `row_limit=` se seria empurrado para clausula `ROWS` do Firebird; `NULL` para `LIMIT` SQL (o DuckDB aplica `LIMIT` SQL acima do scan) |
| `offset_pushed` | BIGINT | Valor do parametro nomeado `row_offset=` se empurrado; `NULL` caso contrario |
| `rows_clause` | VARCHAR | Clausula `ROWS m TO n` do Firebird que seria emitida, ou `NULL` |
| `pk_range_eligible` | BOOLEAN | `true` apenas para chave primaria de coluna unica numerica |
| `pk_range_column` | VARCHAR | Nome dessa coluna PK, ou `NULL` quando nao elegivel (ex.: PK composta ou nao numerica) |
| `pk_range_reason` | VARCHAR | Um de quatro valores normalizados: `single numeric PK`, `non-numeric PK`, `composite PK`, `no primary key` |
| `scan_strategy` | VARCHAR | `pk-range-partitionable` quando `pk_range_eligible` for true; `serial` caso contrario |

#### Invariantes

- `len(residual_filters) == len(not_pushed_reasons)` sempre se mantém.
- Um `NOT IN` em coluna texto `CHARACTER SET NONE` e registrado em
  `residual_filters` como `complex_filter[none_gated]` com a entrada paralela
  em `not_pushed_reasons` igual a `NONE_CHARSET`.
- `limit_pushed` / `offset_pushed` / `rows_clause` sao `NULL` para `LIMIT`
  SQL — apenas os parametros nomeados `row_limit=` / `row_offset=` do scanner
  sao considerados para pushdown.
- `pk_range_column` e `NULL` sempre que `pk_range_eligible` for `false`.

#### Exemplo de uso

```sql
ATTACH 'C:/dados/empresa.fdb user=APP_READONLY password=secret'
  AS fb (TYPE firebird);

-- Verificar o que seria empurrado antes de executar de verdade:
SELECT table_name, pushed_filters, residual_filters, not_pushed_reasons,
       pk_range_eligible, scan_strategy
FROM firebird_explain_pushdown(
  'SELECT EMP_ID, EMP_NAME FROM fb.main.EMPLOYEE WHERE EMP_ID > 10');
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

### `firebird_pool_stats(catalog_name)`

#### O que faz e como funciona

Retorna uma unica linha com o estado factual do pool de conexoes de um
catalogo Firebird anexado. O argumento e o **alias explicito do ATTACH** -
a funcao nao enumera catalogos.

```sql
ATTACH 'C:/dados/empresa.fdb' AS fb
(TYPE firebird, user 'APP_READONLY', password 'secret');

SELECT IDCLIENTE FROM fb.main.CLIENTES WHERE IDCLIENTE = 1;

SELECT * FROM firebird_pool_stats('fb');
```

Colunas de saida (8):

- `catalog_name`: o alias passado.
- `pool_enabled`: se o pool do ATTACH esta habilitado.
- `max_idle_size`: limite configurado da fila idle (`firebird_pool_max_size`);
  `0` = sem limite.
- `idle_timeout_ms`: expiracao idle configurada
  (`firebird_pool_idle_timeout_ms`); `0` = sem expiracao.
- `idle_connections`: conexoes paradas na fila idle agora.
- `total_created`: conexoes fisicas criadas ao longo da vida.
- `total_reused`: conexoes servidas da fila idle ao longo da vida.
- `total_discarded`: conexoes destruidas (pool desligado, ou limite/expiracao
  da fila idle atingidos).

Le apenas contadores e config que o pool ja rastreia, e **nao** faz lease de
conexao - chamar nunca perturba o pool que reporta. Os valores configurados
refletem as settings lidas no momento do `ATTACH`; um `SET` posterior nao
reconfigura um pool existente (refaca o `ATTACH` para aplicar).

#### Para que serve

- Confirmar que o pool esta de fato reusando conexoes.
- Dimensionar `firebird_pool_max_size`.
- Verificar que um catalogo com `firebird_pool_enabled = false` nunca
  estaciona conexoes idle.

#### Uso no dia a dia

```sql
SELECT pool_enabled, idle_connections, total_created, total_reused
FROM firebird_pool_stats('fb');
```

#### Limitacoes atuais

- **Por catalogo, so alias**: passa-se um alias do ATTACH; nao ha forma sem
  argumento que liste todos os catalogos anexados.
- **Sem contagem de ativas/in-use**: o pool rastreia a fila idle e
  contadores de vida, nao quantas conexoes estao emprestadas no momento.
  Contagem de conexoes ativas e trabalho futuro possivel.
- **Sem campo de ultimo erro**: historico de erro de pool nao e exposto
  nesta versao.

### `firebird_type_audit(catalog_name)`

#### O que faz e como funciona

Auditoria de fidelidade de conversao de tipos e charsets de um catalogo
Firebird anexado. Apenas colunas que possuem ressalva sao emitidas; colunas
cujo tipo Firebird mapeia para DuckDB de forma limpa e sem caveats semanticos
sao omitidas. A funcao nunca le dados de negocio: consulta apenas tabelas de
sistema do Firebird (`RDB$RELATION_FIELDS`, `RDB$FIELDS`) via lease de conexao
do pool, e nunca executa o SQL do usuario.

```sql
SELECT * FROM firebird_type_audit('fb');
```

Colunas de saida (7):

| Coluna | Tipo | Notas |
| --- | --- | --- |
| `table_schema` | VARCHAR | Sempre `'main'` (o ATTACH Firebird expoe um unico schema) |
| `table_name` | VARCHAR | Nome da relacao Firebird |
| `column_name` | VARCHAR | Nome do campo Firebird |
| `firebird_type` | VARCHAR | Tipo Firebird conforme reportado (ex.: `DECFLOAT(16)`, `CHAR(10) CHARACTER SET NONE`) |
| `duckdb_type` | VARCHAR | Tipo DuckDB projetado pela extensao (ex.: `VARCHAR`, `HUGEINT`) |
| `finding` | VARCHAR | Codigo de finding — veja tabela abaixo |
| `detail` | VARCHAR | Explicacao de uma frase sobre a ressalva |

Codigos de finding:

| Codigo | Significado |
| --- | --- |
| `decfloat_as_varchar` | DECFLOAT(16/34) projetado como VARCHAR via `CAST` server-side; semantica textual se aplica a comparacoes de filtro |
| `int128` | INT128 nativo ou NUMERIC/DECIMAL(19-38) sem casa decimal projetado como HUGEINT (sem perda); algumas ferramentas BI nao suportam INT128 |
| `time_tz` | TIME WITH TIME ZONE; ressalva de tratamento de offset de sessao e zone-id |
| `timestamp_tz` | TIMESTAMP WITH TIME ZONE; ressalva de tratamento de offset de sessao e zone-id |
| `none_charset` | CHAR/VARCHAR ou BLOB texto com CHARACTER SET NONE; decodificacao depende do `none_encoding` do scan (default documentado: `win1252`); a funcao nao le essa configuracao |
| `blob_text` | BLOB texto (charset nao-NONE) projetado como VARCHAR; ressalva de charset e tamanho |

#### Notas

- `decfloat_as_varchar`, `int128`, `time_tz` e `timestamp_tz` exigem
  Firebird 4 ou superior (tipos FB4+); nao aparecerao em catalogos FB3.
- O comportamento de decodificacao de `none_charset` no momento do scan e
  controlado pela opcao de sessao `none_encoding` (default `win1252`). Esta
  funcao le o schema do catalogo, nao essa configuracao; portanto, o finding
  e emitido sempre que o character set for NONE, independentemente do valor
  de `none_encoding`.
- `int128` sinaliza `INT128` nativo e `NUMERIC`/`DECIMAL` de precisao 19-38
  com escala 0 (todos projetados como HUGEINT). `NUMERIC`/`DECIMAL` com
  escala diferente de 0 mapeia de forma lossless para `DECIMAL` do DuckDB e
  nao e sinalizado.

### `firebird_health(alias)`

#### O que faz e como funciona

Retorna uma unica linha com o diagnostico de saude de um banco Firebird
acessivel por um alias ja anexado via `ATTACH ... (TYPE firebird)`. Le dados
de `MON$DATABASE`, `MON$ATTACHMENTS`, `rdb$get_context('SYSTEM','ENGINE_VERSION')`
e `RDB$DATABASE.RDB$CHARACTER_SET_NAME`. Operacao somente leitura.

```sql
ATTACH 'C:/dados/empresa.fdb' AS fb
(TYPE firebird, user 'SYSDBA', password 'masterkey');

SELECT *
FROM firebird_health('fb');
```

Colunas de saida (15):

| Coluna | Tipo | Descricao |
| --- | --- | --- |
| `engine_version` | VARCHAR | Versao do motor Firebird (ex.: `'5.0.1'`) |
| `ods_version` | VARCHAR | Versao do ODS do arquivo de banco (ex.: `'13.1'`) |
| `sql_dialect` | INTEGER | Dialeto SQL ativo do banco (normalmente `3`) |
| `default_charset` | VARCHAR | Character set padrao do banco (ex.: `'UTF8'`) |
| `page_size` | INTEGER | Tamanho de pagina em bytes (ex.: `8192`) |
| `forced_writes` | BOOLEAN | `true` quando escrita forcada esta ligada |
| `sweep_interval` | INTEGER | Intervalo de sweep configurado (transacoes) |
| `oldest_transaction` | BIGINT | OIT — oldest interesting transaction |
| `oldest_active` | BIGINT | OAT — oldest active transaction |
| `oldest_snapshot` | BIGINT | OST — oldest snapshot transaction |
| `next_transaction` | BIGINT | Proximo numero de transacao |
| `oit_gap` | BIGINT | `next_transaction - oldest_transaction` |
| `oat_gap` | BIGINT | `next_transaction - oldest_active` |
| `attachments` | INTEGER | Numero de conexoes visiveis ao usuario atual |
| `warnings` | LIST(VARCHAR) | Lista de avisos ativos (vazia quando tudo esta ok) |

Avisos possiveis:

| Codigo | Condicao de disparo |
| --- | --- |
| `oit_gap_high` | `oit_gap > 1.000.000` |
| `oat_gap_high` | `oat_gap > 1.000.000` |
| `sweep_disabled` | `sweep_interval = 0` |
| `forced_writes_off` | `forced_writes = false` |
| `charset_none` | `default_charset = 'NONE'` |
| `mon_unavailable` | Falha real na consulta de monitoramento |

Os quatro primeiros codigos (`oit_gap_high`, `oat_gap_high`, `sweep_disabled`,
`forced_writes_off`) so sao avaliados quando a leitura de monitoramento tem
sucesso; se `mon_unavailable` disparar, eles sao suprimidos porque suas
entradas ficam `NULL`. `charset_none` e sempre avaliado (deriva de
`default_charset`, lido de forma independente do `MON$`).

#### Notas

- **Limiar de gap `1.000.000`**: o valor `1000000` e um sinal conservador
  padrao, **nao** um limite universal. E uma constante de compilacao
  (`FB_HEALTH_GAP_THRESHOLD`); para ajustar e necessario recompilar a
  extensao.
- **`attachments` e a contagem visivel ao usuario atual**: sob privilegio
  limitado a visibilidade das views `MON$` e parcial; sob privilegio de
  monitoramento o total completo e retornado. O valor e sempre fiel ao que
  o banco reporta para aquela credencial — nao e um erro.
- **`mon_unavailable` so dispara em falha real**: visibilidade limitada
  (menos anexoes visiveis) nao dispara este aviso. Quando a consulta
  `MON$DATABASE`/`MON$ATTACHMENTS` lanca uma excecao, `mon_unavailable`
  aparece em `warnings`, as colunas com origem em `MON$` ficam `NULL`,
  e `engine_version` e `default_charset` continuam preenchidos.

#### Para que serve

- Verificar rapidamente o estado de saude de um banco Firebird em producao
  sem precisar acessar o servidor diretamente.
- Detectar gaps de transacao excessivos (indicativo de sweep atrasado ou
  transacoes longas) e configuracoes de risco (`forced_writes` desligado,
  charset `NONE`).
- Monitorar o numero de conexoes ativas e a versao do motor.

#### Uso no dia a dia

Inspecionar a saude do banco:

```sql
SELECT engine_version, oit_gap, oat_gap, attachments, warnings
FROM firebird_health('fb');
```

Verificar todos os campos de uma so vez:

```sql
SELECT *
FROM firebird_health('fb');
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

### `SET firebird_pool_enabled = true|false`

#### O que faz e como funciona

Liga ou desliga o `FirebirdConnectionPool` por sessao DuckDB. Quando `true`
(padrao), conexoes liberadas por uma consulta ATTACH voltam para a fila idle
e sao reaproveitadas pela proxima consulta contra o mesmo banco anexado.
Quando `false`, todo `Acquire` constroi conexao nova e todo `Release` a
destroi imediatamente.

Padrao:

```sql
SET firebird_pool_enabled = true;
```

Settings de pool sao lidas no momento do `ATTACH` que cria o catalogo. Um
`SET` posterior nao reconfigura o pool ja em uso por um `ATTACH` existente -
para aplicar, faca `DETACH` + `ATTACH` novamente.

#### Para que serve

- Desligar reuso para diagnosticar problema de estado preso (transacao,
  cache do Firebird, etc).
- Comparar custo de attach versus custo de query.
- Forcar conexao fresca durante teste de seguranca/audit.

#### Uso no dia a dia

```sql
SET firebird_pool_enabled = false;
ATTACH 'firebird://...' AS fb (TYPE firebird);
-- cada consulta abre conexao nova, connection_reused = false sempre.
```

### `SET firebird_pool_max_size = N`

#### O que faz e como funciona

Limita quantas conexoes idle podem ficar paradas na fila do pool. `0`
(padrao) = sem limite. Quando a fila esta cheia, o `Release` descarta a
conexao devolvida em vez de empilhar.

O limite e sobre **fila idle**, nao sobre **leases ativos**: nada impede o
scanner de paralelizar e segurar mais conexoes ativas simultaneamente; o
`max_size` so atua quando elas voltam ao pool.

Valores negativos sao rejeitados no `ATTACH` com `BinderException`
("firebird_pool_max_size must be >= 0 (0 = unlimited), got ...").

Padrao:

```sql
SET firebird_pool_max_size = 0;
```

Lido no `ATTACH`. Mudanca posterior nao reconfigura pool existente.

#### Para que serve

- Bater limite definido em politica interna (numero maximo de conexoes
  simultaneas a um banco Firebird legado).
- Conter consumo em servidores Firebird Embedded ou licenciados por
  conexao.

### `SET firebird_pool_idle_timeout_ms = MS`

#### O que faz e como funciona

Tempo, em milissegundos, que uma conexao pode ficar parada na fila idle
antes de ser descartada na proxima `Acquire`. `0` (padrao) = sem expiracao.
O relogio comeca no `Release()` que devolveu a conexao - nao no momento da
criacao da conexao, nem no `Acquire` anterior.

Valores negativos sao rejeitados no `ATTACH` com `BinderException`
("firebird_pool_idle_timeout_ms must be >= 0 (0 = no expiry), got ...").

Padrao:

```sql
SET firebird_pool_idle_timeout_ms = 0;
```

Lido no `ATTACH`. Mudanca posterior nao reconfigura pool existente.

#### Para que serve

- Forcar rotacao de conexoes paradas ha muito tempo, evitando que cache
  ou estado interno do Firebird fique velho.
- Liberar handles em sessoes longas que ficam ociosas entre rajadas de
  consultas.

## Notas de mapeamento de tipos

### `DECFLOAT(16)` / `DECFLOAT(34)`

`DECFLOAT(16)` e `DECFLOAT(34)` do Firebird 4+ sao IEEE 754
Decimal64 / Decimal128. O DuckDB nao tem tipo decimal-floating-point
nativo, e o caminho de cliente legado usado pela extensao nao tem decoder
de decimal-float.

Essas colunas sao expostas como **VARCHAR**, produzidas por um
`CAST(col AS VARCHAR(64))` server-side na query gerada. O Firebird
converte o valor para texto de forma lossless, entao a coluna chega como
texto exato:

- decimais comuns e formas com expoente fazem round-trip exato
  (ex.: `123.45`, `1.234567890123456789012345678901234E+200`);
- `NaN`, `Infinity`, `-Infinity` chegam como essas strings literais;
- um `NULL` real continua `NULL` (cast de NULL e NULL).

Isso **substitui o comportamento anterior**, em que a coluna era tipada
`DOUBLE` mas sempre retornava `NULL` - um schema enganoso. `VARCHAR`
lossless e honesto e consultavel. Nao ha decoder local
Decimal64/Decimal128 nem fallback `DOUBLE`; um caminho numerico lossy
seria um opt-in futuro.

O pushdown fica consistente com o schema `VARCHAR`: filtros empurrados
(comparacoes simples, `IN` / `NOT IN`) comparam contra a mesma expressao
`CAST(col AS VARCHAR(64))`, entao a semantica textual bate - ex.:
`WHERE D16 = '123.450'` nao casa com um `123.45` armazenado
(numericamente igual, textualmente diferente).

Limitacao: a fixture de teste DECFLOAT e dedicada
(`scripts/fixture_decfloat.sql`, referenciada por `FIREBIRD_DECFLOAT_DB`) e
**ainda nao esta na fixture principal de CI** - o teste pula quando a
variavel nao esta definida. Promove-la para `setup_test_firebird.sh` com a
atualizacao coordenada dos testes de `metadata` / `dbt-sources` esta
registrado como trabalho futuro.

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
