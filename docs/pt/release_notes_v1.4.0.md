# Notas de versão — v1.4.0

Esta versão foi contribuída por **[@fdcastel](https://github.com/fdcastel)
(F.D.Castel)**. As quatro correções abaixo são trabalho dele, nascidas da
cooperação que ele propôs na
[#89](https://github.com/flozer/duckdb-firebird/issues/89). Nosso
agradecimento — esta versão existe graças a essa contribuição.

Nenhuma função ou parâmetro SQL novo. Uma nova coluna de observabilidade
(`snapshot_mode`) e uma mudança de comportamento nos scans particionados
(um único snapshot compartilhado), descritas abaixo.

## Mudanças

### Scans particionados leem um snapshot do Firebird (#94)

Um scan particionado (`partitions > 1`, ou particionamento automático no
caminho ATTACH) antes rodava cada partição na própria transação
read-committed, então as partições podiam ver estados commitados diferentes
da tabela — uma linha movida ou um valor transferido entre partições
durante o scan podia ser contado duas vezes ou perdido.

Agora uma conexão coordenadora inicia uma transação SNAPSHOT somente-leitura
e cada worker de partição junta-se a ela com `isc_tpb_at_snapshot_number`
(Firebird 4+), então todas as partições leem um estado consistente. No
Firebird 3 (sem números de snapshot), ou se o snapshot compartilhado não
puder ser estabelecido, o comportamento anterior é mantido exatamente. Uma
vez estabelecido o snapshot compartilhado, um worker que não consegue se
juntar a ele falha o scan com um erro claro, em vez de ler suas partições
fora do snapshot; sua conexão do pool é descartada, e o erro aparece em
`firebird_last_query()`. Scans seriais não mudam.

Uma nova coluna `snapshot_mode` (`'serial'` / `'shared'` / `'independent'`)
é anexada a `firebird_last_query()` e `firebird_query_log()` depois de
`bytes_read_estimate` (agora 20 colunas).

### Linhas fora da faixa de chave em cache não são mais ignoradas (#95)

No caminho ATTACH, os limites `MIN`/`MAX` da chave primária são sondados na
primeira vez que uma consulta referencia a tabela e então cacheados. Linhas
inseridas depois, com chave acima do `MAX` ou abaixo do `MIN` cacheados,
ficavam silenciosamente ausentes de todo scan posterior daquela tabela —
particionado ou não, inclusive os filtrados — até o banco ser re-anexado. A
primeira partição agora não tem limite inferior e a última não tem limite
superior; um scan de partição única não envia nenhum predicado de faixa.

### Tipos nativos em servidores com `DataTypeCompatibility` (#98)

Contra um servidor configurado com `DataTypeCompatibility = 3.0` ou `2.5`,
`INT128` e `BOOLEAN` levantavam um `INTERNAL Error` do DuckDB (que invalida
a instância do banco), e `TIMESTAMP`/`TIME WITH TIME ZONE` voltavam
deslocados ou corrompidos sem erro. A extensão agora pede os tipos nativos
em toda conexão (`isc_dpb_set_bind`), então `INT128`, `NUMERIC(19+)`,
`DECFLOAT`, os tipos com fuso horário e `BOOLEAN` são lidos corretamente.

### Carregamento da biblioteca cliente por caminho no Windows (#96)

Quando a biblioteca cliente é dada por caminho
(`DUCKDB_FIREBIRD_CLIENT_LIBRARY`), suas DLLs dependentes agora são
resolvidas primeiro na pasta da própria biblioteca
(`LOAD_WITH_ALTERED_SEARCH_PATH`). Um `fbclient.dll` do Firebird 3 — que
traz `msvcr100.dll`, `zlib1.dll` e ICU ao lado — antes falhava com "The
specified module could not be found" a menos que sua pasta também estivesse
no `PATH`.

## Validação

- Matriz CI completa Firebird 3/4/5 verde no DuckDB v1.5.6 e v2.0.0 para as
  quatro mudanças.
- A garantia do snapshot compartilhado é verificada sob um escritor
  concorrente (`scripts/check_shared_snapshot.sh`) em cada perna Firebird
  3/4/5, e o caminho de falha de junção é verificado
  (`scripts/check_snapshot_join_failure.sh`) no Firebird 4+.

## Downloads e compatibilidade

Use o pacote construído para a sua linha do DuckDB:

- `duckdb-firebird-1.4.0-linux-x64.tar.gz` e
  `duckdb-firebird-1.4.0-windows-x64.zip`: DuckDB v1.5.6+.
- `duckdb-firebird-1.4.0-linux-x64-duckdb2.tar.gz` e
  `duckdb-firebird-1.4.0-windows-x64-duckdb2.zip`: DuckDB v2.0+.

O catálogo da comunidade permanece independente do release deste repositório.
