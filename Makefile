# Release entry points. See release/PIPELINE.md. Nothing here prompts; every target fails closed.
#   make release VERSION=1.2.3 PREV=<previous package .tar.xz> OUT=<work dir on a large disk>
#   make verify-repro VERSION=1.2.3 OUT=<work dir>
#   make publish VERSION=1.2.3 OUT=<work dir> [PUBLISH=1]   (dry-run unless PUBLISH=1)
#   make e2e DUMP=<your .nup> [NEG_DUMP=<other game's .nup>] [BUILD_DIR=<dir>] [RUN=1]   (local only, never CI)
# Persistent local defaults (OUT=...) can live in release/local.mk, which is git-ignored.
-include release/local.mk

OUT ?= $(OOT_RELEASE_WORK)
PREV ?=
PREV_ABI ?=
JOBS ?=
ALLOW_ABI_BREAK ?= 0
KEEP_WORK ?= 0
PUBLISH ?= 0
export VERSION OUT PREV PREV_ABI ALLOW_ABI_BREAK KEEP_WORK PUBLISH
ifneq ($(strip $(JOBS)),)
export JOBS
endif

.PHONY: help release verify-repro publish e2e test-release

help:
	@sed -n '2,5p' Makefile

release:
	@test -n "$(VERSION)" || { echo "usage: make release VERSION=x.y.z PREV=<previous .tar.xz|none> OUT=<dir>" >&2; exit 2; }
	release/pipeline.sh release

verify-repro:
	@test -n "$(VERSION)" || { echo "usage: make verify-repro VERSION=x.y.z OUT=<dir>" >&2; exit 2; }
	release/pipeline.sh verify-repro

publish:
	@test -n "$(VERSION)" || { echo "usage: make publish VERSION=x.y.z OUT=<dir> [PUBLISH=1]" >&2; exit 2; }
	PUBLISH=$(PUBLISH) release/publish.sh

e2e:
	release/e2e.sh

test-release:
	python3 -m unittest discover -s tests -p 'test_release*.py' -v
