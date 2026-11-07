# OoT Native — Linux x86-64 v0.1.5 (WIP)

## English

OoT Native recompiles **The Legend of Zelda: Ocarina of Time** code locally into a native Linux program. This release supports the Nintendo Switch 2 edition, title `0400486024DEC000`, update 2.0.0. The package contains the launcher, builder, and tools; **it contains no game files**. The launcher validates and uses only the `.nup` dump you provide for a game you own. It does not decrypt or convert dumps.

**Status: WIP.** Recompilation and startup have been exercised for this release, but compatibility and gameplay remain incomplete. Graphics, audio, controls, video, shaders, and progression may fail. Do not treat this as a finished product.

### Measured requirements

- Linux x86-64, CPU with AVX2 and FMA3.
- GPU/driver with Vulkan 1.3.
- glibc `GLIBC_2.35` or newer and libstdc++ with `GLIBCXX_3.4.30` or newer.
- The .NET 10 runtime is included; the launcher enables invariant globalization, so system .NET, ICU, and development toolchains are not required.
- Reserve at least **45 GB free** on the build destination's disk. The test build directory used 38 GB.
- With 2 jobs, measured total build time was about **2h25** and peak memory was **4.5 GB RSS**. This adds the first attempt (35m17s, stopped by a bug that has since been fixed) to the successful resumed build (1h49m54s). The time and memory above were measured with **2 jobs**, the fixed setting of earlier releases, not with the new default. With more jobs the build should finish sooner and use more RAM; no figures were measured for other job counts.

Version 0.1.5 bundles FFmpeg 7.1.1 with dav1d 1.5.1 for AV1 movie decoding. Decoding is enabled by default; set `NXRT_MOVIE_DECODE=0` to use the timed no-image fallback. The exact source tarballs are in the adjacent `oot-native-linux-x86_64-v0.1.5-2026-11-07-thirdparty-source.tar.xz` archive. `TESTER-v0.1.5.md` says whether this version can reuse an existing build or needs a new one, and how to test it.

### Known shader limitation

In the package build, 25 of 41,632 shaders failed translation and 17 failed SPIR-V validation; associated draws are skipped. The 25 translation failures match those recorded in an earlier reference run. The validation failures have the same `Offset` diagnostic class seen in earlier reference reports; a separate reference run recorded 83 validation failures of mixed types on a different shader set. Counts vary across runs and shader sets. Graphics may be incomplete as a result.

### Download, verify, and start

Download `oot-native-linux-x86_64-v0.1.5-2026-11-07.tar.xz` and compare `sha256sum` with the SHA-256 published alongside the archive. Then extract and verify the package files:

```sh
sha256sum oot-native-linux-x86_64-v0.1.5-2026-11-07.tar.xz
tar -xf oot-native-linux-x86_64-v0.1.5-2026-11-07.tar.xz
cd oot-native-linux-x86_64-v0.1.5-2026-11-07
sha256sum -c SHA256SUMS
./oot-native-launcher
```

The launcher defaults to English. Choose **Português (Brasil)** in the GUI; the selection is saved to `${XDG_CONFIG_HOME:-~/.config}/oot-native/launcher.json` and takes effect on the next start. For the CLI, use `--lang en` or `--lang pt-BR`, or set `OOT_NATIVE_LANG=en|pt-BR`. The system locale does not change the default language.

In the graphical launcher, choose **your own `.nup` dump**, wait for validation, and choose an empty build directory. Building and running happen locally. To use the command line:

```sh
./oot-native-launcher --check "/path/to/your-dump.nup"
./oot-native-launcher --build "/path/to/your-dump.nup" --out "$HOME/Games/oot-native-build"
"$HOME/Games/oot-native-build/out/play-oot-2.0.0"
```

**Build jobs.** The build runs in parallel and, by default, uses **90% of the available threads** (rounded down, at least 1). "Available" is the number of CPUs the process may run on (its CPU affinity, so `taskset` and containers are respected), lowered further if a cgroup v2 CPU quota (`/sys/fs/cgroup/cpu.max`) allows fewer. Change it with `--jobs N` or `OOT_NATIVE_JOBS=N`, for example `./oot-native-launcher --build "/path/to/your-dump.nup" --out "$HOME/Games/oot-native-build" --jobs 6`. In the GUI, use the **Build jobs** field; it shows the default and "Using N of M threads", and the choice is saved in `launcher.json` next to the language. The order is: `--jobs`, then `OOT_NATIVE_JOBS`, then the value saved by the GUI, then the 90% default. `N` must be a whole number of 1 or more; a value above the available threads is accepted with a warning. The build always runs at low priority (`nice -n 19`, `ionice -c3`). The default is a long load on almost every thread; if your machine overheats or becomes unstable, choose fewer jobs. The build's shader step uses at most 8 workers whatever the setting.

