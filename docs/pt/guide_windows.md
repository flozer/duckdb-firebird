# duckdb-firebird — inicio rapido no Windows

Verificado no Windows 11 Pro 26200 + Firebird 5.0.4 SuperServer +
[DuckDB](https://github.com/duckdb/duckdb) v1.5.3 (compilado a partir do
codigo-fonte) + MSVC 19.44 (Visual Studio 2022 Build Tools). Os tempos
abaixo sao de uma instalacao nova num notebook moderno.

## Passo 0 — Requisitos

| Componente | Versao testada | Onde obter |
|---|---|---|
| Servidor Firebird | 5.0.4 (FB3 / FB4 tambem funcionam) | <https://firebirdsql.org/en/firebird-5-0/> |
| Visual Studio 2022 Build Tools (Workload: "Desktop development with C++") | MSVC 19.44 | `winget install Microsoft.VisualStudio.2022.BuildTools` |
| CMake | 4.x | `winget install Kitware.CMake` |
| Ninja | 1.13+ | `winget install Ninja-build.Ninja` |
| Git | 2.40+ | `winget install Git.Git` |
| DuckDB CLI (opcional, pode usar o que construimos) | 1.5.x | `winget install DuckDB.cli` |

A instalacao do Firebird 5 traz **os dois**: o runtime (`fbclient.dll`,
servico do servidor) **e** o SDK (headers em `include\`, biblioteca de
import `lib\fbclient_ms.lib`), entao nao ha um download separado de
"client SDK". Path de instalacao padrao:
`C:\Program Files\Firebird\Firebird_5_0`.

Este guia fixa o DuckDB na v1.5.3 pra builds reproduziveis. A extensao
tambem e verificada contra v1.5.2 e v1.5.4, sem nenhum drift de API —
veja `docs/pt/duckdb_1_5_compatibility_plan.md` pra matriz completa
entre versoes.

## Passo 1 — Clonar e fixar versao

```powershell
git clone https://github.com/flozer/duckdb-firebird.git
cd duckdb-firebird

# Baixa os submodulos duckdb + extension-ci-tools (shallow mantem rapido).
git submodule update --init --recursive --depth=1

# Fixa o DuckDB na v1.5.3 — corresponde a ABI da nossa extensao.
cd duckdb
git fetch --tags --depth=1 origin v1.5.3
git checkout v1.5.3
cd ..
```

## Passo 2 — Build

O repositorio traz [`scripts/build_windows_local.bat`](../../scripts/build_windows_local.bat),
que carrega o `vcvars64.bat`, aponta o CMake pros paths do SDK do FB5, e
chama o Ninja. A partir de um `cmd.exe` normal (ou PowerShell que deixe
rodar .bat):

```cmd
cd D:\path\to\duckdb-firebird
scripts\build_windows_local.bat
```

Saida esperada (~10 min num notebook de 4 nucleos):

```
[1/2] Building CXX object extension\firebird\CMakeFiles\firebird_loadable_extension.dir\src\firebird_scanner.cpp.obj
[2/2] Linking CXX shared library extension\firebird\firebird.duckdb_extension
…
--- artifacts ---
D:\path\to\duckdb-firebird\build\release\extension\firebird\firebird.duckdb_extension
```

Tres coisas ficam em `build\release\`:

- `duckdb.exe` — um build do DuckDB v1.5.3 com a extensao firebird
  **linkada estaticamente** (nao precisa de `LOAD`). Este e o binario
  autoritativo pra teste.
- `extension\firebird\firebird.duckdb_extension` — a variante
  carregavel. Use isso pra `LOAD` num CLI DuckDB v1.5.3 padrao.
- `repository\v1.5.3\windows_amd64\firebird.duckdb_extension` — o
  mesmo carregavel, organizado como o repositorio de extensoes
  upstream.

## Passo 3 — Garantir que o `fbclient.dll` certo carrega

Esse detalhe pega muita gente. O Windows traz um
`C:\Windows\System32\FBCLIENT.DLL` de sistema em maquinas que tem
qualquer versao do Firebird instalada. Essa copia e a que o *primeiro*
instalador escreveu — que numa maquina com FB3+FB5 e o client do FB3,
**nao** o do FB5. Um client FB3 nao consegue decodificar tipos FB4+
(INT128, DECFLOAT, TIMESTAMP_TZ), e o sintoma e
`isc_dsql_prepare: Data type unknown [sqlcode -204]`.

Duas correcoes confiaveis:

```powershell
# Ou copie o client do FB5 pra junto do duckdb.exe:
copy "C:\Program Files\Firebird\Firebird_5_0\fbclient.dll" build\release\

# OU coloque o Firebird_5_0 no inicio do PATH pro shell atual:
$env:Path = 'C:\Program Files\Firebird\Firebird_5_0;' + $env:Path
```

O `build_windows_local.bat` ja copia a DLL do FB5 pra
`build\release\` como parte do passo pos-build.

## Passo 4 — Criar uma base sandbox

```sql
-- Salve como C:\fbtest\create.sql e depois rode:
--   isql -i C:\fbtest\create.sql
SET SQL DIALECT 3;
CREATE DATABASE 'C:\fbtest\biz4.fdb'
    USER 'SYSDBA' PASSWORD 'masterkey'
    DEFAULT CHARACTER SET UTF8;
QUIT;
```

```cmd
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" -i C:\fbtest\create.sql
```

Uma fixture pronta com tipos FB4 esta em
[`scripts/fixture_biz4.sql`](../../scripts/fixture_biz4.sql) — aplique
com:

```cmd
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" ^
    -user SYSDBA -password masterkey ^
    -i scripts\fixture_biz4.sql C:\fbtest\biz4.fdb
```

## Passo 5 — Teste de fumaca

A partir de `build\release\`:

```sql
-- O build estatico ja tem o firebird registrado; nao precisa de LOAD.
SELECT * FROM firebird_tables('C:/fbtest/biz4.fdb');

-- Checagem viva do mapeamento de tipos:
SELECT typeof(BIG_NUM), typeof(BIG_DEC), typeof(TS_TZ)
  FROM firebird_scan('C:/fbtest/biz4.fdb', 'FB4_TYPES', partitions=1)
 LIMIT 1;
-- HUGEINT | DECIMAL(38,5) | TIMESTAMP WITH TIME ZONE
```

### Fluxos federados

```sql
-- Materializar no storage nativo do DuckDB:
CREATE TABLE local_fb4 AS
  SELECT * FROM firebird_scan('C:/fbtest/biz4.fdb', 'FB4_TYPES');

-- Exportar para Parquet (HUGEINT degrada para DOUBLE na saida Parquet —
-- DECIMAL(38, s) permanece exato):
COPY (SELECT * FROM firebird_scan('C:/fbtest/biz4.fdb', 'FB4_TYPES'))
  TO 'C:/fbtest/fb4.parquet' (FORMAT 'parquet');

-- Firebird ⋈ Parquet:
SELECT  f.ID, f.LABEL, p.BIG_DEC
  FROM  firebird_scan('C:/fbtest/biz4.fdb', 'FB4_TYPES') f
  JOIN  read_parquet('C:/fbtest/fb4.parquet') p ON f.ID = p.ID;

-- ATTACH nativo — expoe a base inteira como um catalogo DuckDB:
ATTACH 'C:/fbtest/biz4.fdb' AS fb (TYPE firebird,
                                   user     'SYSDBA',
                                   password 'masterkey');
SELECT * FROM fb.main.DEPARTMENT;
DETACH fb;
```

## Passo 6 — Carregando a extensao num CLI DuckDB padrao

Se voce ja tem um CLI DuckDB v1.5.3 instalado separadamente, pode
carregar o `.duckdb_extension` que acabamos de construir sem
reconstruir o CLI:

```cmd
duckdb -unsigned
```

```sql
LOAD 'D:/path/to/duckdb-firebird/build/release/extension/firebird/firebird.duckdb_extension';
SELECT * FROM firebird_tables('C:/fbtest/biz4.fdb');
```

A flag `-unsigned` (ou `SET allow_unsigned_extensions=true;` no
`duckdbrc`) e necessaria porque o binario da extensao nao e assinado
pela autoridade de extensoes do DuckDB. Quando publicarmos via
`community-extensions`, esse passo deixa de ser necessario.

## Solucao de problemas

### `Data type unknown [sqlcode -204]` em todo prepare
`fbclient.dll` errado. Veja o Passo 3.

### `isc_attach_database: unavailable database`
O parser de URL envia o path literalmente pro libfbclient. No
Windows, use um **path puro** (`C:/fbtest/biz4.fdb`) ou a forma
key=value (`database=C:\fbtest\biz4.fdb;user=SYSDBA;password=masterkey`).
A forma de URL `firebird://USER:PASS@HOST/PATH` e destinada a
servidores remotos; apontar ela pra um path de disco local do Windows
produz a string libfbclient errada (`HOST:/C:/fbtest/biz4.fdb`).

### `INTERNAL Error: Expected vector of type INT128, but found vector of type VARCHAR`
Voce construiu antes do fix do v0.4 pro RDB$FIELD_TYPE (commit que
introduziu o case 24..31 em `LoadTableSchema`). Atualize, reconstrua
*as duas* variantes — a estatica (`firebird_extension.lib`) e a
carregavel (`firebird.duckdb_extension`) — e rode o `duckdb.exe` de
novo.

### `loaded=true installed=true` mas meu LOAD nao faz efeito
Nosso `build\release\duckdb.exe` traz a extensao linkada
estaticamente. `LOAD` e um no-op ali (a tabela de funcoes ja esta
populada desde o inicio do processo). Ou reconstrua depois de editar
a extensao, ou teste contra um CLI DuckDB padrao que carrega o
`.duckdb_extension` a partir do disco.

### Quarentena de antivirus no `firebird.duckdb_extension`
Alguns fornecedores de AV marcam `.dll`-equivalentes novos e nao
assinados. Coloque `build\release\extension\firebird\` e o diretorio
de instalacao do FB5 na lista de permissoes.
