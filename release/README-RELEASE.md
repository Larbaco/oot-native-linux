# OoT Native Linux x86-64 — v0.1.4 (WIP)

## English

OoT Native recompiles The Legend of Zelda: Ocarina of Time (Switch 2, version
2.0.0) locally for Linux x86-64. This community project is not affiliated with Nintendo. The game name
is used only to identify compatibility. No Nintendo logos, images, or icons are
used.

The package is intended to contain tools and source code only, never game files.
Open the launcher, select only a `.nup` dump from your own game, then choose an
empty build directory. Validation and building run locally. The launcher does
not modify the dump or download files. The build directory contains derived
game data; do not share it.

The launcher defaults to English. Choose **Português (Brasil)** in the GUI; the
selection is saved to `${XDG_CONFIG_HOME:-~/.config}/oot-native/launcher.json`
and takes effect on the next start. For the CLI, use
`--lang en` or `--lang pt-BR`, or set `OOT_NATIVE_LANG=en|pt-BR`. The system
locale does not change the default language.

Version 0.1.4 bundles FFmpeg 7.1.1 with dav1d 1.5.1 for AV1 movie decoding.
Decoding is enabled by default; set `NXRT_MOVIE_DECODE=0` to use the timed
no-image fallback. The exact source tarballs are in the adjacent
`oot-native-linux-x86_64-v0.1.4-2026-11-07-thirdparty-source.tar.xz` archive.
See `TESTER-v0.1.4.md` to test with an existing build without rebuilding it.

```sh
./oot-native-launcher --check "/path/to/your-dump.nup"
./oot-native-launcher --build "/path/to/your-dump.nup" --out "$HOME/Games/oot-native-build"
```

**Build jobs.** The build runs in parallel and by default uses **90% of the
available threads** (rounded down, at least 1): the CPUs the process may run on
(its affinity, so `taskset` and containers count), lowered further by a cgroup
v2 CPU quota (`/sys/fs/cgroup/cpu.max`) if that allows fewer. Use `--jobs N` or
`OOT_NATIVE_JOBS=N` to change it, for example
`./oot-native-launcher --build "/path/to/your-dump.nup" --out "$HOME/Games/oot-native-build" --jobs 6`.
The GUI has a **Build jobs** field that shows the default and "Using N of M
threads"; its value is saved in `launcher.json` next to the language. Order:
`--jobs`, then `OOT_NATIVE_JOBS`, then the GUI's saved value, then the 90%
default. `N` is a whole number of 1 or more; more than the available threads is
accepted with a warning. The build always runs at low priority (`nice -n 19`,
`ionice -c3`). The default is a long load on almost every thread: choose fewer
jobs if your machine overheats or becomes unstable. The shader step uses at most
8 workers whatever the setting.

Shader inventory `glob` rows are patterns matched against RomFS paths; a row
with no matches produces a warning. The RomFS marker, explicit `bfsha`/`sarc`
files, and expected build IDs remain required.

This version is work in progress. Compatibility, audio, video, shaders, and
progression may have issues. Measured requirements: Linux x86-64 with AVX2 and
FMA3, a Vulkan 1.3 driver, glibc `GLIBC_@GLIBC_BASELINE@` or compatible, and
libstdc++ with `GLIBCXX_@GLIBCXX_BASELINE@` or compatible. The package includes
the .NET 10 runtime and enables invariant globalization, so system .NET and ICU
are not required. Reserve at least **45 GB free** on the build destination's
disk; the measured build directory used 38 GB. With 2 jobs, measured total build
time was about **2h25** and peak memory was **4.5 GB RSS** (a 35m17s failed first
attempt plus a successful 1h49m54s resume). The time and memory above were measured with **2 jobs**, the fixed setting of earlier releases, not with the new default. With more jobs the build should finish sooner and use more RAM; no figures were measured for other job counts. The doctor checks the selected
destination. The package includes its DejaVu Sans font and requires no
development toolchains on the user's machine.

Known shader limitation: in the measured package build, 25 of 41,632 shaders
failed translation and 17 failed SPIR-V validation; associated draws are
skipped. The 25 translation failures match those recorded in an earlier
reference run. The validation failures have the same `Offset` diagnostic class
seen in earlier reference reports; a separate reference run recorded 83
validation failures of mixed types on a different shader set. Counts vary
between runs and shader sets, and graphics may be incomplete.

The package is licensed under GPL-3.0-or-later, with compatible file-level
licenses preserved and notices provided in `LICENSES/` and
`THIRD-PARTY-NOTICES.md`. See the corresponding source included with the package.
Credits: pokopia-native (xFractals), nxnative (seedds), Ryujinx, Eden-NS2,
zbic (kinnay), Lichii, Eglantine-fairtech, and Orchid.

The Legend of Zelda, Ocarina of Time, and Nintendo Switch are trademarks of
their respective rights holders. These names are used nominatively to identify
compatibility. This independent project is not affiliated with or endorsed by
Nintendo and uses no Nintendo logos, icons, or images.

## Português

OoT Native recompila localmente o código de The Legend of Zelda: Ocarina of
Time (Switch 2, versão 2.0.0) para Linux x86-64. Este projeto comunitário não é afiliado à Nintendo.
O nome do jogo é usado apenas para identificar o título compatível. Não usamos
logotipos, imagens ou ícones da Nintendo.

