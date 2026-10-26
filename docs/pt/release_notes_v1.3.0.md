# Notas de release — v1.3.0

Release de compatibilidade: **suporte ao DuckDB v2.0** ("Cyanoptera",
lançada em 21/10/2026), junto com suporte continuado a **DuckDB
v1.5.6+**. Nenhuma funcao SQL nova; a superficie de features da extensao
permanece a da v1.2.x.

## Mudanca na politica de versoes

As versoes de DuckDB suportadas passam a ser **v1.5.6+ e v2.0+** (v1.5.3
e 1.5.x anteriores saem da matriz de testes). O mesmo fonte compila
contra as duas geracoes de API atraves de um gate de compilacao
(`FIREBIRD_DUCKDB_V2`), selecionado automaticamente a partir da versao
major do build de DuckDB.

## O que entrou no suporte ao DuckDB v2.0

- **Declaracoes de table functions**: o v2.0 substituiu o mapa
  `named_parameters` por declaracoes `FunctionSignature` estilo Python —
  parametros posicionais via `AddPositionalOnly`, nomeados via
  `AddKeywordOnly`. Nomes, parametros, tipos e ordem das funcoes
  permanecem inalterados; o `duckdb_functions()` reporta as mesmas 19
  funcoes com os mesmos parametros documentados nas duas geracoes de API.
- **Expressoes Bound encapsuladas**: o v2.0 tornou privados os membros de
  `Bound*Expression`; o walker de reconciliacao de views e o tradutor de
  filtros agora usam os accessors do v2.0.
- **Flags de build de extensao**: o v2.0 aposentou
  `ENABLE_EXTENSION_AUTOINSTALL`/`AUTOLOADING`; o gate no CMake cobre as
  duas geracoes.
- **Fix de runtime descoberto na validacao**: o DuckDB v2.0 dispara
  workers de particao em rajada, e o provider Windows local (XPS) do
  Firebird derruba handshakes de attach simultaneos de forma intermitente
  (`-901`). Os attaches agora sao serializados por processo (one-shot,
  barato — custo nao mensuravel).
- **`supports_pushdown_type`**: deliberadamente NAO ficou NONE-text-aware
  sob `none_pushdown=true` — forcar literais nao-codificaveis para o
  `TableFilterSet` do planner produzia push cru (resultados errados) no
  v2.0. O hook de filtros complexos faz a transcodificacao de `=`/`IN`
  nas duas geracoes de API sem ele.

## Deltas comportamentais na release v1.3.0 (resolvidos na v1.3.1)

- **Visibilidade de residual para filtros de texto em colunas NONE**: o
  planner do v2.0 ergue filtros de texto nao-empurrados acima do scan sem
  oferta-los a extensao, entao `residual_filters` / `not_pushed_reasons`
  ficam vazios para esses casos (no v1.5 registravam `NONE_CHARSET`) e o
  `firebird_unpushed_mode='error'` nao dispara para eles. Os resultados
  sao identicos (verificado por testes de identidade de contagem); o
  delta de visibilidade esta registrado para follow-up contra as APIs do
  planner v2.0.
- **`firebird_last_query()` entre statements no harness sqllogictest**:
  le vazio no tratamento de contexto do harness do v2.0 (grava
  normalmente via CLI/embedded — verificado). O comportamento do produto
  por sessao nao mudou.

## Follow-up de compatibilidade pos-release (resolvido na v1.3.1)

A release v1.3.1 fecha os deltas de teste acima:

- O DuckDB v2.0 normaliza `NOT IN` como `OPERATOR_NOT` envolvendo
  `COMPARE_IN`; o scanner agora reconhece essa forma. Pushdown remoto de
  `NOT IN`, telemetria residual em colunas NONE e guards do
  `firebird_unpushed_mode` voltam a passar.
- A falha de telemetria atribuida ao contexto do harness nao se reproduziu
  depois da correcao da sintaxe lambda restante. Telemetria entre
  statements passa sem mudanca no estado do produto.
- A validacao Windows/Firebird 5.0.3 agora e identica nas duas linhas:
  **26/26 arquivos de teste e 1.082 assercoes** no DuckDB v1.5.6 e v2.0.0.

Essas mudancas nao fazem parte da tag v1.3.0 publicada nem de seus binarios.

## Assets de release

Duas variantes de build por plataforma, uma por linha de DuckDB:

- `duckdb-firebird-<version>-linux-x64.tar.gz` /
  `...-windows-x64.zip` — construido contra **DuckDB v1.5.6** (carrega no
  v1.5.6+).
- `duckdb-firebird-<version>-linux-x64-duckdb2.tar.gz` /
  `...-windows-x64-duckdb2.zip` — construido contra **DuckDB v2.0.0**.

## Validacao

- `scripts/build_matrix.ps1 -Tags v1.5.6,v2.0.0`: v1.5.6 = 26/26 arquivos
  de teste, 1.082 assercoes; v2.0.0 = 18/26 + 8 deltas documentados, 754
  assercoes. Ambos compilam limpos do mesmo fonte.
- Bateria read-only em um restore real de ERP Firebird 5.0.3 de ~66GB
  (apenas contagens agregadas) exercitando scans, ATTACH, metadata e
  diagnosticos nas duas geracoes de API.
- As pernas de CI do DuckDB v2.0 rodam com `continue-on-error` na
  primeira rodada; falhas nelas nao bloqueiam os assets da v1.5.6.

## Notas de compatibilidade

- Nada removido ou renomeado; comportamento default inalterado; toda
  alavanca permanece opt-in. Usuarios da v1.2.x podem atualizar no lugar
  (o asset construido na v1.5.6 e o caminho de atualizacao in-place;
  usuarios do DuckDB v2.0 pegam os assets `-duckdb2`).
