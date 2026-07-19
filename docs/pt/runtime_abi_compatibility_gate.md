# Runtime/ABI Compatibility Gate

Objetivo: provar que `duckdb-firebird` esta pronto como **ponte runtime**
estavel (DuckDB <-> Firebird), nao introduzir feature nova. Cobre
compatibilidade DuckDB suportada, mecanismo de carga do client-lib em
runtime, artefatos Linux/Windows, matriz DuckDB, matriz Firebird, docs de
instalacao/runtime, maturity battery read-only e lista de follow-ups nao
bloqueantes. Sem acao em `duckdb/community-extensions` ou qualquer repo
upstream nesta frente.

Pre-requisito desta gate: **Production Stability Gate fechada** — todas as
6 frentes de cobertura/CI mergeadas em main: #42/#43 (PR #45), #39 (PR
#46), #31 (PR #49), #26 (PR #50), #47 (PR #51), #48 (PR #52). Ver
`.superpowers` (memoria de sessao) / historico de PRs para o detalhe de
cada uma.

---

## 1. Compatibilidade DuckDB suportada

Verificado contra `v1.5.2`, `v1.5.3` (baseline/pin do submodulo) e
`v1.5.4`. Resultado fresco desta gate (2026-07-19), com a suite completa
de 19 arquivos `firebird_*.test` (854 assercoes) — ver Secao 4 para a
tabela e o detalhe de execucao.

**Decisao:** as tres tags compilam sem alteracao de codigo e produzem
resultado de teste identico. Nenhum patch de compatibilidade pendente.

**Gap de comunicacao (nao bloqueante):** `docs/en/guide_linux.md` e
`docs/en/guide_windows.md` mencionam apenas `v1.5.3` como a versao usada
nos passos de build, sem declarar explicitamente que `v1.5.2`/`v1.5.4`
tambem sao verificadas. Um usuario lendo só esses guias pode achar que
`v1.5.3` e a unica versao suportada. Recomendacao: adicionar uma linha nos
dois guias apontando para `docs/pt/duckdb_1_5_compatibility_plan.md` (ou
seu equivalente em `docs/en/`) como a fonte da matriz de compatibilidade
completa. Nao bloqueia esta gate — tratado como follow-up (Secao 8).

## 2. Client-lib runtime loading

Mecanismo (`src/firebird_client_loader.cpp` / `firebird_client_loader.hpp`):

- **Carga em runtime, nunca link-time**: Windows usa
  `LoadLibraryA`/`GetProcAddress`; POSIX (Linux/macOS) usa
  `dlopen(name, RTLD_NOW | RTLD_GLOBAL)`/`dlsym`, selecionado via `#ifdef
  _WIN32`.
- **Simbolos resolvidos**: apenas a API legada ISC (`isc_attach_database`,
  `isc_dsql_prepare`, `isc_dsql_execute`, `isc_get_segment`, etc. — 16
  simbolos ao todo), nunca a API OO moderna. Essa API e estavel desde o
  Firebird 1.x, entao o mesmo binario da extensao funciona contra client
  libraries de FB 2.5/3/4/5 sem exigir uma versao especifica — so exige
  que os simbolos existam.
- **Candidatos padrao por plataforma**: Windows `{fbclient.dll,
  fbclient_ms.dll}`; macOS `{libfbclient.dylib}`; Linux `{libfbclient.so.2,
  libfbclient.so}`. Nenhum path de filesystem fixo — sao nomes que o
  loader do SO resolve pelo mecanismo de busca normal (`PATH`/rpath/ld.so
  cache).
