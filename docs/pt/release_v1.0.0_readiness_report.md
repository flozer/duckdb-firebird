# v1.0.0 — Relatorio final de Release Prep

Consolidacao de todas as gates fechadas desde v0.6.1 (ultima versao
publicada no catalogo community) e decisao final de release-readiness
para **v1.0.0** do repositorio `duckdb-firebird` (nao do catalogo
`duckdb/community-extensions`, que segue em v0.6.1 ate autorizacao
explicita separada).

Ver `docs/en/release_notes_v1.0.0.md` para o changelog completo por
categoria (features/fixes/CI-runtime/docs).

## Gates fechadas

| Gate | Escopo | Status |
|---|---|---|
| v0.7 — Metadata & Explain | Metadata Bridge 2.0 (7 funcoes catalogo + constraints), `firebird_explain_pushdown`, `firebird_type_audit` | Fechada |
| v0.8 — Diagnostics Bridge | `firebird_health`, alerts estruturados em `firebird_profile_table`, `firebird_index_profile` | Fechada |
| v1.0 — Production Bridge (item Smart Scan Planner) | Relatorio de planejamento de scan consolidando os sinais acima + fix de determinismo de `ROWS` | Fechada |
| v1.0 — Type/BLOB lossless hardening | #33 (view crash INT128/NUMERIC), #35 (ReadBlob multi-segmento), DECFLOAT fallback | Fechada |
| Production Stability Gate | 6 sub-frentes: #42/#43 (CI vermelho), #39 (fixture unica), #31 (cobertura CI), #26 (NONE charset), #47 (guard hardening), #48 (DECFLOAT CI) | Fechada |
| Runtime/ABI Compatibility Gate | 8/8 areas: compat DuckDB, client-lib runtime loading, artifacts Linux/Windows, matriz DuckDB, matriz Firebird, docs, maturity battery real, follow-ups | Fechada — READY |

## Matrizes de compatibilidade (evidencia fresca)

- **DuckDB**: v1.5.2, v1.5.3 (baseline), v1.5.4 — 3/3 PASS, 19 arquivos de
  teste, 854 assercoes cada, zero drift de API
  (`docs/pt/duckdb_1_5_compatibility_plan.md`).
- **Firebird**: 3, 4, 5 — CI real (`build-linux-fb-matrix.yml`) verde nas
  3 legs, fixture canonica unica, client-lib version-matched por leg,
  guard anti-drift hardened.
- **Firebird 2.5**: nao testado — roadmap historicamente citava
  "2.5/3/4/5 quando houver ambiente viavel"; sem ambiente 2.5 disponivel
  nesta rodada. Nao bloqueia (2.5 esta fora de manutencao ativa do
  proprio Firebird); registrado como gap conhecido, nao regressao.
- **Artifacts**: Linux (`firebird.duckdb_extension-linux-x64`) e Windows
  (`firebird.duckdb_extension-windows-x64`) ambos produzidos
  corretamente em CI, com `if-no-files-found: error`.
- **Maturity battery real**: rodada contra base de teste real (~90 GB,
  2926 relacoes, 15367 colunas NONE-charset) — zero erro de produto, 9/9
  amostras de BLOB/texto auto-consistentes.

## Docs

- Function manual (EN/PT): paridade confirmada — todo `firebird_*` em EN
  tem equivalente em PT, incluindo a nota "read-only"/"somente leitura"
  por funcao onde aplicavel.
- Guias de build: `docs/pt/guide_linux.md` e `docs/pt/guide_windows.md`
  criados nesta frente (gap que `docs/DOCS_PARITY.md` ja sinalizava como
  pendente "antes de release maior"). `DOCS_PARITY.md` atualizado para
  `Paired` nos dois.
- Os 4 guias (`guide_linux`/`guide_windows`, EN+PT) agora apontam
  explicitamente para a matriz de compatibilidade DuckDB completa, nao
  so a v1.5.3 usada no pin de build.
- README: reposicionado para deixar claro que materializacao/export/Arrow
  Flight sao capacidades do proprio DuckDB, nao algo que a extensao
  implementa ou gerencia — sem vender ETL/CDC/scheduler como core.
  "Current Status" atualizado para v1.0.0 release-ready, deixando
  explicito que o catalogo Community segue em v0.6.1.
- `SECURITY.md`: politica adequada, sem gap identificado.

## Higiene de issues

- #33 e #35: fix confirmado em main, fechadas nesta rodada (referencia
  ao commit de merge).
- #47 e #48: fechadas nas respectivas frentes da Production Stability
  Gate.

## Follow-ups nao bloqueantes (nao impedem v1.0.0)

- **#36** (aberta) — falha de conexao dentro de
  `ReconcileViewColumnTypes.LookupObjectType` fora do proprio try/catch
  rebaixa o batch ATTACH inteiro pro fallback per-table lento, em vez de
  isolar so a view com problema. Nao e bug de corretude.
- Guard anti-drift (`scripts/check_no_inline_fixture_drift.sh`) tem
  limites de cobertura de keyword documentados no proprio cabecalho do
  script (falso-positivo/falso-negativo residuais, ja mitigados na
  hardening do #47, mas nunca 100% exaustivos por design de regex).
- Windows CI builda e publica o artefato mas nao roda
  `test/sql/firebird_*.test` — cobertura ali e 100% manual
  (`scripts/build_matrix.ps1`), executada por Fernando antes de cada
  release. Gap de automacao real, nao de corretude.
- Firebird 2.5 fora da matriz por falta de ambiente (ver acima).

## Decisao final

**READY para release v1.0.0 do repositorio.**

Nenhum blocker identificado. Todas as gates de produto, CI e
compatibilidade fecharam com evidencia real (nao so local/sintetica).
Os follow-ups listados sao conhecidos, documentados, e nenhum deles
afeta corretude do dado entregue ao usuario.

**Fora de escopo desta frente, mantido explicitamente:**

- Nenhuma acao em `duckdb/community-extensions`.
- Nenhum pedido de update community aberto.
- Nenhum banco Firebird real foi alterado — a maturity battery
  permanece read-only e o relatorio incorporado ja sanitizado.
- Tag/release do GitHub (`v1.0.0`) ainda nao criada — decisao e acao
  separadas, aguardando autorizacao explicita do Fernando.
