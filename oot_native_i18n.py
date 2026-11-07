"""English-first strings shared by the launcher and the packaged builder."""
from __future__ import annotations

import os
import re

DEFAULT_LANGUAGE = "en"
LANGUAGES = ("en", "pt-BR")

STRINGS = {
    "en": {
        "app_title": "OoT Native for Linux",
        "subtitle": "Choose your own .nup dump to build and run locally.",
        "dump_label": "Game dump (.nup)",
        "build_folder_label": "Build folder",
        "language_label": "Language",
        "browse": "Browse…",
        "dump_filetype": "NUP dump",
        "all_files": "All files",
        "language_saved": "Language saved. Restart the launcher to apply it.",
        "language_save_failed": "The language could not be saved. The current preference is unchanged.",
        "language_en": "English",
        "language_pt": "Português (Brasil)",
        "legal_notice": "Use only a dump of your own game. This package contains no game files. Never share the build folder.",
        "space_notice": "Reserve at least 45 GB free on the build disk; the measured build used 38 GB.",
        "options_title": "Run options",
        "fullscreen": "Fullscreen",
        "window_label": "Window:",
        "silent": "Mute audio",
        "gamemode": "GameMode (if installed)",
        "build_priority": "Build uses {jobs} job(s) at low priority.",
        "jobs_label": "Build jobs:",
        "jobs_summary": "Using {jobs} of {threads} threads",
        "jobs_default_hint": "Default: {default} (90% of available threads). More jobs finish sooner and use more RAM.",
        "jobs_invalid_title": "Invalid build jobs",
        "jobs_invalid_gui": "Enter a whole number of 1 or more for Build jobs.",
        "jobs_ignored_env": "OOT_NATIVE_JOBS ignored: {value!r} is not a whole number of 1 or more.",
        "build_button": "Build",
        "run_button": "Run",
        "status_ready": "Ready",
        "status_checking": "Validating dump",
        "status_building": "Building",
        "status_running": "Running",
        "status_completed": "Completed",
        "status_failed_code": "Failed (exit code {code})",
        "status_failed_start": "Could not start",
        "output_footer": "The builder and tools are included. Game-derived files are created in the selected build folder.",
        "error_title_builder": "Builder not found",
        "error_builder_not_found": "The builder is missing from this package: {path}",
        "error_dump_title": "Dump rejected",
        "error_invalid_file": "Choose an existing .nup file. The launcher does not copy or modify it.",
        "error_path_title": "Invalid folder",
        "error_path_absolute": "Choose an absolute path for the build folder.",
        "error_python_title": "Python unavailable",
        "error_python_missing": "The bundled Python 3.12+ runtime is unavailable.",
        "error_python_version": "Python 3.12 or newer is required; {path} reported {version}.",
        "error_python_start": "Could not run the builder's Python interpreter ({path}): {error}",
        "error_doctor_title": "Requirements",
        "error_doctor_unavailable": "The release doctor could not run: {error}",
        "error_doctor_failed": "Required items are missing:",
        "error_build_folder_title": "Build folder rejected",
        "error_run_title": "Build unavailable",
        "error_runtime_unavailable": "The generated game launcher is not available in this build.",
        "error_gamemode_title": "GameMode unavailable",
        "error_gamemode_missing": "Install GameMode or turn this option off.",
        "error_message_prefix": "Error: {error}",
        "error_folder_not_empty": "The build folder is not empty and was not created by this launcher. Choose an empty folder: {path}",
        "error_marker_invalid": "The launcher build marker is invalid: {path}",
        "error_version_missing": "The build version file could not be read: {error}",
        "error_version_invalid": "The build version file contains an invalid value.",
        "error_runtime_not_found": "The generated game launcher was not found or is not executable: {path}",
        "error_process_start": "The process could not be started.",
        "error_build_unavailable": "The build could not be prepared. Check the selected folder and package files.",
        "dump.accepted": "Dump accepted: OoT {version}. The .nup was not modified.",
        "dump.rejected": "Dump rejected: {reason}",
        "dump.warning_count": "Accepted with {count} optional shader-path warning(s).",
        "dump.invalid_format": "This file is not a valid .nup dump. The launcher does not decrypt or convert other formats.",
        "dump.encrypted": "This dump is encrypted or cannot be read. Use a valid, readable dump of OoT 2.0.0.",
        "dump.unreadable": "The dump could not be opened. Check the file and its permissions.",
        "dump.wrong_game_version": "This dump is for another game or version. This package expects OoT Switch 2 version 2.0.0.",
        "dump.incomplete": "The dump is incomplete. Use the full game dump with the 2.0.0 update.",
        "dump.corrupt": "The dump is corrupt or truncated and could not be validated.",
        "dump.invalid": "The dump could not be validated. Use a complete OoT Switch 2 2.0.0 dump.",
        "cli_description": "Build OoT Native from your own game dump.",
        "cli_help_check": "validate a .nup dump without changing it",
        "cli_help_build": "validate and build from a .nup dump",
        "cli_help_out": "build output directory",
        "cli_help_jobs": "parallel build jobs, a whole number of 1 or more (default: 90% of available threads; env: OOT_NATIVE_JOBS)",
        "cli_help_lang": "interface language (default: English)",
        "cli_help_help": "show this help message and exit",
        "cli_usage": "Usage",
        "cli_options": "Options",
        "cli_build_requires_out": "--build requires --out DIR",
        "cli_dump_accepted": "Dump accepted: OoT {version}. The .nup was not modified.",
        "cli_doctor_ready": "Requirements check passed.",
        "cli_doctor_missing": "Requirements check failed:",
        "cli_build_failed": "Build failed with exit code {code}.",
        "cli_argument_error": "Invalid command-line arguments: {error}",
        "doctor.cpu_avx2_fma3": "x86-64 CPU with AVX2 and FMA3",
        "doctor.vulkan_1_3": "Vulkan 1.3 graphics driver",
        "doctor.ttf_font": "Unicode TrueType font",
        "doctor.space_45gb": "At least 45 GB free (38 GB measured build)",
        "doctor.detail_cpu": "A CPU with AVX2 and FMA3 is required.",
        "doctor.detail_vulkan": "Install or update a Vulkan 1.3 graphics driver.",
        "doctor.detail_font": "Install a TrueType font or provide one with STD_FONT.",
        "doctor.detail_space": "45 GB is required; {available} GB is available.",
        "doctor.detail_generic": "This requirement is not available.",
        "doctor.ok": "ready",
        "doctor.resources": "Threads: {threads}; default build jobs: {default}; jobs for this build: {jobs}; RAM available: {ram}",
        "doctor.ram_unknown": "unknown",
        "doctor.warn_ram": "Warning: {jobs} jobs may not fit in the available RAM ({ram}); use fewer with --jobs.",
        "jobs.invalid": "Invalid {source} value {value!r}: use a whole number of 1 or more.",
        "jobs.warn_over_threads": "Warning: {jobs} jobs requested but only {threads} threads are available; the build may be slower.",
        "jobs.warn_over_memory": "Warning: {jobs} jobs exceed the memory ceiling of {cap} (85% of available RAM); using {jobs} as requested.",
        "jobs.log_capped_memory": "Default jobs lowered from {cpu_jobs} to {jobs} to stay within 85% of available RAM.",
        "doctor.missing": "missing",
        "builder_progress_generic": "The build is still running.",
        "builder_tool_failed": "Tool {tool} failed with exit code {code}. See the build log: {log}",
        "builder_failed_generic": "The build failed. Check the build logs for details.",
        "builder_interrupted": "Build interrupted.",
    },
    "pt-BR": {
        "app_title": "OoT Native para Linux",
        "subtitle": "Escolha seu próprio dump .nup para construir e executar localmente.",
        "dump_label": "Dump do jogo (.nup)",
        "build_folder_label": "Pasta de build",
        "language_label": "Idioma",
        "browse": "Escolher…",
        "dump_filetype": "Dump NUP",
        "all_files": "Todos os arquivos",
        "language_saved": "Idioma salvo. Reinicie o launcher para aplicar.",
        "language_save_failed": "Não foi possível salvar o idioma. A preferência atual não mudou.",
        "language_en": "English",
        "language_pt": "Português (Brasil)",
        "legal_notice": "Use apenas um dump do seu próprio jogo. Este pacote não contém arquivos do jogo. Nunca compartilhe a pasta de build.",
        "space_notice": "Reserve pelo menos 45 GB livres no disco da build; a build medida ocupou 38 GB.",
        "options_title": "Opções de execução",
        "fullscreen": "Tela cheia",
        "window_label": "Janela:",
        "silent": "Sem áudio",
        "gamemode": "GameMode (se instalado)",
        "build_priority": "A build usa {jobs} job(s), com prioridade baixa.",
        "jobs_label": "Jobs da build:",
        "jobs_summary": "Usando {jobs} de {threads} threads",
        "jobs_default_hint": "Padrão: {default} (90% das threads disponíveis). Mais jobs terminam antes e usam mais RAM.",
        "jobs_invalid_title": "Jobs da build inválidos",
        "jobs_invalid_gui": "Digite um número inteiro maior ou igual a 1 em Jobs da build.",
        "jobs_ignored_env": "OOT_NATIVE_JOBS ignorada: {value!r} não é um número inteiro maior ou igual a 1.",
        "build_button": "Construir",
        "run_button": "Executar",
        "status_ready": "Pronto",
        "status_checking": "Validando o dump",
        "status_building": "Construindo",
        "status_running": "Executando",
        "status_completed": "Concluído",
        "status_failed_code": "Falhou (código de saída {code})",
        "status_failed_start": "Não foi possível iniciar",
        "output_footer": "O builder e as ferramentas estão incluídos. Os arquivos derivados do jogo são criados na pasta de build escolhida.",
        "error_title_builder": "Builder não encontrado",
        "error_builder_not_found": "O builder não está neste pacote: {path}",
        "error_dump_title": "Dump rejeitado",
        "error_invalid_file": "Escolha um arquivo .nup existente. O launcher não o copia nem modifica.",
        "error_path_title": "Pasta inválida",
        "error_path_absolute": "Escolha um caminho absoluto para a pasta de build.",
        "error_python_title": "Python indisponível",
        "error_python_missing": "O Python 3.12+ incluído no pacote não está disponível.",
        "error_python_version": "É necessário Python 3.12 ou mais recente; {path} informou {version}.",
        "error_python_start": "Não foi possível executar o Python do builder ({path}): {error}",
        "error_doctor_title": "Pré-requisitos",
        "error_doctor_unavailable": "Não foi possível executar o doctor do release: {error}",
        "error_doctor_failed": "Itens necessários ausentes:",
        "error_build_folder_title": "Pasta de build recusada",
        "error_run_title": "Build indisponível",
        "error_runtime_unavailable": "O launcher do jogo não está disponível nesta build.",
        "error_gamemode_title": "GameMode indisponível",
        "error_gamemode_missing": "Instale o GameMode ou desative esta opção.",
        "error_message_prefix": "Erro: {error}",
        "error_folder_not_empty": "A pasta de build não está vazia e não foi criada por este launcher. Escolha uma pasta vazia: {path}",
        "error_marker_invalid": "O marcador da build é inválido: {path}",
        "error_version_missing": "Não foi possível ler o arquivo de versão da build: {error}",
        "error_version_invalid": "O arquivo de versão da build contém um valor inválido.",
        "error_runtime_not_found": "O launcher gerado para o jogo não foi encontrado ou não é executável: {path}",
        "error_process_start": "Não foi possível iniciar o processo.",
        "error_build_unavailable": "Não foi possível preparar a build. Confira a pasta escolhida e os arquivos do pacote.",
        "dump.accepted": "Dump aceito: OoT {version}. O .nup não foi alterado.",
        "dump.rejected": "Dump rejeitado: {reason}",
        "dump.warning_count": "Dump aceito com {count} aviso(s) sobre caminhos opcionais de shaders.",
        "dump.invalid_format": "Este arquivo não é um dump .nup válido. O launcher não descriptografa nem converte outros formatos.",
        "dump.encrypted": "O dump está criptografado ou não pode ser lido. Use um dump válido e legível de OoT 2.0.0.",
        "dump.unreadable": "Não foi possível abrir o dump. Confira o arquivo e suas permissões.",
        "dump.wrong_game_version": "Este dump é de outro jogo ou versão. Este pacote espera OoT Switch 2 versão 2.0.0.",
        "dump.incomplete": "O dump está incompleto. Use o dump completo do jogo com a atualização 2.0.0.",
        "dump.corrupt": "O dump está corrompido ou truncado e não pôde ser validado.",
        "dump.invalid": "Não foi possível validar o dump. Use um dump completo de OoT Switch 2 2.0.0.",
        "cli_description": "Construa o OoT Native usando o seu próprio dump do jogo.",
        "cli_help_check": "valida um dump .nup sem alterá-lo",
        "cli_help_build": "valida e constrói usando um dump .nup",
        "cli_help_out": "pasta de saída da build",
        "cli_help_jobs": "jobs paralelos da build, inteiro maior ou igual a 1 (padrão: 90% das threads disponíveis; variável: OOT_NATIVE_JOBS)",
        "cli_help_lang": "idioma da interface (padrão: inglês)",
        "cli_help_help": "mostrar esta ajuda e sair",
        "cli_build_requires_out": "--build requer --out DIR",
        "cli_dump_accepted": "Dump aceito: OoT {version}. O .nup não foi alterado.",
        "cli_doctor_ready": "Verificação de requisitos concluída.",
        "cli_doctor_missing": "Falha na verificação de requisitos:",
        "cli_build_failed": "A build falhou com código de saída {code}.",
        "cli_argument_error": "Argumentos inválidos; use --help para ver as opções.",
        "cli_usage": "Uso",
        "cli_options": "Opções",
        "doctor.cpu_avx2_fma3": "CPU x86-64 com AVX2 e FMA3",
        "doctor.vulkan_1_3": "Driver gráfico Vulkan 1.3",
        "doctor.ttf_font": "Fonte TrueType Unicode",
        "doctor.space_45gb": "Pelo menos 45 GB livres (build medida: 38 GB)",
        "doctor.detail_cpu": "É necessária uma CPU com AVX2 e FMA3.",
        "doctor.detail_vulkan": "Instale ou atualize um driver gráfico Vulkan 1.3.",
        "doctor.detail_font": "Instale uma fonte TrueType ou indique uma com STD_FONT.",
        "doctor.detail_space": "São necessários 45 GB; há {available} GB livres.",
        "doctor.detail_generic": "Este requisito não está disponível.",
        "doctor.ok": "pronto",
        "doctor.resources": "Threads: {threads}; jobs padrão da build: {default}; jobs desta build: {jobs}; RAM disponível: {ram}",
        "doctor.ram_unknown": "desconhecida",
        "doctor.warn_ram": "Aviso: {jobs} jobs podem não caber na RAM disponível ({ram}); use menos com --jobs.",
        "jobs.invalid": "Valor inválido em {source}: {value!r}. Use um número inteiro maior ou igual a 1.",
        "jobs.warn_over_threads": "Aviso: {jobs} jobs pedidos, mas só há {threads} threads disponíveis; a build pode ficar mais lenta.",
        "jobs.warn_over_memory": "Aviso: {jobs} jobs passam do teto de memória de {cap} (85% da RAM disponível); usando {jobs}, como pedido.",
        "jobs.log_capped_memory": "Jobs padrão reduzidos de {cpu_jobs} para {jobs} para ficar dentro de 85% da RAM disponível.",
        "doctor.missing": "ausente",
        "builder_progress_generic": "A build continua em andamento.",
        "builder_tool_failed": "A ferramenta {tool} falhou com código de saída {code}. Consulte o log da build: {log}",
        "builder_failed_generic": "A build falhou. Consulte os logs da build para obter detalhes.",
        "builder_interrupted": "Build interrompida.",
    },
}