The launcher never modifies the dump or downloads files. **Never share the build directory**: it contains recompiled code, extracted files, and other data derived from your game.

### Saves and logs

Saves, runtime logs, and Vulkan cache are stored in `${XDG_DATA_HOME:-$HOME/.local/share}/oot-native/2.0.0/`. Build logs are inside the build directory. Before sharing a log, remove local paths, personal filenames, and any game-derived data. Do not attach the build directory, dumps, saves, shaders, or screenshots.

### Reporting problems

Include the package version, Linux distribution, Vulkan driver version, the failing step, and a sanitized error message. Also state whether `--check` accepted the dump. Never send the dump or files produced from it.

### Licenses, credits, and trademarks

The combined work is distributed under **GPL-3.0-or-later**. Third-party components retain their individual licenses; their texts and notices are included in `LICENSES/` and `THIRD-PARTY-NOTICES.md`. `zbic-unpack` is a separate GPL-2.0-only process. Review the notices before redistributing or modifying the package.

Credits: **pokopia-native** (xFractals), **nxnative** (seedds), **Ryujinx**, **Eden-NS2**, **zbic** (kinnay), and related contributions/behavior from **Lichii**, **Eglantine-fairtech**, and **Orchid**. Source files retain their headers and attributions.

The Legend of Zelda, Ocarina of Time, and Nintendo Switch are trademarks of their respective rights holders. The game name is used only to identify compatibility. This is an independent project, not affiliated with, sponsored by, or endorsed by Nintendo. The package includes no Nintendo logos, icons, or images.

## Português

OoT Native recompila localmente o código de **The Legend of Zelda: Ocarina of Time** para um programa Linux nativo. Esta versão é compatível com a edição Nintendo Switch 2, título `0400486024DEC000`, atualização 2.0.0. O pacote traz o launcher, o builder e as ferramentas; **não contém arquivos do jogo**. O launcher valida e usa somente um dump `.nup` fornecido por você de um jogo que possui. Não descriptografa nem converte dumps.

**Status: WIP.** A recompilação e a inicialização foram exercitadas neste release, mas compatibilidade e jogabilidade continuam incompletas. Gráficos, áudio, controles, vídeos, shaders e progressão podem apresentar falhas. Não trate esta versão como um produto acabado.

### Requisitos medidos

- Linux x86-64, CPU com AVX2 e FMA3.
- GPU/driver Vulkan 1.3.
- glibc `GLIBC_2.35` ou superior e libstdc++ com `GLIBCXX_3.4.30` ou superior.
- O pacote inclui o runtime .NET 10; o launcher ativa globalização invariante, sem exigir .NET ou ICU instalados no sistema nem toolchains de desenvolvimento.
- Reserve pelo menos **45 GB livres** no disco da pasta de build. A pasta de teste ocupou 38 GB.
- No teste com 2 jobs, o tempo total medido foi de aproximadamente **2h25** e o pico de memória foi **4,5 GB RSS**. O tempo soma a primeira tentativa (35m17s, interrompida por uma falha já corrigida) e a retomada bem-sucedida (1h49m54s). O tempo e a memória acima foram medidos com **2 jobs**, a configuração fixa dos releases anteriores, e não com o novo padrão. Com mais jobs a build deve terminar antes e usar mais RAM; não foram medidos números para outras quantidades de jobs.

O v0.1.5 inclui FFmpeg 7.1.1 com dav1d 1.5.1 para decodificar vídeos AV1. A decodificação fica ativa por padrão; defina `NXRT_MOVIE_DECODE=0` para usar o fallback temporizado sem imagem. Os tarballs exatos do código-fonte estão no arquivo adjacente `oot-native-linux-x86_64-v0.1.5-2026-11-07-thirdparty-source.tar.xz`. `TESTER-v0.1.5.md` diz se esta versão pode reutilizar um build existente ou precisa de um novo, e como testá-la.

### Limitação conhecida de shaders

No build de referência do pacote, 25 de 41.632 shaders falharam na tradução e 17 falharam na validação SPIR-V; os draws associados são ignorados. As 25 falhas de tradução coincidem com as registradas em uma execução de referência anterior. As falhas de validação são da mesma classe `Offset` observada em relatórios de referência anteriores; outra execução de referência registrou 83 falhas de validação, de tipos mistos e em um conjunto diferente. As contagens variam entre execuções e conjuntos de shaders. Isso pode deixar partes dos gráficos incompletas.

### Baixar, verificar e iniciar

