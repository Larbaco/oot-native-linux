#!/usr/bin/env python3
"""Fill the per-release placeholders of the documentation from VERSION, and refuse any other stale version.

  render_docs.py render <src> <dst> --version V --pkg-name N     fill the placeholders of one file
  render_docs.py render-tree <root> --version V --pkg-name N     fill them in place in the documentation set under <root>
  render_docs.py check <root> --version V --pkg-name N           fail if text cites a file name of another release

Placeholders: @VERSION@ @TAG@ @PKG_NAME@ @PACKAGE_ARCHIVE@ @SOURCE_ARCHIVE@ @THIRDPARTY_SOURCE_ARCHIVE@ @TESTER_GUIDE@
(@GLIBC_BASELINE@ and @GLIBCXX_BASELINE@ are filled later, from the measured package, and are left alone here.)
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

DOC_FILES = ('README.md', 'README-RELEASE.md', 'THIRD-PARTY-SOURCE.md', 'release/README-RELEASE.md', 'release/THIRD-PARTY-SOURCE.md')
# Unconfirmed claims about the testers' platform. Applied ONLY to the tester guides (TESTER-v*.md): elsewhere the term is a legitimate
# mention (compatibility notes, research) and must not be blocked.
FORBIDDEN_TERMS = ('SteamOS',)
TERM_FILES = re.compile(r'(?:^|/)TESTER-v[\w.\-]+\.md$')
# Placeholders that are filled after rendering (from the measured package) may remain only in these templates, and only in the source tree.
LATER = {'GLIBC_BASELINE', 'GLIBCXX_BASELINE'}
TOKEN = re.compile(r'@([A-Z][A-Z0-9_]*)@')
SKIP_DIRS = {'python', 'dotnet', 'LICENSES', 'upstream', 'third_party', '.git'}
ARCHIVE_NAME = re.compile(r'oot-native-linux-x86_64-v\d+\.\d+\.\d+[\w.\-]*')
TESTER_NAME = re.compile(r'TESTER-v[\w.\-]+?\.md')


def base_version(version: str) -> str:
    return version.split('-', 1)[0]


def values(version: str, pkg_name: str) -> dict[str, str]:
    return {
        'VERSION': version, 'TAG': 'v' + version, 'PKG_NAME': pkg_name,
        'PACKAGE_ARCHIVE': pkg_name + '.tar.xz', 'SOURCE_ARCHIVE': pkg_name + '-source.tar.xz',
        'THIRDPARTY_SOURCE_ARCHIVE': pkg_name + '-thirdparty-source.tar.xz',
        'TESTER_GUIDE': f'TESTER-v{base_version(version)}.md',
    }


def render_text(text: str, version: str, pkg_name: str, name: str = '<text>') -> str:
    vals = values(version, pkg_name)

    def sub(m: re.Match) -> str:
        key = m.group(1)
        if key in LATER:
            return m.group(0)
        if key not in vals:
            raise SystemExit(f'FAIL: {name}: unknown placeholder @{key}@')
        return vals[key]
    return TOKEN.sub(sub, text)


def doc_set(root: pathlib.Path, version: str) -> list[pathlib.Path]:
    docs = [root / d for d in DOC_FILES]
    docs.append(root / values(version, '')['TESTER_GUIDE'])
    return [d for d in docs if d.is_file()]


def render_tree(root: pathlib.Path, version: str, pkg_name: str) -> list[str]:
    done = []
    for doc in doc_set(root, version):
        text = doc.read_text(encoding='utf-8')
        out = render_text(text, version, pkg_name, str(doc.relative_to(root)))
        if out != text:
            doc.write_text(out, encoding='utf-8')
        done.append(doc.relative_to(root).as_posix())
    return done


def text_files(root: pathlib.Path):
    for p in sorted(root.rglob('*')):
        if p.is_symlink() or not p.is_file() or p.suffix.lower() not in {'.md', '.txt', '.rst'}:
            continue
        rel = p.relative_to(root)
        if rel.parts and rel.parts[0] in SKIP_DIRS:
            continue
        if p.name.startswith(('LICENSE', 'COPYING', 'NOTICE')) or p.name in {'PACKAGE-MANIFEST.txt'}:
            continue
        yield p, rel.as_posix()


def check_tree(root: pathlib.Path, version: str, pkg_name: str, allow_templates: tuple[str, ...] = ()) -> list[str]:
    """Any @NAME@ left in text is a failure, except the baseline placeholders in the templates named in allow_templates."""
    vals = values(version, pkg_name)
    problems = []
    for path, rel in text_files(root):
        try:
            text = path.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            continue
        for m in TOKEN.finditer(text):
            if m.group(1) in LATER and rel in allow_templates:
                continue
            problems.append(f'{rel}: unfilled placeholder {m.group(0)}')
        for term in (FORBIDDEN_TERMS if TERM_FILES.search(rel) else ()):
            if term.lower() in text.lower():
                problems.append(f'{rel}: mentions {term}, which is not a confirmed platform for testers')
        for m in ARCHIVE_NAME.finditer(text):
            cited = m.group(0).rstrip('.-')
            if not cited.startswith(pkg_name):
                problems.append(f'{rel}: cites another release file name: {cited}')
        for m in TESTER_NAME.finditer(text):
            if m.group(0) != vals['TESTER_GUIDE']:
                problems.append(f'{rel}: cites another tester guide: {m.group(0)}')
    return problems


def check_tester_guide(root: pathlib.Path, version: str) -> list[str]:
    guide = values(version, '')['TESTER_GUIDE']
    problems = []
    if not (root / guide).is_file():
        problems.append(f'the tester guide {guide} does not exist at the repository root')
    allow = root / 'release' / 'allowlist.txt'
    if allow.is_file():
        lines = {l.strip() for l in allow.read_text().splitlines()}
        if guide not in lines:
            problems.append(f'{guide} is not in release/allowlist.txt')
        others = sorted(l for l in lines if re.fullmatch(r'TESTER-v.*\.md', l) and l != guide)
        if others:
            problems.append(f'allowlist lists tester guides of other versions: {others}')
    return problems


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('render'); r.add_argument('src', type=pathlib.Path); r.add_argument('dst', type=pathlib.Path)
    t = sub.add_parser('render-tree'); t.add_argument('root', type=pathlib.Path)
    c = sub.add_parser('check'); c.add_argument('root', type=pathlib.Path); c.add_argument('--repo-root', type=pathlib.Path)
    c.add_argument('--allow-template', action='append', default=[], help='relative path of a template that may keep the baseline placeholders')
    for p in (r, t, c):
        p.add_argument('--version', required=True)
        p.add_argument('--pkg-name', required=True)
    a = ap.parse_args(argv)
    if a.cmd == 'render':
        a.dst.write_text(render_text(a.src.read_text(encoding='utf-8'), a.version, a.pkg_name, str(a.src)), encoding='utf-8')
        return 0
    if a.cmd == 'render-tree':
        print('rendered:', ', '.join(render_tree(a.root, a.version, a.pkg_name)))
        return 0
    problems = check_tree(a.root, a.version, a.pkg_name, tuple(a.allow_template))
    if a.repo_root:
        problems += check_tester_guide(a.repo_root, a.version)
    if problems:
        print('\n'.join('FAIL: ' + p for p in problems), file=sys.stderr)
        return 1
    n = sum(1 for _ in text_files(a.root))
    print(f'PASS: {n} documentation text files cite only {a.pkg_name} names')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