assert set(STRINGS["en"]) == set(STRINGS["pt-BR"])


def normalize_language(value: str | None) -> str | None:
    if not value:
        return None
    value = value.strip().replace("_", "-").lower()
    if value in ("en", "english"):
        return "en"
    if value in ("pt-br", "pt", "português (brasil)", "portugues (brasil)"):
        return "pt-BR"
    return None


def resolve_language(cli: str | None = None, environ: dict[str, str] | None = None,
                     config: dict[str, object] | None = None) -> str:
    """Resolve explicit CLI, explicit environment, saved preference, then English."""
    environ = os.environ if environ is None else environ
    config = {} if config is None else config
    return (normalize_language(cli)
            or normalize_language(environ.get("OOT_NATIVE_LANG"))
            or normalize_language(str(config.get("language", "")))
            or DEFAULT_LANGUAGE)


def tr(language: str, key: str, **values: object) -> str:
    catalog = STRINGS.get(normalize_language(language) or DEFAULT_LANGUAGE, STRINGS[DEFAULT_LANGUAGE])
    return catalog.get(key, key).format(**values)


_STATIC_BUILDER_PT = {
    "building dmsh_xlate from the patched nxnative source (.NET 10)": "Compilando dmsh_xlate a partir do código nxnative corrigido (.NET 10)",
    "applying the Switch 2 host patch": "Aplicando o patch do host Switch 2",
    "building ampere-lower (the Switch 2 shader lowering)": "Compilando ampere-lower (redução dos shaders do Switch 2)",
    "building the recompiler and the runtime (windows/src)": "Compilando o recompilador e o runtime",
    "fetching the Vulkan headers": "Baixando os headers do Vulkan",
    "analysing the game's code (nxanalyze)": "Analisando o código do jogo (nxanalyze)",
    "reading the executable's data (nso_tool.py)": "Lendo os dados do executável",
    "extracting the shaders of the EPD containers (epd_shaders.py)": "Extraindo shaders dos contêineres EPD",
    "lowering the shaders (ampere-lower)": "Convertendo os shaders (ampere-lower)",
    "packaging the shaders (s2_pkg.py)": "Empacotando os shaders",
    "analysis: up to date": "Análise já atualizada",
    "corpus: up to date": "Catálogo de shaders já atualizado",
    "RomFS image: up to date": "Imagem RomFS já atualizada",
    "game.so: up to date": "game.so já atualizado",
    "preflight: spirv-opt, spirv-val, ld.lld, ampere-lower and zbic-unpack ready": "Pré-checagem concluída: spirv-opt, spirv-val, ld.lld, ampere-lower e zbic-unpack prontos",
    "done": "Concluído",
    "done (analyze + lift only)": "Concluído (somente análise e recompilação)",
}

