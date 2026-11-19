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
cair em fallback silencioso. No Windows, as DLLs de que o proprio
`fbclient.dll` precisa (no Firebird 3: `msvcr100.dll`, `zlib1.dll`, ICU)
sao procuradas primeiro na pasta da propria biblioteca, entao basta
apontar a variavel para uma instalacao ou kit portatil do Firebird.

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
- `partitions`: paralelismo. `0` = auto, `1` = serial. Com o padrao
  `partition_by = 'pk'`, as faixas vem de um probe `MIN/MAX` da PK; a
  primeira particao nao tem limite inferior e a ultima nao tem limite
  superior, entao linhas fora da faixa sondada (commitadas depois do probe,
  ou depois que um `ATTACH` guardou a faixa em cache) continuam sendo lidas.
- `partition_by`: `'pk'` (padrao) ou `'dbkey'`; veja "Como a tabela e
  dividida (`partition_by`)".
- `row_limit`: limita linhas usando `ROWS` no Firebird.
- `row_offset`: offset global; exige `row_limit`.
- `none_encoding`: estrategia para colunas `CHARACTER SET NONE`.
- `none_pushdown`: opt-in de pushdown de `=` / `IN` sobre colunas
  CHAR/VARCHAR `CHARACTER SET NONE` quando `none_encoding` e `win1252` ou
  `iso8859_1` (padrao `false`; veja "Tratamento de filtros de texto
  CHARACTER SET NONE" nas notas de mapeamento de tipos).
- `numeric_widen_int64`: opt-in que projeta colunas `NUMERIC`/`DECIMAL`
  com escala armazenadas como int64 de 64 bits (precisao 10-18, ou seja,
  `NUMERIC(18,s)` e afins) como `DECIMAL(38,s)` em vez de `DECIMAL(18,s)`,
  para que toda a faixa escalada do int64 do Firebird sobreviva (padrao
  `false`; veja "Notas de mapeamento de tipos").

#### Scans particionados leem um unico snapshot

Com `partitions > 1` (ou quando o caminho `ATTACH` particiona sozinho uma
tabela grande), cada particao e lida por uma conexao propria. No Firebird 4
em diante todas leem **um unico snapshot** do banco: uma conexao
coordenadora abre uma transacao SNAPSHOT somente leitura e cada particao
entra nela (`isc_tpb_at_snapshot_number`). Assim um scan particionado
devolve exatamente as linhas de um unico estado commitado, mesmo com outras
conexoes commitando alteracoes, como faz um scan serial.

- O snapshot fica aberto, somente leitura, ate o fim do scan, como qualquer
  transacao snapshot do Firebird. No Firebird 4+ com o padrao
  `ReadConsistency = 1`, um cursor read committed em execucao ja prende o
  proprio snapshot de statement enquanto roda, entao a coleta de lixo fica
  retida por praticamente o mesmo tempo de antes.
- No Firebird 3 (sem numero de snapshot), ou se o snapshot compartilhado nao
  puder ser estabelecido, cada particao usa sua propria transacao read
  committed, como nas versoes anteriores. As particoes podem entao ver
  estados commitados diferentes.
- Depois que o snapshot compartilhado e estabelecido, todo worker de
  particao precisa entrar nele. Se algum nao conseguir, o scan falha com um
  erro que cita a tabela e o motivo informado pelo servidor; nenhuma
  particao e lida fora do snapshot. O erro tambem fica em
  `firebird_last_query().error_message` quando outro worker do scan abre
  um cursor, antes ou depois da falha.
- Scans seriais nao mudam: uma conexao, uma transacao read committed. O
  snapshot de transacao (opt-in) do caminho `ATTACH` estende o snapshot a
  todos os scans de uma transacao DuckDB, inclusive os seriais; veja "Um
  snapshot por transacao DuckDB (`transaction_snapshot`)".
- `firebird_last_query().snapshot_mode` informa qual caso valeu: `shared`,
  `independent` ou `serial` (e `transaction` com o snapshot de transacao).
- Um scan particionado abre um attachment extra (o coordenador) enquanto
  dura. Ele nao vem do pool de conexoes do `ATTACH`, entao os contadores de
  `firebird_pool_stats()` nao mudam.

**Problemas conhecidos no servidor.** Dois bugs abertos do Firebird
enfraquecem essa garantia no Firebird 4, 5 e master:

- [FirebirdSQL/firebird#9227](https://github.com/FirebirdSQL/firebird/issues/9227):
  uma transacao que entra num snapshot com `isc_tpb_at_snapshot_number` pode
  ver um commit que o dono do snapshot nao ve. Uma transacao que commita
  enquanto as particoes entram no snapshot, sem nenhuma transacao
  read-write mais antiga ainda aberta, pode entao ficar visivel para umas
  particoes e nao para outras, e o scan mistura dois estados commitados.
- [FirebirdSQL/firebird#9301](https://github.com/FirebirdSQL/firebird/issues/9301):
  uma transacao SNAPSHOT que comeca enquanto outra transacao commita pode
  perder as linhas dessa transacao ou ver commits posteriores (correcao
  proposta em [#9303](https://github.com/FirebirdSQL/firebird/pull/9303)).

Os dois valem para o snapshot de um scan particionado, inclusive com
`partition_by = 'dbkey'`, e para o snapshot de transacao, em que a janela
de entrada dura a transacao DuckDB inteira. A extensao e somente leitura,
entao os dados gravados nunca correm risco; so a consistencia do que um scan
ou uma transacao le. Esta nota sera atualizada quando as correcoes forem
lancadas.

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
- Paralelismo opt-in: por faixas de PK quando a tabela tem chave apropriada,
  ou por faixas de `RDB$DB_KEY` com `partition_by = 'dbkey'` (Firebird 4/5).
- Protecao para `CHARACTER SET NONE`: texto bruto nao-UTF8 nao recebe pushdown
  textual quando isso pode alterar resultado.

#### Como a tabela e dividida (`partition_by`)

`partition_by` escolhe como o `firebird_scan` divide uma tabela quando
`partitions > 1`:

- `'pk'` (padrao): faixas de uma PK inteira de coluna unica, calculadas a
  partir de `MIN`/`MAX`. Igual as versoes anteriores.
- `'dbkey'`: faixas de `RDB$DB_KEY` sobre o espaco fisico de registros da
  tabela, calculado a partir de `RDB$PAGES` e do tamanho de pagina. Os
  limites sao lidos dentro do snapshot compartilhado do scan, entao nao ha
  probe de `MIN`/`MAX`. A divisao nao depende de como a PK esta
  distribuida, e **tabelas sem PK utilizavel tambem podem ser divididas**.
  O espaco conta paginas de ponteiro inteiras (uma cobre cerca de 12,8 MiB
  de paginas de dados com paginas de 8 KiB), entao as particoes so ficam
  equilibradas a menos de uma pagina de ponteiro: a maior parte de uma
  tabela menor que algumas paginas de ponteiro cai nas primeiras
  particoes. Sem um filtro empurrado que possa usar indice, cada particao
  e um full scan limitado
  (`RDB$DB_KEY >= MAKE_DBKEY(...) AND RDB$DB_KEY < MAKE_DBKEY(...)`, plano
  `Full Scan (lower bound, upper bound)`). Quando um filtro empurrado pode
  usar indice, o Firebird le por esse indice em cada particao e aplica os
  limites de DB_KEY so como filtro, entao cada particao repete a leitura
  inteira do indice: prefira `'pk'` nessas consultas.

`'dbkey'` so vale com `partitions` explicito de 2 ou mais, em tabelas base
persistentes, no Firebird 4.x ou 5.x (`MAKE_DBKEY` e scans limitados por
DB_KEY sao do Firebird 4.0+), e so quando o snapshot compartilhado do scan
foi iniciado. Em qualquer outro caso (Firebird 3, views, `partitions`
ausente ou `1`, sem snapshot compartilhado) o scan se comporta como sem
`partition_by`. Com `partitions` explicito de 2 ou mais, o probe de
`MIN`/`MAX` das faixas de PK roda entao no inicio do scan, a cada execucao.
`firebird_last_query().partition_method` informa o que foi usado: `pk`,
`dbkey` ou `none` (uma particao).

```sql
-- Tabela sem PK, dividida em quatro (Firebird 4/5):
SELECT COUNT(*)
FROM firebird_scan('<conn>', 'AUDIT_LOG', partitions=4, partition_by='dbkey');
SELECT partition_method, snapshot_mode, partitions FROM firebird_last_query();
-- dbkey  shared  4
```

Dividir um scan nem sempre o deixa mais rapido. Nas nossas medicoes (VM de
4 nucleos, TCP local, cliente Firebird 5.0.3) quatro particoes aceleraram um
scan de 19 colunas em 1,4 a 1,6x, mas deixaram scans estreitos ou com
projecao de colunas cerca de 2 vezes mais lentos que o serial. Faixas de
DB_KEY foram mais rapidas que faixas de PK, com uma diferenca proxima do
custo do probe de `MIN`/`MAX` que elas dispensam (1,5 a 2 s em 5 a 10 M
linhas). Nos nossos
testes a maior parte da lentidao veio da biblioteca cliente do Firebird, que
serializa alocacoes por linha entre conexoes
([FirebirdSQL/firebird#9297](https://github.com/FirebirdSQL/firebird/issues/9297)).
Meca antes de depender de `partitions > 1`.

#### Determinismo de paginacao (`RDB$DB_KEY`)

Ao paginar (`row_limit`/`row_offset`) uma tabela ou view sem chave primaria
numerica de coluna unica utilizavel, o `firebird_scan` ordena por
`RDB$DB_KEY` para que multiplas paginas lidas da mesma query vejam um limite
estavel e deterministico. (Antes, `ROWS` era empurrado sem nenhum `ORDER
BY`, deixando os limites de pagina nao-deterministicos — isso era um bug,
agora corrigido.)

`RDB$DB_KEY` e um **estabilizador fisico de paginacao valido apenas durante
a duracao de um scan/transacao**. Ele NAO e um contrato de ordenacao de
negocio, NAO e estavel entre transacoes ou apos atualizacao de linhas, e
NUNCA e usado para logica incremental/watermark ou recomendado como chave
em nenhum outro lugar desta extensao. O unico outro uso e
`partition_by = 'dbkey'`, em que as faixas de DB_KEY sao planejadas e lidas
dentro de um unico snapshot compartilhado, o unico escopo em que sao
estaveis.

Quando existe uma chave primaria numerica de coluna unica utilizavel, ela e
usada no `ORDER BY` em vez de `RDB$DB_KEY` — preferencial, por ser uma
coluna real e significativa em vez de um estabilizador fisico.

Para uma view pesada (com `JOIN`, `GROUP BY`, ou funcao de agregacao) ou uma
view cuja definicao nao pode ser lida, `RDB$DB_KEY` e inseguro — o Firebird
devolve silenciosamente `NULL` SQL para essas views em vez de gerar erro,
e e por isso que essa decisao e tomada estaticamente, no bind, a partir da
forma da view, em vez de detectada em tempo de execucao. Nesse caso o
`firebird_scan` nao empurra `ROWS` ao Firebird; ele aplica `row_limit`/
`row_offset` localmente, entao a fatia solicitada ainda e honrada
corretamente, apenas sem a otimizacao de pushdown server-side. Veja as
colunas `view_heavy` / `view_heavy_reasons` de `firebird_explain_pushdown`
mais abaixo para checar isso com antecedencia.

#### Reconciliacao de tipo de coluna em views

Quando o alvo do scan e uma view, os tipos de coluna sao reconciliados
contra o layout de colunas descrito em tempo real pelo Firebird, em vez de
confiar apenas nos metadados de catalogo da view (tanto o tipo DuckDB da
coluna quanto o descritor interno de fetch sao corrigidos juntos). O tipo
de uma coluna computada/agregada de uma view (ex.: um `SUM()` sobre uma
coluna `NUMERIC`) e congelado no momento do `CREATE VIEW` e pode
legitimamente discordar do que o compilador SQL do Firebird produz hoje
para a mesma expressao — sem essa reconciliacao, essa divergencia podia
derrubar o scan (um bug real, corrigido como issue #33). Scans de tabela
base nao sao afetados (a largura de armazenamento de uma tabela nao pode
discordar do proprio metadado de catalogo) e nao pagam custo adicional
por essa reconciliacao.

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

As opcoes espelham os parametros nomeados do `firebird_scan` — `user`,
`password`, `charset`, `role`, `dialect`, `none_encoding`, `none_pushdown`
(opt-in de pushdown de `=` / `IN` sobre colunas CHAR/VARCHAR
`CHARACTER SET NONE`) e `numeric_widen_int64` (opt-in de projecao
`DECIMAL(38,s)` para numericos escalados com backing int64):

```sql
ATTACH 'C:/dados/empresa.fdb' AS fb
(TYPE firebird, none_encoding 'win1252', none_pushdown true,
 numeric_widen_int64 true);
```

Desde a v1.2.1, `none_encoding` e `none_pushdown` tambem sao aceitos dentro
da propria string de conexao, nas duas formas suportadas — query de URI
(`firebird://usuario:senha@host:3050/path?charset=UTF8&none_encoding=win1252&none_pushdown=true`)
e DSN chave=valor (`database=C:/dados/empresa.fdb;user=APP_READONLY;password=secret;none_encoding=win1252;none_pushdown=true`).
Booleans aceitam `true`/`false`/`1`/`0`; qualquer outro valor falha com erro
acionavel (chaves desconhecidas continuam ignoradas — escreva com cuidado).
Precedencia: opcao explicita do ATTACH / parametro nomeado do
`firebird_scan` > string de conexao > default. `numeric_widen_int64`
permanece disponivel apenas como opcao do ATTACH / parametro nomeado.

A forma chave=valor tambem pode ser separada por espacos em vez de `;`
(`database=C:/dados/empresa.fdb user=APP_READONLY password=secret`), desde
que todo token seja uma chave conhecida (`database`, `user`, `password`,
`charset`, `role`, `dialect`, `none_encoding`, `none_pushdown`). Use `;`
quando um valor tiver espacos. Erros de conexao e o caminho exibido por
`duckdb_databases()` nunca mostram a senha. O proprio DuckDB cita o inicio
do comando que falhou em alguns erros (`LINE 1: ...`); uma string de
conexao escrita literalmente ali ainda pode aparecer. Passe-a por uma
variavel (`getvariable('cs')`) para evita-lo.

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

#### Um snapshot por transacao DuckDB (`transaction_snapshot`)

Por padrao cada scan le o seu proprio estado do banco: um scan particionado
le um unico snapshot (veja "Scans particionados leem um unico snapshot") e
um scan serial le na transacao read committed da sua conexao. Assim dois
scans de um mesmo statement (um join, um self-join, duas subqueries), ou
dois statements de um mesmo `BEGIN … COMMIT`, podem ver estados commitados
diferentes enquanto outras conexoes escrevem.

Com o snapshot de transacao ligado, todos os scans do `ATTACH` numa
transacao DuckDB leem **um unico snapshot** (Firebird 4+): um por statement
em modo autocommit, um por transacao dentro de `BEGIN … COMMIT`. Vem
desligado; ligue para a sessao ou para um `ATTACH`:

```sql
SET firebird_transaction_snapshot = true;            -- todo ATTACH Firebird

ATTACH 'firebird://srv/dados/erp.fdb' AS erp
  (TYPE firebird, transaction_snapshot true);        -- so este ATTACH
```

A opcao do `ATTACH` vence a configuracao nos dois sentidos
(`transaction_snapshot false` deixa um `ATTACH` de fora).

- No primeiro scan da transacao, uma conexao do pool do `ATTACH` abre uma
  transacao SNAPSHOT somente leitura (o coordenador), e todo scan da
  transacao entra nela com `isc_tpb_at_snapshot_number`, como as particoes
  de um scan. No `COMMIT` ou `ROLLBACK` o coordenador volta ao pool, de
  volta a read committed; ate la `firebird_pool_stats().active_connections`
  o conta. Com `firebird_pool_max_size = 1` ou o pool desligado, o
  coordenador e fechado a cada commit e a proxima transacao abre um attach
  novo (cerca de 12 ms por transacao num servidor local).
- Uma conexao do pool que o servidor ja fechou (um restart, uma queda de
  rede) e pulada na escolha do coordenador. Se nenhum coordenador puder ser
  iniciado, o scan falha em vez de ler sem o snapshot. Toda conexao que
  entra no snapshot confere em seguida se o coordenador ainda o segura (uma
  ida e volta ao servidor), e o scan falha se ele foi perdido: o Firebird
  reinicia os numeros de snapshot quando o banco e reaberto, entao um
  numero perdido poderia ser do snapshot de outra sessao. Um scan cuja
  propria conexao do pool foi perdida tambem falha. Em todos os casos, rode
  a consulta de novo (dentro de `BEGIN … COMMIT`, depois de um `ROLLBACK`).
- **Scans seriais tambem mudam**: leem o snapshot da transacao, nao read
  committed, entao nao veem commits feitos depois do primeiro scan Firebird
  da transacao.
- `firebird_last_query().snapshot_mode` informa `transaction` para cada scan
  que leu o snapshot da transacao.
- Se um scan nao consegue entrar no snapshot, ele falha com um erro que cita
  a tabela e o motivo informado pelo servidor, como um scan particionado.
- **Firebird 3** nao tem numero de snapshot: os scans se comportam como sem
  a opcao e informam `serial` ou `independent`. A primeira transacao de um
  `ATTACH` descobre isso, e as seguintes nao tentam de novo ate o proximo
  `ATTACH`.
- Uma chamada direta de `firebird_scan()` nao faz parte da transacao de um
  `ATTACH` e nao e afetada. Assim `partition_by`, um parametro de
  `firebird_scan()`, nunca se combina com esta opcao: scans do `ATTACH` nao
  o aceitam.
- Os problemas conhecidos no servidor listados em "Scans particionados leem
  um unico snapshot" valem aqui tambem, pela transacao inteira.

**Limite de ociosidade.** Uma transacao DuckDB aberta prende o seu snapshot
no servidor e, como qualquer transacao SNAPSHOT longa, retem a coleta de
lixo. `firebird_transaction_snapshot_idle_timeout` (segundos, padrao `60`,
`0` = sem limite) limita o tempo entre o fim de um statement e o proximo
scan do `ATTACH` na transacao: no fim de cada statement a extensao aplica
`SET SESSION IDLE TIMEOUT` no coordenador, e o proximo scan o desliga. Se
nenhum scan vier a tempo, o proprio servidor fecha o coordenador e libera o
snapshot, mesmo que o cliente nao faca nada. O proximo scan da transacao
entao falha:

```text
IO Error: Firebird ATTACH 'erp': the snapshot of this DuckDB transaction is
gone. Its coordinating connection was closed by the server, because no scan
of this ATTACH ran in the transaction for
firebird_transaction_snapshot_idle_timeout (60 s), or because the
connection was lost. Run the query again (inside BEGIN ... COMMIT, after a
ROLLBACK).
```

- Depois que um statement iniciou um scan do `ATTACH`, o resto desse
  statement nunca conta, por mais que demore. Statements que nao leem o
  `ATTACH` contam.
- Um statement termina quando o resultado e lido ate o fim ou fechado. **Um
  cliente que mantem um resultado aberto** (uma API ou ferramenta de BI
  segurando um cursor) mantem o snapshot enquanto fizer isso, como qualquer
  statement em execucao, com ou sem `BEGIN`: o limite so comeca quando o
  resultado termina.
- Lido no primeiro scan Firebird da transacao; um `SET` posterior vale para
  a proxima transacao.
- O Firebird conta o timeout em segundos inteiros, entao a extensao pede um
  segundo a mais: o snapshot sobrevive pelo menos o tempo configurado. Se o
  servidor recusar o timer, o snapshot e liberado na hora, e o proximo scan
  da transacao falha com o motivo informado pelo servidor.
- Valores fora de `0..2147483646` sao rejeitados pelo `SET` (o servidor
  aceita ate 2³¹ − 1 segundos, e a extensao soma um):
  `firebird_transaction_snapshot_idle_timeout must be 0 (no limit) to 2147483646 seconds, got -1`.

### `firebird_query(alias, sql)`

Executa um `SELECT` em um banco Firebird anexado exatamente como escrito e
retorna suas linhas. Use para SQL Firebird que o DuckDB nao expressa sobre
`fb.main.<tabela>`: procedures selecionaveis, `FIRST`/`SKIP`, funcoes do
Firebird, joins e agregacoes que voce quer que o servidor execute.

```sql
ATTACH 'database=C:/dados/empresa.fdb;user=APP_READONLY;password=secret' AS fb (TYPE firebird);

SELECT * FROM firebird_query('fb', 'SELECT DEPT_NO, COUNT(*) AS N FROM EMPLOYEE GROUP BY 1');
SELECT * FROM firebird_query('fb', 'SELECT * FROM SP_PEDIDOS_DO_DIA(CURRENT_DATE)');
```

| Argumento | Tipo | Descricao |
|---|---|---|
| `alias` | VARCHAR | Nome de um ATTACH `TYPE firebird` |
| `sql` | VARCHAR | Um `SELECT` Firebird (CTE, procedure selecionavel e `EXECUTE BLOCK` com `SUSPEND` tambem sao `SELECT`s) |

Os dois argumentos podem ser qualquer expressao constante, por exemplo
`firebird_query('fb', getvariable('sql_extracao'))`.

- **Somente SELECT.** O comando e preparado no bind e seu tipo lido
  (`isc_info_sql_stmt_type`); `UPDATE`, `EXECUTE PROCEDURE`, DDL e os
  demais falham no bind.
- **Transacao somente leitura.** Um `SELECT` ainda pode executar codigo
  que grava: uma procedure selecionavel, ou um `EXECUTE BLOCK` com
  `SUSPEND`. Ele roda na transacao somente leitura do pool do ATTACH,
  entao o servidor recusa essas gravacoes ("attempted update during
  read-only transaction"). Testado no Firebird 3, 4 e 5, a recusa tambem
  cobre:
  - DDL executado via `EXECUTE STATEMENT`;
  - blocos `IN AUTONOMOUS TRANSACTION` (a transacao autonoma tambem e
    somente leitura);
  - `EXECUTE STATEMENT ... ON EXTERNAL DATA SOURCE`, na transacao de quem
    chama ou `WITH AUTONOMOUS TRANSACTION` (testado contra o proprio
    banco).
- **Onde o somente leitura para.**
  - Sequences nao sao transacionais: `GEN_ID(G, 1)` ou `NEXT VALUE FOR G`
    no `SELECT` avanca `G`, e o avanco fica.
  - Funcoes externas (UDFs e UDRs) rodam fora do controle de transacao. A
    transacao somente leitura nao impede o que elas fazem fora do banco,
    como ao executar o comando no `isql` contra o seu proprio banco.
- **Tipos.** Cada coluna do resultado vira o tipo DuckDB que um scan da ao
  mesmo tipo Firebird, com o `none_encoding` e o `numeric_widen_int64` do
  ATTACH. `DECFLOAT` volta como texto, como no scan. Os nomes das colunas
  sao os do Firebird (aliases); duplicados ganham os sufixos usuais do
  DuckDB, e um nome vazio vira `columnN`.
- **Sem planejamento.** O SQL vai ao Firebird como escrito: sem pushdown
  de projecao, filtro ou `ROWS`, sem particoes. Um filtro escrito em volta
  dele no DuckDB e aplicado depois que as linhas chegam.
  `firebird_explain_pushdown` o mostra com `scan_strategy = 'passthrough'`
  e `table_name` NULL.
- **Snapshot.** Com `firebird_transaction_snapshot` ligado, ele le o
  snapshot da transacao DuckDB, como os scans do ATTACH.
- **Telemetria.** Cada chamada deixa um registro em
  `firebird_last_query()` / `firebird_query_log()`: o SQL enviado em
  `remote_sql`, `rows_read`, tempos, `partitions` 1 e `table_name` vazio.
- **Preparado duas vezes.** O comando e preparado no bind (para conhecer
  as colunas) e de novo ao executar. Se as colunas mudarem nesse
  intervalo, a consulta falha e pede para ser executada de novo.

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

### `firebird_profile_table(qualified_name [, exact_row_count])`

#### O que faz e como funciona

Retorna uma unica linha com diagnostico factual de uma relacao Firebird
acessivel por um catalogo Firebird ja anexado via `ATTACH ... (TYPE
firebird)`. O argumento e um nome qualificado no formato
`catalog.schema.table`; a parte de schema so e aceita como `main` (o
caminho ATTACH expoe exatamente um schema) e pode ser omitida. O parametro
nomeado opcional `exact_row_count=true` troca a estimativa barata de linhas
por um `COUNT(*)` real no servidor (veja "Estimativa de linhas" abaixo).

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
- `estimated_rows`: melhor contagem disponivel: limite superior pela faixa
  da PK por padrao, `COUNT(*)` exato com `exact_row_count=true`; `NULL`
  quando nao ha faixa de PK numerica e nenhuma contagem exata rodou.
- `row_estimate_method`: `pk_range_upper_bound` ou `exact_count`; `NULL`
  junto com uma estimativa `NULL`.

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

#### Estimativa de linhas - como e produzida

`estimated_rows` nunca dispara um full scan por conta propria:

- **`pk_range_upper_bound`** (padrao, tabelas com PK numerica de coluna
  unica): `MAX(PK) - MIN(PK) + 1`, do mesmo probe que alimenta
  `recommended_partitions`. E um *limite superior que assume chave
  densa* — colunas identity/sequence acompanham bem; chaves esparsas
  (lacunas, deletes) superestimam. Nunca e apresentado como contagem.
- **`exact_count`** (somente com `exact_row_count=true`): um
  `SELECT COUNT(*)` real executado no servidor sobre a tabela de origem.
  Isto le todas as linhas do servidor — peca conscientemente. Quando a
  contagem nao pode rodar (ex.: privilegio), a estimativa barata
  permanece e `row_estimate_method` mostra o que foi de fato produzido; o
  alerta `exact_count_executed` so e emitido quando a contagem rodou.
- Tabelas sem PK numerica de coluna unica utilizavel (composta /
  nao-numerica / sem PK) nao tem estimativa barata: ambas as colunas ficam
  `NULL` a menos que `exact_row_count=true` produza uma.

#### `recommended_partitions` - como e calculado

`recommended_partitions` e um numero **consultivo (advisory)**. Ele **nao**
muda como o `firebird_scan` roda; nada e paralelizado automaticamente e nao
ha promessa de ganho de performance. Ele so diz qual `partitions=N` voce
*poderia* tentar.

A partir desta versao, `recommended_partitions` e calculado pela mesma
funcao `PickPartitionCount` que o proprio `firebird_scan` chama em tempo de
scan. Antes eram duas formulas separadas, calibradas de forma diferente,
entao a recomendacao de um profile e a contagem de particoes escolhida por
um scan real podiam discordar; agora nao podem mais.

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
| `no_primary_key` | HIGH | Sem chave primaria — full scan, sem faixas de PK (`partition_by='dbkey'` pode dividi-la no Firebird 4/5) |
| `composite_pk_serial` | LOW | PK composta — serial por padrao (`partition_by='dbkey'` pode dividi-la no Firebird 4/5) |
| `numeric_pk_no_range_serial` | LOW | PK numerica de coluna unica sem faixa MIN/MAX utilizavel — scan serial |
| `non_numeric_pk_serial` | LOW | PK nao-numerica de coluna unica — serial por padrao (`partition_by='dbkey'` pode dividi-la no Firebird 4/5) |
| `no_indexed_filter_columns` | MEDIUM | Nenhuma coluna de filtro indexada barata encontrada |
| `none_charset_text_columns` | MEDIUM | Uma ou mais colunas texto usam CHARACTER SET NONE |
| `filter_before_scan` | MEDIUM | Tabela base de risco HIGH com colunas filtraveis — a mensagem as nomeia |
| `materialize_before_scan` | HIGH | Tabela base de risco HIGH sem colunas de filtro/watermark — materialize |
| `exact_count_executed` | LOW | `exact_row_count=true` executou um `COUNT(*)` no servidor |

### `firebird_index_profile(qualified_name)`

#### O que faz e como funciona

Retorna **uma linha por indice existente** de uma tabela Firebird acessivel
por um catalogo ja anexado via `ATTACH ... (TYPE firebird)`. O argumento e um
nome qualificado no mesmo formato de `firebird_profile_table`:
`catalog.schema.table` (schema so aceito como `main`, e pode ser omitido).

```sql
ATTACH 'C:/dados/empresa.fdb' AS fb
(TYPE firebird, user 'APP_READONLY', password 'secret');

SELECT *
FROM firebird_index_profile('fb.main.CLIENTES');
```

**Grao por indice:** uma tabela sem nenhum indice ainda assim emite
exatamente **uma linha sintetica** — nunca zero linhas. Nessa linha,
`index_name IS NULL` marca a ausencia de indice, `columns` vem como lista
vazia `[]`, e as demais colunas especificas de indice (`is_unique`,
`is_active`, `is_primary_key`, `is_foreign_key`, `selectivity`) ficam `NULL`.
A linha sintetica sempre carrega o alerta `no_indexes_on_table` (severidade
`HIGH`) em `alerts`.

Colunas de saida (9):

| Coluna | Tipo | Descricao |
| --- | --- | --- |
| `index_name` | VARCHAR | Nome do indice; `NULL` na linha sintetica de "sem indice" |
| `columns` | LIST(VARCHAR) | Colunas do indice, na ordem dos segmentos; `[]` na linha sintetica |
| `is_unique` | BOOLEAN | Indice unico; `NULL` na linha sintetica |
| `is_active` | BOOLEAN | Indice ativo (`RDB$INDICES.RDB$INDEX_INACTIVE = 0`); `NULL` na linha sintetica |
| `is_primary_key` | BOOLEAN | Indice suporta a chave primaria da tabela; `NULL` na linha sintetica |
| `is_foreign_key` | BOOLEAN | Indice suporta uma chave estrangeira; `NULL` na linha sintetica |
| `selectivity` | DOUBLE | Valor bruto de `RDB$STATISTICS`; `NULL` quando nunca calculado; `NULL` na linha sintetica |
| `alerts` | LIST(STRUCT(code VARCHAR, severity VARCHAR, message VARCHAR)) | Alertas desse indice (ou o alerta de "sem indice" na linha sintetica) |
| `unindexed_filter_candidates` | LIST(VARCHAR) | Colunas da tabela nao cobertas por nenhum segmento de indice (de nenhum indice, ativo ou inativo); repetida em toda linha |

Catalogo de codigos de alerta:

| Codigo | Severidade | Condicao |
| --- | --- | --- |
| `no_indexes_on_table` | HIGH | Tabela sem nenhum indice — dispara a linha sintetica |
| `index_inactive` | MEDIUM | `is_active = false` para esse indice |
| `missing_statistics` | LOW | `RDB$STATISTICS IS NULL` para esse indice |

#### Notas

- `selectivity` e um valor **bruto**, sem interpretacao: um valor **menor**
  tende a indicar indice **mais seletivo**. `NULL` significa que a
  estatistica nunca foi calculada — **nao** significa "desatualizada".
  **Sem limiar numerico** nesta versao: nao existe alerta de baixa
  seletividade (evita a funcao virar um advisor opaco).
- `unindexed_filter_candidates` exclui qualquer coluna coberta por
  **qualquer** segmento de indice, inclusive de indice **inativo** — esse
  sinal de inatividade chega separadamente pelo alerta `index_inactive` na
  propria linha daquele indice, nao pela omissao da lista.

#### Para que serve

- Ver, indice por indice, quais existem, se estao ativos, se tem estatistica
  calculada, e se suportam PK/FK.
- Confirmar rapidamente se uma tabela ficou sem nenhum indice.
- Descobrir colunas candidatas a filtro que hoje nao tem nenhuma cobertura de
  indice.

#### Uso no dia a dia

Perfilar os indices de uma tabela:

```sql
SELECT index_name, columns, is_unique, is_active, selectivity
FROM firebird_index_profile('fb.main.CLIENTES');
```

Ver candidatas a filtro sem cobertura de indice:

```sql
SELECT DISTINCT unindexed_filter_candidates
FROM firebird_index_profile('fb.main.CLIENTES');
```

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
- `bytes_read_estimate`: ESTIMATIVA de bytes de payload lidos do Firebird
  (G5) - veja a definicao exata logo abaixo.
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
- `snapshot_mode`: como as particoes do scan se relacionam
  transacionalmente: `serial` (uma particao), `shared` (todas as particoes
  leram um unico snapshot do Firebird; Firebird 4+) ou `independent` (cada
  particao usou sua propria transacao read committed: Firebird 3, ou o
  snapshot compartilhado nao pode ser estabelecido). Com o snapshot de
  transacao do `ATTACH` (opt-in), tambem `transaction`: o scan leu o
  snapshot da sua transacao DuckDB (Firebird 4+). Veja "Scans
  particionados leem um unico snapshot".
- `partition_method`: como a tabela foi dividida: `none` (uma particao),
  `pk` (faixas de PK) ou `dbkey` (faixas de `RDB$DB_KEY`,
  `partition_by = 'dbkey'`).
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
funcao nova. `bytes_read_estimate` (G5), `snapshot_mode` e
`partition_method` entram como colunas aditivas no fim - o schema agora
tem 21 colunas, compartilhado por `firebird_last_query()` e
`firebird_query_log()`. As razoes sao factuais e
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

Remedio por codigo de `not_pushed_reasons` (os codigos sao uma API
estavel):

| Motivo | Significado | Remedio |
|---|---|---|
| `NONE_CHARSET` | O filtro mira uma coluna de texto `CHARACTER SET NONE` e nao foi empurrado. Com `none_pushdown=true` isso significa que o literal nao tem byte no encoding alvo (ex. `'€'` em `iso8859_1`) ou a coluna e BLOB de texto NONE; sem a flag, todo filtro de texto em colunas NONE e barrado. | Use `none_encoding='strict'` para dados ja em UTF-8, ou ative `none_pushdown=true` (parametro do `firebird_scan` ou opcao do ATTACH) para empurrar `=` / `IN` sobre CHAR/VARCHAR NONE; ranges, `LIKE` e `NOT IN` permanecem sempre no cliente. Confirme com `firebird_explain_pushdown`. |
| `UNSUPPORTED_OP` | O operador/forma/tipo de constante do filtro ainda nao tem traducao para SQL Firebird. | Simplifique o predicado ou aceite o filtro do lado DuckDB. |
| `ROWID_OR_INVALID_COLUMN` | O filtro mira o rowid virtual ou coluna fora do schema resolvido. | Filtre por colunas reais em vez de `rowid`. |
| `UNSUPPORTED_PROJECTION_MAPPING` | A coluna projetada do filtro nao pode ser mapeada de volta para uma coluna de origem. | Confira a lista de colunas / forma da projecao da query. |

Para fazer filtros residuais agirem em vez de so reportar, use
`SET firebird_unpushed_mode = 'warn' | 'error'` - veja
[Nivel 5 - Opcoes de sessao](#nivel-5---opcoes-de-sessao).

#### Definicao exata de `bytes_read_estimate` (ESTIMATIVA)

O fbclient nao expoe medidor de trafego de rede; a extensao **estima** o
payload transferido:

```
bytes_read_estimate = rows_read x largura da linha no XSQLDA
                      + bytes de segmentos de BLOB lidos
```

- **Largura da linha no XSQLDA** e a soma dos tamanhos dos descritores de
  fetch por coluna, fixada uma vez no prepare: `CHAR(n)` conta `n` bytes,
  `VARCHAR(n)` conta `n + 2` (prefixo de tamanho), numericos/temporais
  contam sua largura fixa, e uma coluna BLOB conta seu quadro de 8 bytes
  (o id do BLOB).
- **Conteudo de BLOB** e contado onde de fato e lido: os bytes de segmento
  devolvidos por BLOB entram na estimativa na medida em que cruzam o
  `isc_get_segment`.
- **Deterministico**: duas execucoes identicas da mesma query reportam a
  mesma estimativa (mesma largura, mesmo numero de linhas).

Nao entra na conta: headers de protocolo e enquadramento por linha, as
round-trips de prepare/execute/bind, metadados de paginacao (`ROWS`) e o
handshake de conexao/attachment. Trate como estimativa de payload para
orcamento de rede - "esse scan vai mover ~X MB" - nao como medidor exato
de bytes de fio.

Exemplo de orcamento de rede (WAN de ~0,3 MB/s - esse scan leva minutos ou
horas?):

```sql
SELECT rows_read, bytes_read_estimate,
       bytes_read_estimate / rows_read AS bytes_per_row,
       1.0 * bytes_read_estimate / 1024 / 1024 AS est_mb
FROM firebird_last_query();
```

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

Orcar a rede antes de rodar um scan pesado (probe curto com `row_limit`,
depois extrapolar):

```sql
SELECT rows_read, bytes_read_estimate,
       1.0 * bytes_read_estimate / 1024 / 1024 AS est_mb
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

#### Colunas de saida (18)

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
| `scan_strategy` | VARCHAR | `pk-range-partitionable` quando `pk_range_eligible` for true; `passthrough` para uma chamada de `firebird_query()` (SQL enviado como escrito, `table_name` NULL); `serial` caso contrario |
| `view_heavy` | BOOLEAN | `NULL` quando o alvo do scan e uma tabela base; para uma view, `true` se ela tem `JOIN`, `GROUP BY`, funcao de agregacao, ou definicao ilegivel/nao-inspecionavel, `false` para uma view simples de passagem |
| `view_heavy_reasons` | VARCHAR[] | Notas factuais sobre a forma da view: `view contains a JOIN`, `view contains aggregation (GROUP BY or aggregate functions)`, `view definition not inspected (RDB$VIEW_SOURCE unavailable or unreadable)`, `view has no WHERE filter in its definition`. Esta ultima pode aparecer mesmo quando `view_heavy` e `false` (uma view simples sem filtro nao e "pesada", mas a observacao ainda e util) — nao assuma que a lista fica vazia sempre que `view_heavy` e `false`. Vazio (e `view_heavy` e `NULL`) quando o alvo e uma tabela base. |
| `charset_pushdown_blocked` | BOOLEAN | `true` quando qualquer entrada em `not_pushed_reasons` e `NONE_CHARSET` — uma sintese pura de um sinal ja existente, sem deteccao nova |
| `planned_partitions` | BIGINT | O numero exato de particoes que o `firebird_scan` usaria para a faixa de PK desta tabela, calculado pela mesma funcao `PickPartitionCount` que o scanner chama; `NULL` quando `pk_range_eligible` e `false` |

#### Invariantes

- `len(residual_filters) == len(not_pushed_reasons)` sempre se mantém.
- Um `NOT IN` em coluna texto `CHARACTER SET NONE` e registrado em
  `residual_filters` como `complex_filter[none_gated]` com a entrada paralela
  em `not_pushed_reasons` igual a `NONE_CHARSET`.
- `limit_pushed` / `offset_pushed` / `rows_clause` sao `NULL` para `LIMIT`
  SQL — apenas os parametros nomeados `row_limit=` / `row_offset=` do scanner
  sao considerados para pushdown.
- `pk_range_column` e `NULL` sempre que `pk_range_eligible` for `false`.
- `planned_partitions` e `NULL` sempre que `pk_range_eligible` for `false`.

#### Notas sobre `view_heavy`, `charset_pushdown_blocked` e `planned_partitions`

- `planned_partitions` e o valor exato que o `firebird_scan` usaria para a
  faixa de PK desta tabela — calculado pela mesma funcao
  `PickPartitionCount`, nao uma estimativa separada.
- `charset_pushdown_blocked` e uma sintese booleana de `not_pushed_reasons`
  contendo `NONE_CHARSET` — um flag de conveniencia sobre um sinal ja
  existente, nao uma deteccao nova.
- `view_heavy` / `view_heavy_reasons`: quando o alvo do scan e uma view com
  `JOIN`, `GROUP BY`, funcao de agregacao, ou definicao ilegivel,
  `view_heavy = true` e `view_heavy_reasons` explica por que. Isso tambem
  explica por que `remote_sql` pode nao mostrar clausula `ROWS` mesmo
  quando `row_limit`/`row_offset` foram pedidos contra essa view — veja a
  nota sobre `RDB$DB_KEY` na secao `firebird_scan` acima. `view_heavy_reasons`
  pode carregar a nota `view has no WHERE filter in its definition`
  independentemente de `view_heavy` — uma view simples sem filtro ainda e
  reportada com `view_heavy = false`, mas a observacao de filtro ausente
  aparece de qualquer forma.

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
atual. O log e desligado por padrao. Cada linha carrega o mesmo schema de
telemetria de 21 colunas do `firebird_last_query()` - incluindo
`bytes_read_estimate`, entao o log rotacionado preserva as estimativas de
transferencia por query da sessao.

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

Colunas de saida (11):

- `catalog_name`: o alias passado.
- `pool_enabled`: se o pool do ATTACH esta habilitado.
- `max_idle_size`: limite configurado da fila idle (`firebird_pool_max_size`);
  `0` = sem limite.
- `idle_timeout_ms`: expiracao idle configurada
  (`firebird_pool_idle_timeout_ms`); `0` = sem expiracao.
- `idle_connections`: conexoes paradas na fila idle agora.
- `total_created`: conexoes fisicas criadas ao longo da vida.
- `total_reused`: conexoes servidas da fila idle ao longo da vida.
- `total_discarded`: conexoes destruidas (pool desligado, limite/expiracao
  da fila idle atingidos, uma conexao de worker que nao conseguiu entrar no
  snapshot de um scan particionado, uma conexao cuja transacao nao pode
  ser restaurada depois de um scan particionado, uma conexao cuja carga do
  catalogo falhou, ou uma conexao cujo attachment foi perdido).
- `active_connections`: conexoes emprestadas agora e ainda nao devolvidas
  via Release nem descartadas.
- `last_error`: mensagem sanitizada da criacao de conexao com falha mais
  recente (`NULL` quando nenhuma); a senha e redigida antes de armazenar.
- `bytes_read_estimate`: (G5) estimativa acumulada ao longo da vida dos
  bytes de payload Firebird lidos por scans que rodaram **dentro deste
  catalogo** (caminho ATTACH). Mesma semantica dos outros contadores de
  vida: relativa ao processo, nunca reseta. Chamadas diretas a
  `firebird_scan()` **nao** entram - nao pertencem a catalogo algum.
  Definicao da estimativa: veja [`firebird_last_query()`](#firebird_last_query).

Le apenas contadores e config que o pool ja rastreia, e **nao** faz lease de
conexao - chamar nunca perturba o pool que reporta. Os valores configurados
refletem as settings lidas no momento do `ATTACH`; um `SET` posterior nao
reconfigura um pool existente (refaca o `ATTACH` para aplicar).

`last_error` so e populada por falhas de criacao de conexao **posteriores**
ao `ATTACH` (uma falha durante o proprio `ATTACH` nunca registra um
catalogo) — por exemplo, o servidor Firebird sumiu entre consultas. A
mensagem armazenada e o texto da excecao com toda ocorrencia da senha da
conexao substituida por `****` e o comprimento limitado a 500 caracteres.
`active_connections` conta leases entregues menos devolvidos; uma consulta
em voo em outra conexao mantem o valor acima de 0.

#### Para que serve

- Confirmar que o pool esta de fato reusando conexoes.
- Dimensionar `firebird_pool_max_size`.
- Verificar que um catalogo com `firebird_pool_enabled = false` nunca
  estaciona conexoes idle.
- Com `bytes_read_estimate`, acompanhar quanto payload Firebird uma sessao
  inteira de trabalho ja puxou por catalogo (antes/depois de uma extracao
  pesada).

#### Uso no dia a dia

```sql
SELECT pool_enabled, idle_connections, total_created, total_reused
FROM firebird_pool_stats('fb');

SELECT bytes_read_estimate,
       1.0 * bytes_read_estimate / 1024 / 1024 AS est_mb_total
FROM firebird_pool_stats('fb');
```

#### Limitacoes atuais

- **Por catalogo, so alias**: passa-se um alias do ATTACH; nao ha forma sem
  argumento que liste todos os catalogos anexados.
- **Sem historico de erros**: apenas o erro de conexao *mais recente* e
  mantido (sem lista com data/hora de falhas passadas).

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
| `int64_numeric_widenable` | `NUMERIC`/`DECIMAL` com escala e backing int64; a faixa escalada fisica do int64 excede o dominio de `DECIMAL(18,s)` — passe `numeric_widen_int64=true` (parametro do `firebird_scan` ou opcao do ATTACH) para projetar `DECIMAL(38,s)` sem perda |
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
  com escala 0 (todos projetados como HUGEINT). `int64_numeric_widenable`
  cobre os numericos escalados com backing int64 (precisao 10-18, escala
  diferente de 0); numericos escalados mais estreitos (precisao 1-9,
  backing SMALLINT/INTEGER) sempre cabem em seu `DECIMAL` DuckDB e nao sao
  sinalizados.

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

Colunas de saida (18):

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
| `client_version` | VARCHAR | Versao da biblioteca cliente Firebird carregada, ex.: `'5.0.3.1683'`; `NULL` quando a biblioteca nao informa |
| `inline_blob_size` | INTEGER | Limite de blob inline deste attach em bytes (o Firebird conta cada segmento com 2 bytes de tamanho): blobs dentro dele vem junto com a linha (`0` = inline desligado); `NULL` quando o cliente ou o servidor e anterior ao 5.0.3, e em attaches locais (XNET) e embedded, onde o servidor nao envia blobs inline. Veja `SET firebird_max_inline_blob_size` |
| `network_protocol` | VARCHAR | Transporte deste attach: `'TCPv4'`, `'TCPv6'`, `'XNET'` (local no Windows), `'WNET'` (named pipes do Firebird 3/4); `NULL` quando embedded |

Avisos possiveis:

| Codigo | Condicao de disparo |
| --- | --- |
| `oit_gap_high` | `oit_gap > 1.000.000` |
| `oat_gap_high` | `oat_gap > 1.000.000` |
| `sweep_disabled` | `sweep_interval = 0` |
| `forced_writes_off` | `forced_writes = false` |
| `charset_none` | `default_charset = 'NONE'` |
| `mon_unavailable` | Falha real na consulta de monitoramento |
| `inline_blobs_unavailable` | Um attach TCP a um servidor 5.0.3+ por uma biblioteca cliente anterior ao 5.0.3, entao os blobs nao vem inline (carregue um `fbclient` 5.0.3+) |

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

### `SET firebird_unpushed_mode = 'silent' | 'warn' | 'error'`

#### O que faz e como funciona

Controla o que acontece quando um scan Firebird mantem filtros no DuckDB
que **nao foram empurrados** ao Firebird - o mesmo sinal que a telemetria
mostra em `not_pushed_reasons` (`NONE_CHARSET`, `UNSUPPORTED_OP`,
`ROWID_OR_INVALID_COLUMN`, `UNSUPPORTED_PROJECTION_MAPPING`).

- `silent` (padrao) - comportamento atual: so telemetria, sem custo extra.
- `warn` - um aviso por scan pelo canal nativo de warnings do DuckDB (o
  CLI imprime no console; hosts embarcados recebem pelo subsystem de
  logging, e `warnings_as_errors` promove o aviso a excecao).
- `error` - o scan falha com Invalid Input Error listando a quantidade de
  filtros residuais, os codigos presentes e o remedio de cada um. A
  checagem roda uma vez por scan, antes de buscar qualquer linha, entao
  armar antes de uma carga longa falha rapido.

Padrao:

```sql
SET firebird_unpushed_mode = 'silent';
```

Valor desconhecido (`'warnn'`, ...) e rejeitado com Invalid Input Error
assim que um scan tiver filtros residuais a reportar - um typo nunca
desliga silenciosamente uma guarda que voce pediu.

#### Para que serve

- Falhar rapido em carga longa (COPY / CREATE TABLE AS) quando o pushdown
  esperado nao aconteceu, em vez de descobrir depois pelos numeros.
- Auditoria de performance: levantar warnings em sessoes de analise.

#### Uso no dia a dia

```sql
SET firebird_unpushed_mode = 'error';
COPY (SELECT * FROM fb.main.TABELA_GRANDE) TO 'carga.parquet';
-- falha aqui se algum filtro ficou residual, com motivo + remedio

SET firebird_unpushed_mode = 'silent';   -- de volta ao padrao na sessao
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

### `SET firebird_transaction_snapshot = true|false`

Desligado por padrao. Ligado, todos os scans de um `ATTACH` Firebird numa
transacao DuckDB leem um unico snapshot do Firebird (Firebird 4+). A opcao
`transaction_snapshot` do `ATTACH` tem precedencia por catalogo. Veja "Um
snapshot por transacao DuckDB (`transaction_snapshot`)".

### `SET firebird_transaction_snapshot_idle_timeout = SEGUNDOS`

Padrao `60`, `0` = sem limite. Com o snapshot de transacao ligado, o
servidor libera o snapshot de uma transacao DuckDB quando passam esses
segundos entre o fim de um statement e o proximo scan do `ATTACH` na
transacao; esse scan entao falha. Veja "Um snapshot por transacao DuckDB
(`transaction_snapshot`)".

### `SET firebird_dummy_packet_interval = SEGUNDOS`

#### O que faz e como funciona

Keepalive para fetches longos. Acima de `0`, **toda** conexao aberta pela
extensao - conexoes por chamada do `firebird_scan()` e conexoes de
`ATTACH`, do pool ou novas - envia `isc_dpb_dummy_packet_interval` no DPB
do attach, pedindo ao Firebird para emitir um pacote dummy no fio a cada
`SEGUNDOS`, de modo que uma conexao derrubada em silencio por NAT /
firewall de estado seja detectada durante o fetch, em vez de aparecer
depois como cursor morto (`-504 ... cursor lost`) ou conexao encerrada
(`-902`).

- **Unidade: segundos** - a mesma unidade de `DummyPacketInterval` no
  `firebird.conf` do servidor e da documentacao do protocolo de rede do
  Firebird.
- **Padrao `0` = desligado**: o item do DPB nao e enviado e o attach fica
  byte a byte identico as versoes anteriores.
- Lido no bind do `firebird_scan()` e no `ATTACH`. Para um attach ja
  existente, um `SET` posterior nao se aplica - faca `DETACH` + `ATTACH`
  de novo (o pool cria conexoes a partir do valor capturado no attach).
- Valores negativos (ou acima do limite de 32 bits do DPB) sao rejeitados
  com Binder Exception que cita a unidade:
  "firebird_dummy_packet_interval must be >= 0 (0 = disabled), got -5 (unit: seconds)".

#### Para que serve

- Extracoes longas via WAN, onde NAT/firewall derruba conexoes ociosas
  no meio do fetch (sintomas classicos: `-504` cursor lost aos ~7000s,
  `-902` em seria historica).
- Diagnostico: se o erro de fetch passa a ocorrer com keepalive ligado,
  o problema nao e timeout de ociosidade.

#### Uso no dia a dia

```sql
SET firebird_dummy_packet_interval = 60;   -- segundos
ATTACH 'firebird://srv:/data/erp.fdb' AS erp (TYPE firebird);
COPY (SELECT * FROM erp.main.TABELA_GRANDE) TO 'carga.parquet';

SET firebird_dummy_packet_interval = 0;    -- de volta ao padrao na sessao
```

Se mesmo assim um fetch morrer no meio, o scanner levanta `IO Error` com
contexto acionavel (tabela, linhas ja lidas pelo worker, esta setting como
remedio e ponteiro para `firebird_last_query()`) - veja a secao de
troubleshooting em `docs/en/observability.md`. Ressalva honesta: o valor
de DPB por anexacao depende do servidor honra-lo. A issue do Firebird
[#8266](https://github.com/FirebirdSQL/firebird/issues/8266) reporta que
em algumas versoes o valor e parseado mas nao aplicado, cabendo ao
`DummyPacketInterval` do servidor agendar os pacotes dummy. Enviar o DPB
segue sendo o pedido correto do lado cliente e e inofensivo em servidores
que o ignoram; o contexto no erro de fetch (abaixo) e a metade confiavel
do diagnostico.

### `SET firebird_max_inline_blob_size = BYTES`

#### O que faz e como funciona

Blobs inline (Firebird 5.0.3+). Quando a biblioteca cliente carregada e o
servidor sao 5.0.3 ou mais novos, o Firebird envia blobs ate esse tamanho
junto com a linha que os referencia. Ler esses blobs nao exige mais idas
e voltas extras, o que e varias vezes mais rapido em tabelas com muitos
blobs pequenos (cerca de 12x para 200.000 blobs de texto pequenos via TCP
na nossa medicao). A extensao envia o valor como
`isc_dpb_max_inline_blob_size` em toda conexao que abre.

- **Padrao `-1`**: nada e enviado e vale o padrao do proprio cliente,
  65.535 bytes num cliente 5.0.3+. **Ou seja, o inline ja vem ligado por
  padrao** em attaches TCP sempre que cliente e servidor sao 5.0.3+.
- `0` desliga o inline; `1..65535` define o limite. Valores fora de
  `-1..65535` sao rejeitados (o Firebird limita o tamanho a 65.535).
- Lido no bind do `firebird_scan()` e no `ATTACH`, como
  `firebird_dummy_packet_interval`. Para um attach ja existente, um `SET`
  posterior nao se aplica.
- Clientes e servidores mais antigos ignoram o ajuste.
- **Para ver o que esta valendo**, use `firebird_health(alias)`:
  `inline_blob_size` mostra o limite efetivo, ou `NULL` quando um dos
  lados e anterior ao 5.0.3 ou o attach e local (XNET) ou embedded: o
  servidor so envia blobs inline via TCP (veja `network_protocol`). O aviso
  `inline_blobs_unavailable` aponta o caso que tem conserto: servidor 5.0.3+
  acessado via TCP por um `fbclient` mais antigo.

```sql
ATTACH 'firebird://srv:/data/erp.fdb' AS erp (TYPE firebird);
SELECT client_version, engine_version, inline_blob_size, warnings
FROM firebird_health('erp');

SET firebird_max_inline_blob_size = 0;   -- desliga para novas conexoes
```

## Notas de mapeamento de tipos

### Politica de tratamento de tipos

A regra que esta extensao segue para todo tipo Firebird que ela expoe: se
o DuckDB tem um equivalente sem perda, usa esse equivalente; se nao tem,
expoe o valor como `VARCHAR` sem perda (nunca truncando para caber) ou
falha explicitamente (nunca substituindo silenciosamente por um valor
errado). Nada e truncado, arredondado ou retornado incorreto sem um erro
visivel ou uma ressalva ja documentada. Isso e uma descricao da politica
que este codigo ja segue para seus casos de tipo dificil existentes —
`DECFLOAT` como `VARCHAR` abaixo, os modos explicitos de `none_encoding`
de `CHARACTER SET NONE` (veja o guia de uso) — nao a afirmacao de um novo
mecanismo automatico que a verifica em todo lugar.

Uma excecao conhecida e rastreada a essa politica existe hoje:

- `TIME WITH TIME ZONE` atualmente descarta a zona/offset original na
  leitura; ja sinalizado pelo finding `time_tz` de `firebird_type_audit`
  acima.

(Issue #35 — BLOB multi-segmento truncado por um defeito no loop de
segmentos de `ReadBlob` — foi corrigida; `ReadBlob` agora continua lendo
segmentos ate `isc_segstr_eof`, o unico sinal real de fim de BLOB, em
vez de parar erroneamente no primeiro `rc == 0`.)

### Servidores com `DataTypeCompatibility`

Um servidor Firebird 4+ pode descrever tipos novos aos clientes na forma
legada: `DataTypeCompatibility = 3.0` (no `firebird.conf` ou por banco no
`databases.conf`) envia `INT128` como `BIGINT`, `DECFLOAT` como
`DOUBLE PRECISION` e os tipos `WITH TIME ZONE` sem a zona; `2.5` tambem
envia `BOOLEAN` como `CHAR(5)`. A extensao obtem os tipos das colunas dos
metadados declarados (`RDB$FIELDS`), entao pede os tipos nativos em toda
conexao (`isc_dpb_set_bind` com regras `... TO NATIVE`). Isso sobrepoe a
configuracao do servidor apenas nas conexoes da propria extensao; outras
aplicacoes continuam recebendo a forma legada. O Firebird 3 nao tem essa
configuracao e ignora o pedido.

### `NUMERIC(18,s)` / `DECIMAL(18,s)` — o dominio do int64 escalado

O Firebird armazena `NUMERIC`/`DECIMAL` de dialect 3 com precisao 10-18
como um **inteiro escalado de 64 bits**. Esse int64 fisico carrega valores
ate +/- (2^63-1) escalados, enquanto a projecao default do DuckDB para
essas colunas, `DECIMAL(18,s)`, termina em 10^18-1 escalado — uma ordem de
grandeza mais estreita. Valores acima do teto do DuckDB sao escritos sem
alteracao no vector `DECIMAL(18,s)` (fisicamente int64), e entao:

- o valor **e exibido** com mais digitos de precisao do que o tipo
  declarado permite (o dominio do tipo e violado silenciosamente);
- aritmetica inocente **falha ruidosamente** nos valores extremos — por
  exemplo, o sentinela classico `NUMERIC(18,6) = -9223372036854.775808`
  (valor escalado `INT64_MIN`) levanta `Out of Range Error: Overflow in
  multiplication of DECIMAL(18)` para `col * 2` e `Overflow in negation`
  para `abs(col)`;
- consumidores que confiam na precisao `DECIMAL(18,s)` declarada (models
  dbt, ferramentas BI, alvos de CTAS) tratam mal as linhas fora do dominio.

`numeric_widen_int64=true` — parametro nomeado do `firebird_scan` ou
opcao do ATTACH — projeta essas colunas como **`DECIMAL(38,s)`**, que tem
backing hugeint e e sem perda para toda a faixa escalada do int64:

```sql
SELECT N18_6 FROM firebird_scan('C:/dados/erp.fdb', 'LEDGER',
                                numeric_widen_int64=true)
WHERE N18_6 <= -9223372036854.775808;

ATTACH 'C:/dados/erp.fdb' AS erp
    (TYPE firebird, numeric_widen_int64 true);
```

Escopo e comportamento:

- Aplica-se **apenas** a numericos escalados com backing int64 (precisao
  10-18, escala diferente de 0). `NUMERIC(18,0)` continua `BIGINT`;
  numericos escalados mais estreitos (precisao 1-9) ja cabem em seus
  `DECIMAL(4,s)`/`DECIMAL(9,s)`; numericos com backing INT128 (precisao
  19+) ja eram `DECIMAL(38,s)`.
- O **tipo declarado da coluna** e o **vector de fetch** mudam juntos (o
  fetch faz a extensao de sinal do int64 escalado para o vector hugeint),
  inclusive pela reconciliacao de tipos de views.
- O default e `false`: sem a flag, o mapeamento e o fetch `DECIMAL(18,s)`
  sao byte a byte o comportamento historico.
- O `firebird_type_audit` sinaliza cada coluna afetada por esta opcao com
  o finding estavel `int64_numeric_widenable` (veja acima), permitindo
  dimensionar o impacto num schema real antes de ativar a flag.

### Tratamento de filtros de texto `CHARACTER SET NONE`

Por padrao, filtros de texto que tocam coluna CHAR/VARCHAR (ou BLOB de
texto) `CHARACTER SET NONE` nao sao empurrados ao Firebird, exceto quando
`none_encoding='strict'`: literais DuckDB sao UTF-8 enquanto o servidor
armazena e compara bytes brutos, entao um literal UTF-8 empurrado poderia
silenciosamente nao encontrar linhas (`'São Paulo'` e `53 E3 6F ...` num
disco Windows-1252 mas `53 C3 A3 6F ...` em UTF-8). O predicado e aplicado
pelo DuckDB apos a transcodificacao, entao o resultado fica correto nos
dois caminhos.

`none_pushdown=true` (parametro nomeado do `firebird_scan` ou opcao do
ATTACH) habilita o pushdown de **`=` e `IN (constantes)`** sobre colunas
**CHAR/VARCHAR** NONE quando `none_encoding` e `win1252` ou
`iso8859_1`/`latin1`:

```sql
SELECT *
FROM firebird_scan('C:/legado/erp.fdb', 'CLIENTES',
                   none_pushdown=true)
WHERE CITY = 'São Paulo';

ATTACH 'C:/legado/erp.fdb' AS erp
    (TYPE firebird, none_encoding 'win1252', none_pushdown true);

SELECT COUNT(*) FROM erp.main.CLIENTES
WHERE CITY IN ('São Paulo', 'Resende');
```

Por que e sem perda:

- O Firebird compara valores `CHARACTER SET NONE` **byte a byte** (semantica
  binaria, sem collation). Cada literal UTF-8 e re-codificado para os bytes
  brutos do encoding alvo — identicos aos que um escritor Windows-1252 /
  Latin-1 gravou — entao uma linha casa exatamente quando seu valor
  transcodificado e igual ao literal.
- O SQL remoto carrega esses bytes atras de um introduzidor de charset
  (`"CITY" = _WIN1252 '...'` / `_ISO8859_1 '...'`). O introduzidor e
  obrigatorio: sem ele, o `isc_dsql_prepare` valida os literais do
  statement contra o charset de conexao UTF-8 e falha com
  "Malformed string".
- A codificacao WIN1252 mapeia `codepoint < 0x80` e `0xA0..0xFF` direto para
  o byte identico, mais a tabela fixa do Windows-1252 para os 27 codepoints
  alocados em `0x80–0x9F` (Euro, aspas curvas, tracos, ...). ISO-8859-1
  mapeia `codepoint <= 0xFF` direto para o byte. Sem dependencias novas.

Fallback conservador e escopo:

- **Qualquer literal nao-codificavel** (ex. `'€'` em `iso8859_1`, cirilico
  em `win1252`) deixa o filtro inteiro no DuckDB — reaplicado acima do scan —
  e registra a razao estavel `NONE_CHARSET`, entao
  `firebird_unpushed_mode='warn' | 'error'` reage exatamente como nos outros
  casos barrados.
- **Escopo e so `=` e `IN`.** Comparacoes de intervalo nunca sao empurradas
  por aqui (a ordem de bytes num encoding de um byte nao coincide sempre com
  a ordem de codepoints Unicode), e `NOT IN` / `LIKE` mantem o gating
  NONE_CHARSET existente. BLOBs de texto NONE tambem ficam de fora (o
  Firebird nao avalia `=` / IN sobre valores BLOB).
- Com `none_encoding='strict'` ou `'blob'` a flag nao tem **efeito algum**:
  dados strict sao UTF-8 valido e ja empurram; colunas blob sao `BLOB` do
  DuckDB. Combinar nao e erro.

Nota de telemetria: o fragmento empurrado (e o `remote_sql` exibido por
`firebird_last_query()` / `firebird_query_log()` /
`firebird_explain_pushdown()`) contem intencionalmente os bytes
re-codificados, que nao sao UTF-8 valido. O DuckDB valida valores
`VARCHAR`, entao essas superficies exibem cada byte desse escapado como
`\xNN` — ex. `"CITY" = _WIN1252 'S\xE3o Paulo'`. O SQL de fato enviado ao
Firebird carrega o byte real (`0xE3`).

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