- **Override explicito**: `DUCKDB_FIREBIRD_CLIENT_LIBRARY` (env var) — se
  definido, e autoritativo; falha de carga gera erro imediato, sem
  fallback silencioso para os candidatos padrao. E o mesmo mecanismo que a
  matriz Linux CI usa para versionar corretamente o client por leg
  (#41).
- **Erro/fallback**: se nenhum candidato carrega, lanca `IOException`
  listando exatamente quais nomes foram tentados e sugerindo a variavel de
  override. Se um candidato carrega mas falta um simbolo, erro explicito
  dizendo que a lib "parece antiga demais ou nao e um build Firebird".
  Carregamento e memorizado por processo (mutex + flag estatica) — custo
  pago uma vez.

**Decisao:** mecanismo robusto, documentado, funciona atraves de versoes
de client sem exigir recompilacao, com override explicito e mensagens de
erro acionaveis. Sem gap identificado.

## 3. Artefatos Linux/Windows

| | Linux (`build-linux-fb-matrix.yml`) | Windows (`build-windows.yml`) |
|---|---|---|
| Estrategia | matriz `fb_major: [3,4,5]`, 3 jobs | job unico `windows-x64` |
| DuckDB pin | `v1.5.3` | `v1.5.3` |
| Firebird | servidor + client version-matched por leg via Docker | SDK unico pinado (`Firebird-4.0.5.3140-0-x64.zip`), sem matriz |
| Artefato | `firebird.duckdb_extension`, upload como `firebird.duckdb_extension-linux-x64` (so na leg `fb5`, `if-no-files-found: error`) | mesmo arquivo, upload como `firebird.duckdb_extension-windows-x64`, `if-no-files-found: error` |
| Suite `test/sql/firebird_*.test` | 17 arquivos incondicionais + 2 gated `fb_major != 3` | **nenhum** — so um smoke de "Inspect output"; suite completa roda apenas manualmente via `scripts/build_matrix.ps1` local |

**Decisao:** os dois pipelines produzem o artefato corretamente e falham
alto se ele nao existir (`if-no-files-found: error`). O runner Windows e
pinado em `windows-2022` (nao `windows-latest`) porque o MSVC 14.51 do
`windows-latest` derrubou um simbolo que o `fmt` empacotado pelo DuckDB
v1.5.3 precisa — nota de ABI/toolchain relevante para esta gate.

**Gap real (nao bloqueante, ja mitigado por processo manual):** o
workflow Windows nao roda nenhum arquivo `test/sql/firebird_*.test` em CI
— toda a cobertura de teste no Windows depende da execucao manual de
`scripts/build_matrix.ps1` por Fernando antes de cada release (pratica ja
estabelecida e seguida nesta propria gate, Secao 4). Como processo manual
ja cobre isso de forma confiavel a cada release, isso nao bloqueia, mas
fica registrado como follow-up de automacao (Secao 8) — rodar a suite
tambem no Windows CI removeria a dependencia de alguem lembrar de rodar
localmente.

## 4. Matriz DuckDB (v1.5.2 / v1.5.3 / v1.5.4)

Reexecutada nesta gate (2026-07-19) em worktree isolado
(`C:/tmp/fbwt-rabi`, nunca o worktree principal), com copia local de
`scripts/build_matrix.ps1` apontando `$root` para esse worktree (o script
commitado continua fixo em `d:/Dados/duckdb-firebird` por design — nao
alterado). Ambiente: `C:\fbtest\test.fdb` / `decfloat.fdb` / `none.fdb`,
SYSDBA/masterkey local.

| Versao | Commit DuckDB | Build | Testes | Assertions | Status |
|---|---|---|---|---|---|
| v1.5.2 | `8a5851971f` | ok | 19 pass / 0 env-missing | 854 | PASS |
| v1.5.3 | `14eca11bd9` | ok | 19 pass / 0 env-missing | 854 | PASS (baseline) |
| v1.5.4 | `08e34c447b` | ok | 19 pass / 0 env-missing | 854 | PASS |

Zero API drift, zero patch necessario, resultado identico nas tres
versoes. Detalhe completo (evolucao desde a verificacao de 2026-06-19:
13->19 arquivos, 605->854 assercoes) em
`docs/pt/duckdb_1_5_compatibility_plan.md`.

## 5. Matriz Firebird 3/4/5

CI real (`build-linux-fb-matrix.yml`), 3 legs, todas verdes apos as 6
frentes da Production Stability Gate:

- Fixture unica canonica (`scripts/fixture_common.sql`,
  `scripts/fixture_biz4.sql`, `scripts/fixture_none_charset.sql`,
  `scripts/fixture_decfloat.sql` — piggyback em `biz4.fdb`), zero DDL
  inline no workflow, guard (`scripts/check_no_inline_fixture_drift.sh`,
  hardened em #47) impedindo regressao futura.
- Client-lib version-matched extraido do proprio container por leg (#41),
  eliminando o mismatch client/server que motivou #40.
- `mkblob_fixture` compilado no runner, linkado por nome exato de arquivo
  (nunca `-lfbclient` generico) contra o client version-matched.
- 17 arquivos incondicionais + `firebird_type_audit_fb4`/`firebird_decfloat`
  gated `fb_major != 3` (skip por versao, nao por env ausente).

**Decisao:** matriz real, sem mascaramento de comportamento Firebird,
sem uso de `DataTypeCompatibility` como atalho. Pronta.

## 6. Docs de instalacao/runtime

- `docs/en/guide_linux.md`, `docs/en/guide_windows.md`: passos de build
  corretos e testados (usados como base do proprio CI), mas mencionam so
  `v1.5.3` — ver gap na Secao 1.
- `docs/en/function_manual.md`: cobre as funcoes relevantes para esta gate
  (`firebird_health`, `firebird_type_audit`, ATTACH) com exemplos
  corretos, usados para validar o harness da Secao 7.
- Nenhuma doc de instalacao precisou de correcao de conteudo tecnico
  (comandos/paths verificados batem com o comportamento real).

## 7. Maturity battery read-only

Harness novo: `scripts/maturity_battery.ps1`. Roda 100% orientado a
variaveis de ambiente (`FIREBIRD_MATURITY_DB`, `ISC_USER`,
`ISC_PASSWORD`, `MATURITY_DUCKDB_EXE` opcional), sem nenhum path,
usuario, senha ou DSN hardcoded no script.

O que roda:

1. `firebird_health('fb')` — diagnostico estrutural (versao, ODS,
   dialect, charset, page_size, forced_writes, sweep_interval,
   oit_gap/oat_gap, contagem de attachments, warnings). Sem dado de
   negocio.
2. Contagem de relacoes expostas via `information_schema.tables`.
   **Achado real durante a validacao**: a ponte nao distingue table vs
   view em `table_type` — tudo aparece como `'BASE TABLE'` (confirmado:
   fixture local com 9 tabelas + 5 views reportou 14 `BASE TABLE` / 0
   `VIEW`). O harness reporta contagem total, sem a quebra enganosa
   table/view.
3. `firebird_type_audit('fb')` agregado por `finding` (contagem por
   codigo, nunca nome de tabela/coluna).
4. **Checagem de auto-consistencia de BLOB/texto**: para uma amostra de
   colunas com achado `blob_text`/`none_charset` (nomes usados so
   internamente, nunca impressos), busca a MESMA linha duas vezes e
   compara `md5(valor)` entre as duas buscas — prova determinismo sem
   nunca imprimir o conteudo. Reporta apenas: quantas colunas amostradas,
   quantas bateram entre as duas buscas, quantas divergiram (sinal de
   nao-determinismo a investigar), tamanhos min/max/media.

**Nunca aparece no relatorio**: valores de linha, conteudo BLOB/texto,
nomes de tabela/coluna, connection string completa, path real de
producao. Em caso de erro, o detalhe cru vai para um log local-only
(`%TEMP%\maturity_battery_diag.log`), nunca para o relatorio
compartilhavel.

**Validacao feita nesta gate**: dry-run completo contra a fixture local
(`C:\fbtest\test.fdb`) — 14 relacoes, 2 `blob_text` + 1 `none_charset`,
3/3 colunas amostradas auto-consistentes, zero erros, elapsed ~1.4s.
Durante essa validacao, dois bugs reais do proprio harness foram achados
e corrigidos: (1) DSN de teste no formato errado (o ATTACH desta
extensao usa o mesmo parser de `firebird_scan` — path/URL direto +
`ISC_USER`/`ISC_PASSWORD`, nao `"database=...;user=...;password=..."`
embutido); (2) um gotcha real do PowerShell — checar `.__error` num
array de 2+ elementos faz *member enumeration* e retorna um array de
`$null`, que e truthy mesmo vazio, disparando falso-positivo de erro;
corrigido checando o TIPO do retorno (`-is [hashtable]`) em vez da
truthiness da propriedade.

**Pendente:** este harness ainda **nao rodou contra o ambiente de
producao real** — precisa do Fernando definir `FIREBIRD_MATURITY_DB` /
`ISC_USER` / `ISC_PASSWORD` (e opcionalmente `MATURITY_DUCKDB_EXE`
apontando pra um build com a extensao) e executar:

```powershell
$env:FIREBIRD_MATURITY_DB = "..."   # nunca comitar isso
$env:ISC_USER = "..."
$env:ISC_PASSWORD = "..."
pwsh -File scripts/maturity_battery.ps1 > maturity_report.md
```

O `maturity_report.md` gerado e seguro para compartilhar (sem dado
sensivel) e deve ser anexado a esta gate antes da decisao final de
release.

## 8. Follow-ups nao bloqueantes

- **Issue hygiene (achado nesta gate)**: #33 (INT128/NUMERIC view crash)
  e #35 (ReadBlob multi-segment truncation) tem fix confirmado mergeado em
  main (`33c5d46`/`637fbb8` respectivamente, codigo atual verificado) mas
  as issues do GitHub continuam **abertas**. Recomendacao: fechar as duas
  com comentario apontando o commit/PR de merge — acao de GitHub visivel,
  fica para autorizacao explicita do Fernando, nao feita nesta gate.
- **#36** (aberta, real, nao-bloqueante): falha de conexao em
  `ReconcileViewColumnTypes.LookupObjectType` fora do proprio try/catch
  rebaixa o batch ATTACH inteiro pro fallback per-table lento, em vez de
  isolar so a view com problema. Nao e bug de corretude.
  - **#47** e **#48**: fechadas nesta mesma Production Stability Gate,
  sem residuo (guard hardening e DECFLOAT CI, respectivamente).
- **Debt list do roadmap (`docs/en/roadmap.md`)**: estimativas de linha em
  `firebird_profile_table`, contagem active/in-use + `last_error` em
  `firebird_pool_stats()`, exercitar `recommended_partitions > 1` em CI,
  pushdown de agregacao (bloqueado por limite da API DuckDB v1.5.3) — nenhum
  item novo alem do que ja estava documentado.
- **Windows CI sem suite de testes** (Secao 3) — automatizar
  `test/sql/firebird_*.test` no `build-windows.yml` removeria a
  dependencia do processo manual `build_matrix.ps1`.
- **Doc gap de versao suportada** (Secao 1) — `guide_linux.md`/
  `guide_windows.md` nao mencionam a matriz completa v1.5.2-v1.5.4.

## Decisao final

**READY, condicionado a maturity battery real (Secao 7).**

Tudo que pode ser verificado sem acesso ao ambiente de producao do
Fernando esta verde: compatibilidade DuckDB (3/3), matriz Firebird CI
(3/3), client-lib runtime loading (robusto, documentado), artefatos
Linux/Windows (ambos produzidos corretamente), docs de instalacao
(corretas, com um gap cosmetico de comunicacao). O unico item em aberto
e a execucao real do harness da Secao 7 contra o banco de producao —
por desenho, isso so pode ser feito pelo Fernando, no ambiente dele.

Nenhuma acao em `duckdb/community-extensions` ou upstream nesta frente.
Preparacao de changelog/README/release artifacts e Community Update
seguem como proximos passos, apos autorizacao explicita separada.
