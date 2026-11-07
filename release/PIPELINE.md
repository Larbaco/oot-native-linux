# Pipeline de release (local, determinístico, sem LLM)

Um comando gera o pacote, roda todos os gates e só então dá os nomes finais. Sem prompts; o primeiro gate
que falha aborta e nada é promovido.

```
make release VERSION=X.Y.Z PREV=/caminho/da/release-anterior.tar.xz OUT=/disco/grande/oot-release-work
make verify-repro VERSION=X.Y.Z OUT=/disco/grande/oot-release-work
make publish VERSION=X.Y.Z OUT=/disco/grande/oot-release-work            # dry-run; PUBLISH=1 publica de verdade
make e2e DUMP=/caminho/oot.nup [NEG_DUMP=outro-jogo.nup] [BUILD_DIR=dir] [RUN=1] OUT=...   # só local, nunca CI
```

Variáveis: `PREV` (pacote `.tar.xz` da release anterior; `none` só com `ALLOW_ABI_BREAK=1`), `PREV_ABI` (baseline de ABI commitado, ex. `release/abi-baseline-v0.1.3.json`; se dado, é a referência e precisa concordar com o `PREV`), `OUT` (nunca `/`,
`/tmp` ou dentro do repo; pode ficar em `release/local.mk`, ignorado pelo git), `JOBS` (padrão: 90% das threads, a regra única de `oot_native_jobs.py`),
`ALLOW_ABI_BREAK=1`, `KEEP_WORK=1` (mantém a árvore de trabalho), `MIN_FREE_GB` (padrão 25).
Toda tarefa pesada roda com `nice -n 19 ionice -c3`.

## Gates

| Gate | O que garante |
|---|---|
| `pre-clean-tree` | árvore git limpa; registra commit, versão e `SOURCE_DATE_EPOCH` (timestamp do **commit**, não a hora atual; a data do nome do pacote sai dele) |
| `pre-free-space`, `pre-final-names` | espaço livre em `OUT`; `OUT/v<VERSION>` ainda não existe; `PREV` informado |
| `pins-downloads`, `pins-nxnative` | LLVM, SDL3, headers Vulkan, Python, .NET: SHA-256 de `pins.env` (arquivo errado é apagado); nxnative no commit fixo, árvore intocada |
| `images` | imagens do builder e do smoke: base `ubuntu@sha256:…`, apt num snapshot datado, pip com `--require-hashes`, .NET por tarball verificado; o conteúdo instalado precisa bater com `BUILDER_FP`/`SMOKE_FP` |
| `container-prep` … `container-stage` | CPython 3.12 e o runtime do builder, `nxs2.py setup`, compilação, staging do pacote (cada fase num `docker run` próprio) |
| `tests-python-tools`, `ctest`, `tests-native`, `ctest-evidence` | testes Python e das ferramentas, CTest (zero falhas, `nxrt.vulkan_loader_interposition` passou), testes do `zbic-unpack` sem skip |
| `docs-version-source`, `docs-version-package` | os textos (`README.md`, `README-RELEASE.md`, `THIRD-PARTY-SOURCE.md`, `TESTER-v<versão base>.md`) são modelos com marcadores entre arrobas (VERSION, PKG_NAME, PACKAGE_ARCHIVE, THIRDPARTY_SOURCE_ARCHIVE, TESTER_GUIDE), preenchidos por `render_docs.py` a partir de `VERSION`; o gate falha se qualquer `.md`/`.txt` empacotado ou da fonte citar nome de arquivo de outra versão, tiver marcador entre arrobas sem preencher (as linhas de baseline de glibc só são toleradas no modelo `release/README-RELEASE.md` da fonte; no pacote nada pode sobrar), citar plataforma de tester não confirmada (lista em `render_docs.py`), ou se o guia `TESTER-v<base>.md` não existir/não estiver (sozinho) na allowlist |
| `source-stage`, `release-check-source-dir`, `source-tarball` | fonte por allowlist; `release_check` no diretório e no tarball (PI, caminhos pessoais, assinaturas de jogo) |
| `thirdparty-source` | tarball `-thirdparty-source` com os fontes exatos de FFmpeg 7.1.1 e dav1d 1.5.1 (SHA-256 de `pins.env`, conferidos de novo dentro do arquivo), com `release_check` próprio; vai dentro do pacote e no manifesto |
| `package-manifest`, `release-check-package-dir`, `negative-forbidden-fixture`, `package-tarball` | `SHA256SUMS` só com caminhos relativos e que se verifica de dentro do pacote; `release_check` no pacote e no tarball; um `.nso` sintético precisa ser rejeitado |
| `smoke-*` | pacote extraído num Ubuntu 22.04 limpo, sem rede: os gates Vulkan 2a–2e (backend `vk`, sem `vk*` exportado, `libvulkan.so.1`, sonda `LD_PRELOAD`, `ldd`, referência = release anterior), doctor, imports, dump inválido em en/pt-BR, GUI, ZBIC, `ldd` em todos os ELF, tradutor sem ICU. `smoke-evidence` exige **cada** linha PASS esperada: linha ausente = FAIL |
| `abi` | veja abaixo |
| `manifest`, `promote` | `release-manifest.json`; só aqui os arquivos ganham nome final |

## Gate de ABI e `requires_rebuild`

