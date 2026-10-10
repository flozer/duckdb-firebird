# Notas de release — v1.2.1

Patch release: um fix de ergonomia reportado por um consumidor real de
data lake. Nenhuma funcao nova; totalmente aditivo.

## Fix

### `none_encoding` / `none_pushdown` aceitos dentro da string de conexao

O opt-in `none_pushdown` da v1.2.0 so era alcancavel como parametro
nomeado do `firebird_scan` ou opcao do ATTACH — coloca-lo na string de
conexao era silenciosamente ignorado (o parser pulava a chave
desconhecida, entao a configuracao parecia aplicada e estava desligada).
Agora ambos sao parseados da propria string de conexao, nas duas formas
suportadas:

- Query de URI: `firebird://user:pass@host:3050/path?charset=UTF8&none_encoding=win1252&none_pushdown=true`
- DSN chave=valor: `database=C:/dados/empresa.fdb;user=APP_READONLY;password=secret;none_encoding=win1252;none_pushdown=true`

Booleans aceitam `true`/`false`/`1`/`0`; boolean malformado falha com erro
acionavel em vez de ser silenciosamente ignorado (`connection-string
option 'none_pushdown' expects true/false (or 1/0), got '...'`).

Precedencia: opcao explicita do ATTACH / parametro nomeado do
`firebird_scan` > string de conexao > default. `numeric_widen_int64`
permanece disponivel apenas como opcao do ATTACH / parametro nomeado
(inalterado).

O caminho do ATTACH propaga os valores da string de conexao para o
catalogo (todo scan atras do alias empurra `=`/`IN` sobre NONE), e o
`firebird_scan` direto honra a string com o parametro nomeado mantendo a
precedencia.

## Validacao

- Matriz local completa: 26/26 arquivos de teste, PASS identico em DuckDB
  v1.5.3 e v1.5.6.
- Nova cobertura de connection string em `firebird_none_pushdown.test`
  (pushdown DSN de ponta a ponta, erro de boolean malformado; a forma de
  query URI e exercitada nas pernas de CI onde o env `firebird://` e
  canonico).
- Bateria pontual read-only em um restore real de ERP Firebird 5.0.3 de
  ~66GB (apenas contagens agregadas): configuracao somente via string de
  conexao empurra `ANOMES IN (_WIN1252 ...)` de ponta a ponta com
  contagens identicas a forma por parametro nomeado.
