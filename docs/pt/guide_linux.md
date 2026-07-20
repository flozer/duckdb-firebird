# duckdb-firebird — inicio rapido no Linux

O CI roda isto em todo push: `.github/workflows/build-linux.yml`. Este guia
e o equivalente para desenvolvimento local. Os numeros abaixo sao do
Ubuntu 24.04 LTS, 4 vCPU.

## Passo 0 — Requisitos

| Componente | Versao testada | Instalar |
|---|---|---|
| Servidor Firebird | 5.0.x (3.0 / 4.0 tambem funcionam) | `apt install firebird5.0-server` (Ubuntu 24+) ou [tarball firebirdsql.org](https://firebirdsql.org/en/firebird-5-0/) |
| `libfbclient` + headers | mesma versao do servidor | `apt install firebird-dev` (fornece a entrada `pkg-config`) |
| Ferramentas de build | gcc 11+ / clang 14+ | `apt install build-essential cmake ninja-build git` |
| Python 3 | 3.10+ | `apt install python3` (usado pelo harness de teste do CI) |

Para o pacote `firebird5.0-server` no Ubuntu/Debian, a senha padrao do
`SYSDBA` fica em `/etc/firebird/SYSDBA.password` — leia uma vez na
instalacao e guarde.

## Passo 1 — Clonar e fixar versao

```bash
git clone https://github.com/flozer/duckdb-firebird.git
cd duckdb-firebird
git submodule update --init --recursive --depth=1
DUCKDB_GIT_VERSION=v1.5.3 make set_duckdb_version
```

## Passo 2 — Build

O repositorio traz [`scripts/build_linux_local.sh`](../../scripts/build_linux_local.sh),
que fixa a versao do [DuckDB](https://github.com/duckdb/duckdb), verifica os headers do `libfbclient`, e delega
para o Makefile das [DuckDB extension-ci-tools](https://github.com/duckdb/extension-ci-tools):

```bash
scripts/build_linux_local.sh
```

Saida esperada (~8 min em 4 vCPU):

```
[469/469] Linking CXX executable duckdb
```

Artefatos:

- `build/release/duckdb` — DuckDB v1.5.3 com `firebird` linkado
  estaticamente.
- `build/release/extension/firebird/firebird.duckdb_extension` —
  carregavel num CLI DuckDB padrao.
- `build/release/repository/v1.5.3/linux_amd64/firebird.duckdb_extension`
  — o mesmo carregavel, empacotado para o repositorio de extensoes
  upstream.

Se `pkg-config --modversion fbclient` falhar, aponte o CMake pro SDK
explicitamente:

```bash
scripts/build_linux_local.sh --fb-sdk-root /opt/firebird
```

Opcoes uteis:

```bash
scripts/build_linux_local.sh --clean
scripts/build_linux_local.sh --debug
SKIP_SUBMODULES=1 scripts/build_linux_local.sh
SKIP_DUCKDB_PIN=1 scripts/build_linux_local.sh
```

No WSL, builds a partir de `/mnt/c` ou `/mnt/d` sao muito mais lentos
que builds dentro do filesystem Linux, porque o submodulo do DuckDB tem
muitos arquivos pequenos. Se o script parecer travar em
`cd duckdb && git checkout v1.5.3`, geralmente e o passo de fixacao de
versao do DuckDB tocando arquivos no drive montado do Windows. Depois
da primeira fixacao bem-sucedida, use
`SKIP_DUCKDB_PIN=1 scripts/build_linux_local.sh` para builds seguintes,
ou clone o repositorio em `~/src` para builds Linux muito mais rapidos.

Se voce alterna entre builds Windows e WSL, o script remove
automaticamente um `build/release/CMakeCache.txt` obsoleto quando
detecta que o CMake foi configurado a partir do outro estilo de path
(`D:/...` vs `/mnt/d/...`). Use `AUTO_CLEAN_CMAKE_CACHE=0` se preferir
que ele falhe em vez disso.

Para gerar um arquivo de distribuicao local apos um build de release:

```bash
scripts/package_dist_linux.sh
# opcional: empacotar o libfbclient.so do sistema tambem
scripts/package_dist_linux.sh --include-fbclient
```

## Passo 3 — Subir uma fixture Firebird 5

```bash
sudo systemctl start firebird5.0-server
# (Opcional) resetar a senha do SYSDBA pra uma senha conhecida:
sudo gsec -modify SYSDBA -pw masterkey

# Construir uma base sandbox com a fixture de tipos FB4:
mkdir -p /tmp/fbtest
isql-fb -u SYSDBA -p masterkey <<'EOF'
CREATE DATABASE '/tmp/fbtest/biz4.fdb' DEFAULT CHARACTER SET UTF8;
EOF
isql-fb -u SYSDBA -p masterkey -i scripts/fixture_biz4.sql /tmp/fbtest/biz4.fdb
```

(Em distros que nao renomeiam para `isql-fb`, o binario e so `isql`
— confira com `which isql` primeiro.)

O script de CI do repositorio `scripts/setup_test_firebird.sh` e a
referencia autoritativa para a fixture FB3 mais antiga; a fixture FB4
acima e a nova, usada pelo guia Windows e pelo relatorio de verificacao
em `test_report.md`.

## Passo 4 — Teste de fumaca

```bash
./build/release/duckdb -unsigned <<'EOF'
SELECT * FROM firebird_tables('/tmp/fbtest/biz4.fdb');
SELECT typeof(BIG_NUM), BIG_NUM
  FROM firebird_scan('/tmp/fbtest/biz4.fdb', 'FB4_TYPES', partitions=1)
 LIMIT 2;
EOF
```

Esperado:

```
HUGEINT   170141183460469231731687303715884105727
HUGEINT  -170141183460469231731687303715884105728
```

Se aparecer `VARCHAR` em vez disso, voce esta rodando um build mais
antigo que o fix do RDB$FIELD_TYPE 24-31 — atualize e rode
`make clean && make release`.

## Passo 5 — Consultas federadas

```sql
-- Materializar um snapshot:
CREATE TABLE local_fb4 AS
  SELECT * FROM firebird_scan('/tmp/fbtest/biz4.fdb', 'FB4_TYPES');

-- Exportar para Parquet:
COPY (SELECT * FROM firebird_scan('/tmp/fbtest/biz4.fdb', 'FB4_TYPES'))
  TO '/tmp/fbtest/fb4.parquet' (FORMAT 'parquet');

-- ATTACH nativo:
ATTACH '/tmp/fbtest/biz4.fdb' AS fb (TYPE firebird,
                                     user     'SYSDBA',
                                     password 'masterkey');
SELECT * FROM fb.main.DEPARTMENT;
DETACH fb;
```

A forma de URL tambem funciona — util para servidores Firebird
remotos:

```sql
SELECT * FROM firebird_scan(
    'firebird://SYSDBA:masterkey@db.host:3050/srv:/data/prod.fdb?charset=UTF8',
    'EMPLOYEE');
```

## Carregando num CLI DuckDB padrao

```bash
duckdb -unsigned <<'EOF'
LOAD '/path/to/duckdb-firebird/build/release/extension/firebird/firebird.duckdb_extension';
SELECT * FROM firebird_tables('/tmp/fbtest/biz4.fdb');
EOF
```

Se o CLI padrao for mais antigo que a ABI v1.5.3 contra a qual a
extensao foi construida, o LOAD falha com um erro de incompatibilidade
de versao — alinhe as versoes ou use nosso `build/release/duckdb` em
vez disso.

## Rodando a suite de testes de integracao

```bash
GEN=ninja make test_release   # usa build/release/test/unittest
```

A suite espera um servidor Firebird alcancavel com as fixtures
descritas em `scripts/setup_test_firebird.sh`. O CI cuida de tudo isso
num container; localmente voce vai apontar pro seu proprio servidor via
`FIREBIRD_TEST_DB=/tmp/fbtest/test.fdb` etc.

## Solucao de problemas

### `libfbclient.so.2: cannot open shared object file`
O nome do pacote varia — tente `apt install firebird-dev libfbclient2`
ou `apt install firebird5.0-utils`. Em distros baseadas em RHEL:
`dnf install firebird-libs firebird-devel`.

### `op-system-call-failed isc_attach_database`
O servidor Firebird nao esta rodando, ou o path nao existe, ou o
usuario com que o servidor roda nao consegue ler o `.fdb`. Verifique
`systemctl status firebird5.0-server` e `ls -l /tmp/fbtest/biz4.fdb`
(o UID do daemon Firebird precisa conseguir abrir o arquivo).

### `partitions > 1` mais lento que `partitions=1`
Num SuperServer de host unico, o scheduler do servidor serializa
consultas contra o mesmo arquivo — cursores extras so adicionam
overhead. `partitions=` e opt-in para deployments remotos ou
Classic/SuperClassic, onde o paralelismo e barato. Veja
"Why the conservative default?" em `architecture.md`.