_BUILDER_PATTERNS_PT = (
    (re.compile(r"^warning: optional RomFS glob matched no files: (.+)$"), "Aviso: o padrão opcional do RomFS não encontrou arquivos: {0}"),
    (re.compile(r"^preflight: dmsh_xlate source SHA ([0-9a-f]+)$"), "Pré-checagem do dmsh_xlate concluída (SHA do código-fonte: {0})"),
    (re.compile(r"^fetching LLVM (.+)$"), "Baixando LLVM {0}"),
    (re.compile(r"^building SDL3 (.+)$"), "Compilando SDL3 {0}"),
    (re.compile(r"^fetching nxnative \(([^)]+)\).*$"), "Baixando o nxnative fixado em {0}"),
    (re.compile(r"^checking out nxnative (.+)$"), "Selecionando a versão fixada do nxnative ({0})"),
    (re.compile(r"^resetting nxnative .*$"), "Restaurando a versão fixada do nxnative"),
    (re.compile(r"^setup done:.*$"), "Configuração concluída; ferramentas do release prontas"),
    (re.compile(r"^RomFS image: (\d+) % of ([0-9.]+) GB$"), "Imagem RomFS: {0}% de {1} GB"),
    (re.compile(r"^RomFS image from .*$"), "Preparando a imagem RomFS"),
    (re.compile(r"^extracting the shaders of (\d+) sources$"), "Extraindo shaders de {0} fontes"),
    (re.compile(r"^  extracted .*$"), "Fonte de shader extraída"),
    (re.compile(r"^extracting the shaders of the EPD containers.*$"), "Extraindo shaders dos contêineres EPD"),
    (re.compile(r"^adding shader dumps from (\d+) folder\(s\)$"), "Adicionando dumps de shader de {0} pastas"),
    (re.compile(r"^tables: (\d+) facts rows \((\d+) hand-made\), shader map (\d+) entries$"), "Tabelas: {0} linhas de dados ({1} manuais), mapa de shaders com {2} entradas"),
    (re.compile(r"^starting (.+)$"), "Iniciando {0}"),
    (re.compile(r"^OoT Native builder (.+)$"), "Builder OoT Native {0}"),
    (re.compile(r"^load: (\d+) worker\(s\) of (\d+) CPU threads, low priority.*$"), "Usando {0} workers de {1} threads da CPU, com prioridade baixa"),
    (re.compile(r"^WARNING: ([0-9.]+) GB free where the build folder is;.*$"), "AVISO: há {0} GB livres no disco; reserve pelo menos 45 GB (a build medida ocupou 38G)"),
    (re.compile(r"^preflight: .*failed.*$"), "Falha na pré-checagem das ferramentas do pacote"),
    (re.compile(r"^(\d+) shaders failed validation:.*$"), "{0} shaders falharam na validação; os draws associados foram ignorados"),
    (re.compile(r"^translating the shaders for Vulkan \(nxshader, (\d+) workers?\)$"), "Traduzindo shaders para Vulkan (nxshader, {0} workers)"),
    (re.compile(r"^recompiling the game's code for this PC \(nxlift, (\d+) workers? at the lowest priority\)$"), "Recompilando o código do jogo para este PC (nxlift, {0} workers, prioridade baixa)"),
    (re.compile(r"^nxlift detached: pid (\d+), log (.+)$"), "nxlift em segundo plano: processo {0}, log {1}"),
    (re.compile(r"^Linux game: (.+)$"), "Jogo Linux: {0}"),
    (re.compile(r"^the Linux game: run (.+)$"), "Jogo Linux pronto para executar: {0}"),
    (re.compile(r"^WARNING:.*$"), "Aviso: confira os requisitos de espaço da build"),
)