O pacote deve conter ferramentas e código-fonte, nunca arquivos do jogo. Para
usar, abra o launcher, selecione somente um dump `.nup` do seu próprio jogo e
escolha uma pasta de build vazia. A validação e a construção são executadas
localmente. O launcher não altera o dump nem baixa arquivos. A pasta de build
contém dados derivados do jogo: não a compartilhe.

O idioma inicial do launcher é inglês. Escolha **Português (Brasil)** na GUI;
a escolha é salva em `${XDG_CONFIG_HOME:-~/.config}/oot-native/launcher.json`
e passa a valer na próxima inicialização. Na CLI, use
`--lang en` ou `--lang pt-BR`, ou defina `OOT_NATIVE_LANG=en|pt-BR`. O locale
do sistema não altera o idioma padrão.

O v0.1.4 inclui FFmpeg 7.1.1 com dav1d 1.5.1 para decodificar vídeos AV1. A
decodificação fica ativa por padrão; defina `NXRT_MOVIE_DECODE=0` para usar o
fallback temporizado sem imagem. Os tarballs exatos do código-fonte estão no
arquivo adjacente `oot-native-linux-x86_64-v0.1.4-2026-11-07-thirdparty-source.tar.xz`.
Consulte `TESTER-v0.1.4.md` para testar com um build existente, sem recompilá-lo.

```sh
./oot-native-launcher --check "/caminho/do/seu-dump.nup"
./oot-native-launcher --build "/caminho/do/seu-dump.nup" --out "$HOME/Games/oot-native-build"
```

**Jobs da build.** A build roda em paralelo e, por padrão, usa **90% das
threads disponíveis** (arredondado para baixo, no mínimo 1): as CPUs em que o
processo pode rodar (sua affinity, então `taskset` e containers contam),
reduzidas ainda mais por uma cota de CPU do cgroup v2 (`/sys/fs/cgroup/cpu.max`)
se ela permitir menos. Use `--jobs N` ou `OOT_NATIVE_JOBS=N` para mudar, por
exemplo
`./oot-native-launcher --build "/caminho/do/seu-dump.nup" --out "$HOME/Games/oot-native-build" --jobs 6`.
A GUI tem o campo **Jobs da build**, que mostra o padrão e "Usando N de M
threads"; o valor é salvo no `launcher.json`, junto com o idioma. Ordem:
`--jobs`, depois `OOT_NATIVE_JOBS`, depois o valor salvo pela GUI, depois o
padrão de 90%. `N` é um inteiro maior ou igual a 1; mais que as threads
disponíveis é aceito com um aviso. A build sempre roda com prioridade baixa
(`nice -n 19`, `ionice -c3`). O padrão é uma carga longa em quase todas as
threads: escolha menos jobs se a máquina esquentar ou ficar instável. A etapa de
shaders usa no máximo 8 workers, qualquer que seja o valor.

As linhas `glob` do inventário de shaders são padrões aplicados aos nomes do
RomFS: uma linha sem correspondências gera aviso. Marcador RomFS, arquivos
`bfsha`/`sarc` explícitos e build IDs esperados continuam obrigatórios.

Esta versão está em desenvolvimento. Compatibilidade, áudio, vídeo, shaders e
progressão ainda podem ter falhas. Requisitos medidos: Linux x86-64 com AVX2 e
FMA3, driver Vulkan 1.3, glibc `GLIBC_@GLIBC_BASELINE@` ou compatível,
libstdc++ com `GLIBCXX_@GLIBCXX_BASELINE@` ou compatível. O pacote inclui o
runtime .NET 10 e ativa globalização invariante, sem exigir .NET ou ICU no
sistema. Reserve pelo menos **45 GB livres** no disco do destino: a pasta de
build medida ocupou 38 GB. Com 2 jobs, o tempo total medido foi cerca de **2h25**
e o pico de memória **4,5 GB RSS** (primeira tentativa interrompida após 35m17s
mais retomada bem-sucedida de 1h49m54s). O tempo e a memória acima foram medidos com **2 jobs**, a configuração fixa dos releases anteriores, e não com o novo padrão. Com mais jobs a build deve terminar antes e usar mais RAM; não foram medidos números para outras quantidades de jobs. O doctor verifica o destino selecionado.
O pacote inclui a fonte DejaVu Sans usada no build e não instala toolchains de
desenvolvimento.

Limitação conhecida de shaders: no build medido, 25 de 41.632 shaders falharam
na tradução e 17 na validação SPIR-V; os draws associados são ignorados. As 25
falhas de tradução coincidem com as registradas em uma execução de referência
anterior. As falhas de validação têm a mesma classe de diagnóstico `Offset`
observada em relatórios de referência anteriores; as contagens variam entre
execuções e conjuntos de shaders. Outra execução de referência registrou 83
falhas de validação de tipos mistos, em um conjunto diferente. Os gráficos podem
ficar incompletos.

O pacote é licenciado como GPL-3.0-or-later, com licenças individuais preservadas
nos arquivos compatíveis e avisos em `LICENSES/` e `THIRD-PARTY-NOTICES.md`.
Consulte também a fonte correspondente incluída no pacote. Créditos: pokopia-native
(xFractals), nxnative (seedds), Ryujinx, Eden-NS2, zbic (kinnay), Lichii,
Eglantine-fairtech e Orchid.

The Legend of Zelda, Ocarina of Time e Nintendo Switch são marcas de seus
titulares. Os nomes são usados de forma nominativa para identificar a
compatibilidade. Este projeto independente não é afiliado nem endossado pela
Nintendo e não usa seus logos, ícones ou imagens.