O `game.so` de um build é ligado ao `nxrt` pelo nome dos símbolos `rt_*`/`adapter_*`/`nxw_*`, por
`NXW_ABI_VERSION`, pelo layout de `NxwGuestCpu`, pelo plano de endereços e pelo registro de host-calls
(que o `nxanalyze` usa no build). O gate extrai isso do `nxrt` novo sem carregar jogo: `nxrt --abi-info`
(constantes e registro compilados) + `nm -D` (exports), e confere contra a fonte correspondente do pacote
(`hle_registry.gen.c` e cabeçalhos); se as duas visões divergirem, falha. Depois compara com a release
anterior (o `PREV`; releases sem `--abi-info` são descritas pela fonte + exports).

`requires_rebuild: true` se mudou: versão da ABI, tamanho/layout do frame, plano de endereços, capacidade
de slots, **remoção** de um export `rt_/adapter_/nxw_`, ou qualquer entrada do registro (nome, tipo, estado).
Adição de export isolada não exige rebuild (aparece em `exports_added`). Com `false`, quem já tem uma pasta de
build da release anterior pode reutilizá-la com o `nxrt` novo; com `true`, precisa refazer o build. Para
promover uma release assim é preciso `ALLOW_ABI_BREAK=1`; o manifesto registra `abi_break_allowed`.
Contagens de relocações por jogo dependem do dump e só existem em tempo de execução, não entram aqui.

## Disco

Tudo grande vive em `OUT` (disco grande). As imagens Docker (modo rootless) ficam no disco do `$HOME`: o pipeline não cria volumes
nem escreve no `/tmp` de dentro do container (ele é montado em `OUT/work/.../out/container-tmp`), e cada gate registra o espaço livre
de `/` e de `OUT` em `gates.jsonl` (`free_root_gb`, `free_out_gb`). Primeiro uso: ~13 GB de cache (LLVM extraído, downloads, nxnative)
+ ~6 GB por execução; `KEEP_WORK=1` mantém a árvore de trabalho.

## Onde ficam os arquivos

- Final: `OUT/v<VERSION>/` com `*.tar.xz`, `*-source.tar.xz`, os `.sha256`, `release-manifest.json`,
  `*.release-abi.json` (listas completas de exports e registro) e `gate-logs/`.
- Trabalho e logs de cada gate: `OUT/work/v<VERSION>/logs/<gate>.log` e `gates.jsonl`; uma falha deixa tudo aqui.
- Cache compartilhado: `OUT/cache/` (downloads verificados, LLVM extraído, clone do nxnative).

## Reprodutibilidade

`make verify-repro` constrói duas vezes (mesma imagem, mesmos pins, `SOURCE_DATE_EPOCH` do commit) e grava
`OUT/repro/v<VERSION>/repro-report.{json,md}` com os arquivos que diferem e a causa provável. É relatório,
não bloqueio. Já tratados: `SOURCE_DATE_EPOCH` (clang/gcc `__DATE__`/`__TIME__`), `tar --sort=name --mtime
--owner=0`, `LC_ALL`/`TZ` fixos, `PYTHONHASHSEED=1` no PyInstaller, `.pyc` removidos, `pip --no-compile`,
ordem do `SHA256SUMS`. Limitações conhecidas estão no relatório da última execução.

## Publicar: `make publish`

`release/publish.sh` automatiza a publicação de uma release já promovida. **Dry-run por padrão**; só publica com `PUBLISH=1`.
Pré-condições (todas obrigatórias, nesta ordem): manifesto de `OUT/v<VERSION>` com todos os gates e gates de smoke PASS e CTest limpo;
SHA-256 dos três tarballs iguais aos `.sha256` e ao manifesto; o commit do manifesto é o HEAD e a árvore está limpa (por isso as notas
`release/notes/v<VERSION>.md` são versionadas **antes** do `make release`; elas levam os SHA-256 por um marcador SHA256_BLOCK);
o repositório builder (derivado de `git remote origin`) é PRIVADO, consultado via `gh api`; tag e release não existem no repositório
público (`<owner>/<nome>-linux`, derivado do builder; `PUBLIC_REPO=` sobrescreve); a identidade do autor (git config ou
`PUBLISH_AUTHOR_NAME/EMAIL`; nunca em arquivo versionado) é a do commit público anterior (`ALLOW_NEW_IDENTITY=1` para trocar).
Export: a fonte publicada é o tarball `-source` menos `upstream/`, igual à allowlist, com `release_check` e as varreduras de guarda
(endereços privados, logins, caminhos de máquina, chaves/tokens, e-mails, arquivos > 5 MB, binários, tipos de jogo, menções novas ao
título que o dono aceitou manter). Vira **um** commit novo, filho do `main` público, sem histórico do builder; data do commit =
`SOURCE_DATE_EPOCH`, então o hash é determinístico. Tag anotada `v<VERSION>` e release com 6 assets (3 tarballs + 3 `.sha256`),
pre-release por padrão (`STABLE=1` troca por release estável com `--latest`). Antes de publicar de verdade ele repete as checagens
remotas, e depois confere: digests dos assets no GitHub × sidecars, sidecars baixados × locais, `main` público = commit novo e
= export byte a byte, tag anotada apontando para o commit, tags antigas inalteradas, varreduras no conteúdo publicado e builder
ainda privado. Nada usa `--force`; se algo falhar depois do push, o script para e informa (não há rollback automático).

Os dumps nunca entram no pipeline, no CI nem nos artefatos.