Baixe `oot-native-linux-x86_64-v0.1.5-2026-11-07.tar.xz` e compare o resultado de `sha256sum` com o SHA-256 publicado junto ao arquivo. Depois extraia e valide os arquivos do pacote:

```sh
sha256sum oot-native-linux-x86_64-v0.1.5-2026-11-07.tar.xz
tar -xf oot-native-linux-x86_64-v0.1.5-2026-11-07.tar.xz
cd oot-native-linux-x86_64-v0.1.5-2026-11-07
sha256sum -c SHA256SUMS
./oot-native-launcher
```

O idioma inicial do launcher é inglês. Escolha **Português (Brasil)** na GUI; a escolha é salva em `${XDG_CONFIG_HOME:-~/.config}/oot-native/launcher.json` e passa a valer na próxima inicialização. Na CLI, use `--lang en` ou `--lang pt-BR`, ou defina `OOT_NATIVE_LANG=en|pt-BR`. O locale do sistema não altera o idioma padrão.

No launcher gráfico, escolha **o seu próprio dump `.nup`**, deixe a validação terminar e escolha uma pasta de build vazia. A construção e a execução acontecem localmente. Para usar pela linha de comando:

```sh
./oot-native-launcher --check "/caminho/do/seu-dump.nup"
./oot-native-launcher --build "/caminho/do/seu-dump.nup" --out "$HOME/Games/oot-native-build"
"$HOME/Games/oot-native-build/out/play-oot-2.0.0"
```

**Jobs da build.** A build roda em paralelo e, por padrão, usa **90% das threads disponíveis** (arredondado para baixo, no mínimo 1). "Disponíveis" é o número de CPUs em que o processo pode rodar (a affinity de CPU, então `taskset` e containers são respeitados), reduzido ainda mais se uma cota de CPU do cgroup v2 (`/sys/fs/cgroup/cpu.max`) permitir menos. Mude com `--jobs N` ou `OOT_NATIVE_JOBS=N`, por exemplo `./oot-native-launcher --build "/caminho/do/seu-dump.nup" --out "$HOME/Games/oot-native-build" --jobs 6`. Na GUI, use o campo **Jobs da build**; ele mostra o padrão e "Usando N de M threads", e a escolha é salva no `launcher.json`, junto com o idioma. A ordem é: `--jobs`, depois `OOT_NATIVE_JOBS`, depois o valor salvo pela GUI, depois o padrão de 90%. `N` deve ser um inteiro maior ou igual a 1; um valor acima das threads disponíveis é aceito com um aviso. A build sempre roda com prioridade baixa (`nice -n 19`, `ionice -c3`). O padrão é uma carga longa em quase todas as threads; se a máquina esquentar ou ficar instável, escolha menos jobs. A etapa de shaders da build usa no máximo 8 workers, qualquer que seja o valor.

O launcher não altera o dump nem baixa arquivos. **Nunca compartilhe a pasta de build**: ela contém código recompilado, arquivos extraídos e outros dados derivados do seu jogo.

### Saves e logs

Saves, logs de execução e cache Vulkan ficam em `${XDG_DATA_HOME:-$HOME/.local/share}/oot-native/2.0.0/`. Os logs da construção ficam dentro da pasta de build. Antes de enviar um log para relatar um problema, remova caminhos locais, nomes de arquivos pessoais e qualquer dado derivado do jogo. Não anexe a pasta de build, dumps, saves, shaders ou capturas de tela.

### Problemas

Inclua a versão do pacote, a distribuição Linux, a versão do driver Vulkan, a etapa que falhou e uma mensagem de erro sanitizada. Relate também se `--check` aceitou o dump. Não envie o dump nem arquivos produzidos a partir dele.

### Licenças, créditos e marcas

O conjunto é distribuído sob **GPL-3.0-or-later**. Componentes de terceiros mantêm suas licenças individuais; os textos e avisos acompanham o pacote em `LICENSES/` e `THIRD-PARTY-NOTICES.md`. `zbic-unpack` é um processo separado sob GPL-2.0-only. Consulte os avisos antes de redistribuir ou modificar o pacote.

Créditos: **pokopia-native** (xFractals), **nxnative** (seedds), **Ryujinx**, **Eden-NS2**, **zbic** (kinnay), e contribuições/comportamentos relacionados a **Lichii**, **Eglantine-fairtech** e **Orchid**. Os arquivos de origem preservam seus cabeçalhos e atribuições.

The Legend of Zelda, Ocarina of Time e Nintendo Switch são marcas de seus titulares. O nome do jogo é usado apenas para identificar a compatibilidade. Este é um projeto independente e não é afiliado, patrocinado nem endossado pela Nintendo. O pacote não inclui logos, ícones ou imagens da Nintendo.
