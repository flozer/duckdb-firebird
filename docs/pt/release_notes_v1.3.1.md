# Notas de release — v1.3.1

Patch release que completa a compatibilidade com DuckDB v2.0. Nenhuma
funcao SQL ou parametro novo; comportamento default inalterado.

## Correcoes

### Pushdown e observabilidade de `NOT IN` no DuckDB v2.0

O DuckDB v1.5 apresenta `NOT IN` a extensao como `COMPARE_NOT_IN`. O
DuckDB v2.0 normaliza o mesmo predicado como `OPERATOR_NOT` envolvendo
`COMPARE_IN`. O scanner agora aceita as duas representacoes.

Isso fecha as cinco falhas de teste no v2.0 causadas por essa mudanca de
forma da expressao e restaura:

- pushdown remoto de `NOT IN`, incluindo o caminho VARCHAR lossless de
  DECFLOAT;
- telemetria de filtro residual e `NONE_CHARSET` para colunas NONE com
  gate;
- warnings/erros do `firebird_unpushed_mode` para esses filtros residuais;
- relatorios consistentes do `firebird_explain_pushdown` nas duas linhas
  de API.

### Suite compartilhada entre versoes

Cinco assercoes usavam a grafia lambda antiga. Agora usam a sintaxe
explicita `lambda`, aceita tanto no v1.5.6 quanto no v2.0. A falha de
telemetria antes atribuida ao tratamento de contexto do sqllogictest nao
se reproduziu apos essa correcao; nenhuma mudanca de estado do produto foi
necessaria.

A matriz local de build nao mantem mais uma lista de deltas de teste
tolerados no DuckDB v2.0.

### Assets de release obrigatorios

Todos os quatro jobs de assets agora sao obrigatorios. Falha em qualquer
pacote DuckDB v2.0 falha o workflow, sem `continue-on-error`.

## Validacao

- DuckDB v1.5.6: **26/26 arquivos de teste, 1.082 assercoes**, Windows,
  Firebird 5.0.3.
- DuckDB v2.0.0: **26/26 arquivos de teste, 1.082 assercoes**, Windows,
  Firebird 5.0.3.
- O teste separado de type-audit FB4 ficou skip local por ausencia de
  `FIREBIRD_BIZ4_DB`; ele e coberto pelas fixtures FB4/FB5 da CI.
- Smoke EMPLOYEE passou nas duas linhas: ATTACH nativo, scan de CUSTOMER,
  descoberta de metadata e telemetria do `firebird_last_query()`.

## Downloads e compatibilidade

Use o pacote construido para sua linha de DuckDB:

- `duckdb-firebird-1.3.1-linux-x64.tar.gz` e
  `duckdb-firebird-1.3.1-windows-x64.zip`: DuckDB v1.5.6+.
- `duckdb-firebird-1.3.1-linux-x64-duckdb2.tar.gz` e
  `duckdb-firebird-1.3.1-windows-x64-duckdb2.zip`: DuckDB v2.0+.

O catalogo community permanece independente desta release do repositorio.
