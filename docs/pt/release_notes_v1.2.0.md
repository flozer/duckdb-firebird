# Notas de release — v1.2.0

Release de features que fecha a fila G: diagnosticos, pushdown e
estabilidade originados de feedback real de producao (um lakehouse sobre
ERP brasileiro legado em `CHARACTER SET NONE` lido por WAN lenta). Nenhuma
funcao SQL nova — comportamento mais rico na superficie existente, duas
opcoes novas de `firebird_scan`/ATTACH, um setting novo de sessao e
colunas de telemetria aditivas. Publicada a partir do `main`; a
atualizacao do catalogo DuckDB Community e um passo separado, com
autorizacao explicita.

## Destaques

### `none_pushdown` — `=` / `IN` em colunas CHARACTER SET NONE (G2)

O maior destravamento. Filtros de texto em colunas NONE nunca empurravam
(um `IN` de 1000 ids significava full scan no servidor). Com
`firebird_scan(..., none_encoding='win1252', none_pushdown=true)`
(tambem opcao de ATTACH), literais constantes de `=` e `IN` sao
re-encodeados para os bytes de armazenamento atras de um introduzidor de
charset `_WIN1252`/`_ISO8859_1` — o Firebird compara colunas NONE byte a
byte, entao o casamento e lossless para literais codificaveis. Literais
nao-codificaveis (ex. `€` em latin1) caem de volta para o DuckDB
automaticamente com a razao estavel `NONE_CHARSET`. Escopo: `=`/`IN` em
CHAR/VARCHAR apenas; range, `LIKE` e `NOT IN` permanecem no cliente.
Validado em base real de ERP: o SQL enviado carrega
`ANOMES IN (_WIN1252 '202501', ...)` de ponta a ponta.

### `firebird_unpushed_mode` — fim do full scan silencioso (G1)

`SET firebird_unpushed_mode = 'error'` falha o scan no momento em que ele
mantem filtros no DuckDB, nomeando cada razo residual com o remedio;
`'warn'` emite warning nativo do DuckDB; `'silent'` (default) preserva o
comportamento historico. Transforma horas de full scan silencioso em uma
falha acionavel de segundos.

### `numeric_widen_int64` — fim da classe de overflow de NUMERIC(18,s) (G3)

O `NUMERIC(18,s)` int64-backed do Firebird carrega valores escalados ate
±9,2e18, mas o `DECIMAL(18,s)` do DuckDB para em 1e18−1 escalado: valores
grandes fazem round-trip na exibicao e depois quebram aritmetica inocente
(`Out of Range Error: Overflow in multiplication of DECIMAL(18)` — o
sentinela classico e `NUMERIC(18,6) = -9223372036854.775808`).
`numeric_widen_int64=true` (scan ou ATTACH) projeta essas colunas como
`DECIMAL(38,s)` — lossless. O `firebird_type_audit` ganhou o finding
`int64_numeric_widenable` para o tooling reagir sem a flag.

### Estabilidade de sessao: keepalive + contexto de erro de fetch (G4)

`SET firebird_dummy_packet_interval = 60` (segundos, 0 = desligado) envia
o DPB de keepalive do Firebird por conexao para sessoes longas atraves de
NAT/WAN. Quando um fetch morre (`-504` cursor lost / `-902` shutdown), o
erro agora carrega a tabela, linhas lidas ate ali, a dica de keepalive e a
referencia ao `firebird_last_query()`. Caveat honesto documentado:
versoes recentes do Firebird podem parsear mas nao agir sobre o item de
DPB (firebird#8266); o contexto de erro e a metade confiavel.

### `bytes_read_estimate` — orcamento de rede (G5)

`firebird_last_query()` e `firebird_query_log()` ganharam
`bytes_read_estimate` (linhas × soma das larguras do XSQLDA + bytes de
segmento de BLOB efetivamente lidos; uma ESTIMATIVA documentada — o
fbclient nao expoe bytes de fio), e `firebird_pool_stats()` ganhou o
total acumulado por catalogo (so scans via ATTACH). Feito para orcamento
em WAN: "este scan vai custar 47k bytes" antes de roda-lo.

### Dividas de CI/testes fechadas (continuacao da fila F)

A cobertura G esta ligada as suites canonicas: cinco arquivos de teste
novos (`firebird_unpushed_mode`, `firebird_none_pushdown`,
`firebird_numeric_widen`, `firebird_session_stability`,
`firebird_bytes_estimate`) mais as fixtures da fila F, com
`numerics.fdb` provisionado no `setup_test_firebird.sh` e nos dois
workflows Linux.

### Experimento de retomada por keyset (G6 — registrado, implementacao adiada)

Em uma base real de ERP de ~66GB (tabela de 967 mil linhas, span esparso
de 20M na PK, apenas agregados): `ORDER BY pk` e index-driven com custo
~zero (4,94s vs 5,02s) e um predicado de retomada
`WHERE pk > X ORDER BY pk` empurra e roda no mesmo tempo — retomada por
keyset apos `-504`/`-902` e viavel e barata. A implementacao vira
follow-up pos-v1.2.0 com o design destravado.

## Validacao

- Suite completa de sqllogictest verde no wiring de CI (26 arquivos) e
  localmente via `scripts/build_matrix.ps1`: **26/26 arquivos de teste,
  1.078 assercoes, PASS identico em DuckDB v1.5.3 e v1.5.6**.
- Bateria read-only em um restore real de ERP Firebird 5.0.3 de ~66GB
  (apenas metadados e contagens agregadas): `none_pushdown` empurrou
  `ANOMES IN (_WIN1252 ...)` de ponta a ponta com contagens iguais;
  `firebird_unpushed_mode='error'` falhou um `NOT IN` sobre NONE com o
  remedio completo; `numeric_widen_int64` projetou `NUMERIC(18,2)` como
  `DECIMAL(38,2)` com `SUM` limpa sobre 5.902 linhas;
  `firebird_dummy_packet_interval` armado com health integro;
  `bytes_read_estimate` deterministico (47.216 = 5.902 × 8) e acumulando
  por catalogo com pool coerente.

## Notas de compatibilidade

- Totalmente aditivo: dois parametros nomeados novos (`none_pushdown`,
  `numeric_widen_int64`), um setting novo de sessao
  (`firebird_unpushed_mode`), uma opcao nova de ATTACH
  (`firebird_dummy_packet_interval` e um setting de sessao consumido na
  criacao de conexao), cinco colunas novas de telemetria/estatisticas
  (`bytes_read_estimate` ×3 superficies, `active_connections` e
  `last_error` vieram na v1.1.0), um finding estavel novo
  (`int64_numeric_widenable`). Nada removido ou renomeado; o
  comportamento default nao muda em lugar nenhum (toda alavanca nova e
  opt-in).
- O `firebird_scan` agora expoe 13 parametros (2 posicionais + 11
  nomeados).
