# Notas de versão — v1.4.1

Esta versão foi contribuída por **[@fdcastel](https://github.com/fdcastel)
(F.D.Castel)**. As seis mudanças abaixo são trabalho dele, continuando a
cooperação que ele propôs na
[#89](https://github.com/flozer/duckdb-firebird/issues/89). Nosso
agradecimento — esta versão existe graças a essa contribuição.

Três adições opt-in (`partition_by = 'dbkey'`, ajuste de blobs inline e um
snapshot de transação) e três correções no caminho ATTACH, todas somente
leitura e retrocompatíveis. As consultas existentes não mudam, a menos que
ativem as novas opções.

## Adicionado

### `partition_by = 'pk' | 'dbkey'` (#101)

`firebird_scan()` ganha o parâmetro `partition_by`. O padrão `'pk'` mantém
o comportamento anterior (faixas sobre a chave primária). `'dbkey'`
(`partitions >= 2` explícito, tabelas base, Firebird 4 e 5) divide a tabela
em faixas de `RDB$DB_KEY` sobre o espaço físico de registros (`RDB$PAGES`,
`MAKE_DBKEY`), lidas dentro do snapshot compartilhado: sem sondagem
`MIN`/`MAX`, uma divisão que não depende de como as chaves estão
distribuídas (balanceada até uma pointer page), e tabelas **sem chave
primária utilizável podem ser particionadas**. Tudo que `'dbkey'` não
consegue tratar — inclusive um scan cujo snapshot compartilhado não pôde ser
iniciado — cai de volta para `'pk'`. Com um filtro empurrado que usa índice,
cada partição DB_KEY repete o index scan, então `'pk'` é a melhor escolha
ali.

Uma nova coluna `partition_method` (`'none'` / `'pk'` / `'dbkey'`) é anexada
a `firebird_last_query()` e `firebird_query_log()` após `snapshot_mode`
(21 colunas).

### Ajuste e relato de blobs inline do Firebird 5.0.3+ (#107)

`SET firebird_max_inline_blob_size = BYTES` ajusta ou desativa os blobs
inline do Firebird 5.0.3+ (`isc_dpb_max_inline_blob_size`, `-1` = padrão do
cliente, `0` = desligado, máx 65535), lido no bind do `firebird_scan()` e no
`ATTACH` como `firebird_dummy_packet_interval`. O inline já vem ligado por
padrão em conexões TCP quando a biblioteca cliente e o servidor são ambos
5.0.3+ (o servidor não faz inline de blobs sobre XNET ou embedded).

`firebird_health()` ganha três colunas — `client_version`,
`inline_blob_size` e `network_protocol` (18 colunas) — e um aviso
`inline_blobs_unavailable` para um servidor 5.0.3+ alcançado via TCP por uma
biblioteca cliente mais antiga.

### Snapshot de transação no caminho ATTACH (opt-in, desligado por padrão) (#109)

`SET firebird_transaction_snapshot = true`, ou a opção de ATTACH
`transaction_snapshot`, faz cada scan do ATTACH numa transação DuckDB —
scans seriais inclusive — ler um único snapshot do Firebird (Firebird 4+):
um por statement em autocommit, um por `BEGIN … COMMIT`. Um coordenador do
pool do ATTACH o segura até commit ou rollback; os scans relatam
`snapshot_mode = 'transaction'`. O Firebird 3 mantém o comportamento
por-scan.

`SET firebird_transaction_snapshot_idle_timeout = SECONDS` (padrão 60,
`0` = nenhum) faz o servidor liberar o snapshot (`SET SESSION IDLE TIMEOUT`)
quando nenhum scan do ATTACH segue o fim de um statement a tempo; o próximo
scan então falha com uma mensagem nomeando a setting. Uma conexão que entra
verifica que o coordenador ainda segura o número do snapshot antes de
confiar nele, então um número reutilizado de um banco reaberto nunca é lido
no snapshot de outra sessão.

## Corrigido

### Uma conexão morta do pool nunca é re-estacionada (#106, #103)

Uma conexão do pool cujo attachment havia sumido (morto no servidor ou pelo
idle timeout, ou falha de rede) voltava ao pool após uma consulta falha,
então toda consulta posterior no ATTACH falhava do mesmo jeito até o
re-attach. Isso acontecia após um `firebird_health()` ou outra função de
catálogo falha, um scan falho e uma sondagem de PK falha — que também
lembrava "sem chave primária utilizável", então a tabela nunca era
particionada de novo. Uma conexão agora se marca quebrada num erro de
conexão perdida e o pool a descarta em vez de estacioná-la; a sondagem de PK
não lembra um resultado de uma conexão quebrada, e uma lista de tabelas
carregada numa delas não é cacheada. Um catalog-load falho também passa a
ser contado como descartado, então não deixa mais
`firebird_pool_stats().active_connections` uma unidade alto demais.

### Erros de alias de ATTACH nomeiam a função chamada (#102)

As funções que recebem um alias de ATTACH (`firebird_health()`,
`firebird_pool_stats()`, `firebird_profile_table()`,
`firebird_index_profile()`, `firebird_generators()` e as outras funções de
metadados de catálogo) relatavam um alias desconhecido ou não-Firebird sob o
nome `firebird_generate_dbt_sources`. O erro agora nomeia a função que foi
de fato chamada.

## Limitação conhecida

As issues abertas do Firebird
[#9227](https://github.com/FirebirdSQL/firebird/issues/9227) e
[#9301](https://github.com/FirebirdSQL/firebird/issues/9301) ainda podem
deixar um scan particionado — e o snapshot de transação — misturar dois
estados commitados no Firebird 4/5 sob uma corrida estreita. O comportamento
é somente leitura; nada é escrito na origem. Esta nota será atualizada
quando as correções upstream forem lançadas.

## Validação

- Matriz completa de CI Firebird 3/4/5 verde no DuckDB v1.5.6 e v2.0.0, com
  um leg dedicado DuckDB v2.0.0 × Firebird 5 para que os testes e scripts
  exclusivos de Firebird 4+ também rodem no build DuckDB v2.0.
- As garantias de snapshot compartilhado e de snapshot de transação são
  checadas sob um gravador concorrente (`scripts/check_shared_snapshot.sh`)
  em cada leg Firebird 3/4/5, com um controle opção-desligada que precisa
  rasgar no Firebird 4/5; `scripts/check_dead_pooled_connection.sh` mata o
  coordenador e as conexões estacionadas e faz outra sessão reutilizar um
  número de snapshot perdido.

## Downloads e compatibilidade

Use o pacote compilado para a sua linha de DuckDB:

- `duckdb-firebird-1.4.1-linux-x64.tar.gz` e
  `duckdb-firebird-1.4.1-windows-x64.zip`: DuckDB v1.5.6+.
- `duckdb-firebird-1.4.1-linux-x64-duckdb2.tar.gz` e
  `duckdb-firebird-1.4.1-windows-x64-duckdb2.zip`: DuckDB v2.0+.

O catálogo da comunidade permanece independente do release deste
repositório.