def translate_builder_message(message: str, language: str) -> str:
    language = normalize_language(language) or DEFAULT_LANGUAGE
    if language == "en":
        return message
    if message in _STATIC_BUILDER_PT:
        return _STATIC_BUILDER_PT[message]
    for pattern, template in _BUILDER_PATTERNS_PT:
        match = pattern.match(message)
        if match:
            return template.format(*match.groups())
    return tr(language, "builder_progress_generic")


def translate_builder_error(message: str, language: str) -> str:
    language = normalize_language(language) or DEFAULT_LANGUAGE
    if language == "en":
        return message
    match = re.match(r"([^ ]+) failed \((-?\d+)\); log: ([^\n]+)", message)
    if match:
        localized = tr(language, "builder_tool_failed", tool=match.group(1), code=match.group(2), log=match.group(3))
    else:
        localized = tr(language, "builder_failed_generic")
    # Preserve the builder's original technical line in every locale so users
    # can report the same actionable cause in English or Portuguese.
    return f"{localized}\n{message}"


def dump_error_key(code: str | None) -> str:
    allowed = {"invalid_format", "encrypted", "unreadable", "wrong_game_version", "incomplete", "corrupt", "invalid"}
    return "dump." + (code if code in allowed else "invalid")


def doctor_label_key(code: str | None) -> str:
    allowed = {"cpu_avx2_fma3", "vulkan_1_3", "ttf_font", "space_45gb"}
    return "doctor." + (code if code in allowed else "detail_generic")


def doctor_detail_key(code: str | None) -> str:
    return {
        "cpu_avx2_fma3": "doctor.detail_cpu",
        "vulkan_1_3": "doctor.detail_vulkan",
        "ttf_font": "doctor.detail_font",
        "space_45gb": "doctor.detail_space",
    }.get(code or "", "doctor.detail_generic")
