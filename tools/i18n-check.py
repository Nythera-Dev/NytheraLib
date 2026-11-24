#!/usr/bin/env python3
"""
Static i18n checks for NytheraLib.

  1) Missing keys (always on): every i18n::tr / i18n::trp string literal key in
     src/ must exist in assets/translations/en.json. Exit 1 when any are missing.

  2) --unused: catalog entries not referenced anywhere in src/.

  3) --check-locales: verify all locale JSON files in assets/translations/ are
     valid JSON objects and contain no keys absent from en.json.

  4) --raw: flag likely user-visible string literals in src/ui/ that are not
     wrapped in i18n::tr.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TRANSLATIONS = ROOT / "assets" / "translations" / "en.json"
DEFAULT_SRC = ROOT / "src"
DEFAULT_RAW_ROOTS = ("src/ui",)

TR_RE = re.compile(r'i18n::tr\s*\(\s*"([^"]+)"')
TRP_RE = re.compile(r'i18n::trp\s*\(\s*"([^"]+)"')
KEY_LITERAL_RE = re.compile(r'"([a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+(?:-plural)?)"')

_UI_METHODS = (
    "setText",
    "setPlaceholder",
    "setTitle",
    "setSubtitle",
    "setButtonText",
    "setEmptyText",
    "setDescription",
    "setHelp",
    "setMessage",
    "setTooltip",
    "setHint",
    "setLabel",
    "setHeader",
    "setFooter",
    "setCaption",
    "setSummary",
    "setDetail",
)
RAW_UI_LITERAL_RE = re.compile(
    rf"(?:->|\.)\s*(?P<meth>{'|'.join(_UI_METHODS)})\s*\(\s*\"(?P<lit>[^\"\\]*)\"",
)
HAS_I18N = re.compile(r"i18n::tr(?:p)?\s*\(")
IGNORE_LINE_MARK = re.compile(r"//\s*check-i18n:ignore-line\b|//\s*i18n:ignore-line\b")
SKIP_LINE_HINT = re.compile(
    r"(assert\s*\(|static_assert\s*\(|\bkLog\w*\.|\buiAssert|\bLogger\b|\bstd::cerr\b|\bspdlog::|\bNYTHERA_DEBUG\b|\bthrow\b)",
)
STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
_IGNORE_LITERAL_LOWER = frozenset(
    {"true", "false", "null", "none", "auto", "default", "inherit", "transparent", "check", "menu", "close"}
)


def flatten_json_strings(node: object, prefix: str, out: dict[str, str]) -> None:
    if isinstance(node, dict):
        for k, v in node.items():
            p = f"{prefix}.{k}" if prefix else str(k)
            flatten_json_strings(v, p, out)
    elif isinstance(node, str):
        out[prefix] = node
    elif isinstance(node, (int, float, bool)):
        return
    elif isinstance(node, list):
        for i, item in enumerate(node):
            flatten_json_strings(item, f"{prefix}[{i}]", out)


def iter_source_files(src: Path, extensions: tuple[str, ...]) -> Iterable[Path]:
    for ext in extensions:
        yield from src.rglob(f"*{ext}")


def collect_tr_keys(text: str) -> tuple[set[str], set[str]]:
    return set(TR_RE.findall(text)), set(TRP_RE.findall(text))


def check_catalog(keys: Iterable[str], catalog: dict[str, str]) -> list[str]:
    return [k for k in sorted(keys) if not k.endswith(".") and k not in catalog]


def _ascii_letter_count(s: str) -> int:
    return sum(1 for c in s if ("A" <= c <= "Z" or "a" <= c <= "z"))


def is_placeholder_or_internal_literal(s: str) -> bool:
    t = s.strip()
    if not t:
        return True
    low = t.lower()
    if low in _IGNORE_LITERAL_LOWER or low.startswith("nythera:"):
        return True
    if re.fullmatch(r"[\d.\s%+-]+", t):
        return True
    if _ascii_letter_count(t) == 0:
        return True
    if re.fullmatch(r"[A-Za-z]{1,3}", t):
        return True
    return False


def looks_like_user_visible_copy(s: str) -> bool:
    if is_placeholder_or_internal_literal(s):
        return False
    letters = _ascii_letter_count(s)
    if letters < 4:
        return False
    if " " in s:
        return True
    if letters >= 7 or (letters >= 5 and len(s.strip()) >= 5):
        return True
    return False


def cpp_code_before_line_comment(line: str) -> str:
    out: list[str] = []
    i = 0
    n = len(line)
    in_str = False
    esc = False
    while i < n:
        c = line[i]
        if not in_str:
            if c == '"':
                in_str = True
                out.append(c)
            elif c == "/" and i + 1 < n and line[i + 1] == "/":
                break
            else:
                out.append(c)
        else:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        i += 1
    return "".join(out)


def is_wide_noise_literal(s: str) -> bool:
    t = s.strip()
    low = t.lower()
    if low.startswith(("http://", "https://")) or re.fullmatch(r"#[0-9a-fA-F]{3,8}", t):
        return True
    if "/" in t and " " not in t and re.fullmatch(r"[\w./+\-]+", t):
        return True
    if t.count(".") >= 2 and " " not in t and re.fullmatch(r"[a-zA-Z0-9_.-]+", t):
        return True
    if ("{" in t and "}" in t) or "::" in t:
        return True
    if low.startswith(("error:", "warning:")) or re.fullmatch(r"ok\n?", low):
        return True
    if r"\x" in t or r"\u" in t:
        return True
    return False


def looks_like_wide_prose_string(s: str) -> bool:
    if is_placeholder_or_internal_literal(s) or is_wide_noise_literal(s):
        return False
    letters = _ascii_letter_count(s)
    if letters < 5:
        return False
    t = s.strip()
    return (" " in t and len(t) >= 10) or (len(t) >= 28 and letters >= 15)


def scan_raw_ui_file(text: str, *, wide_strings: bool) -> list[tuple[int, str, str]]:
    hits: list[tuple[int, str, str]] = []
    seen: set[tuple[int, str]] = set()
    for i, line in enumerate(text.splitlines(), start=1):
        if IGNORE_LINE_MARK.search(line) or HAS_I18N.search(line) or SKIP_LINE_HINT.search(line):
            continue
        if re.match(r"^\s*#", line):
            continue
        code = cpp_code_before_line_comment(line)
        setter_literals: set[str] = set()
        for m in RAW_UI_LITERAL_RE.finditer(code):
            lit, meth = m.group("lit"), m.group("meth")
            if not looks_like_user_visible_copy(lit):
                continue
            if (i, lit) not in seen:
                seen.add((i, lit))
                setter_literals.add(lit)
                hits.append((i, meth, lit))
        if wide_strings:
            for m in STRING_LITERAL_RE.finditer(code):
                lit = m.group(1)
                if lit in setter_literals or not looks_like_wide_prose_string(lit):
                    continue
                if (i, lit) not in seen:
                    seen.add((i, lit))
                    hits.append((i, "literal", lit))
    return hits


def collect_key_literals(text: str) -> tuple[set[str], set[str]]:
    exact = {m.group(1) for m in KEY_LITERAL_RE.finditer(text)}
    prefixes = {m.group(1) for m in re.finditer(r'"([a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+\.)"', text)}
    return exact, prefixes


def find_unused_keys(
    catalog: dict[str, str],
    directly_used: set[str],
    source_literals: set[str],
    source_prefixes: set[str],
) -> tuple[list[str], list[str]]:
    unused: list[str] = []
    possibly_dynamic: list[str] = []
    for key in sorted(catalog):
        if key in directly_used or key in source_literals:
            continue
        if key.endswith("-plural") and key[:-7] in source_literals:
            continue
        if any(key.startswith(p) for p in source_prefixes):
            possibly_dynamic.append(key)
        else:
            unused.append(key)
    return unused, possibly_dynamic


def check_locales(translations_dir: Path, en_catalog: dict[str, str]) -> list[str]:
    issues: list[str] = []
    for path in sorted(translations_dir.glob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception as e:
            issues.append(f"{path.name}: failed to parse JSON: {e}")
            continue
        if not isinstance(data, dict):
            issues.append(f"{path.name}: root is not a JSON object")
            continue
        loc_cat: dict[str, str] = {}
        flatten_json_strings(data, "", loc_cat)
        unknown = sorted(set(loc_cat) - set(en_catalog))
        if unknown:
            issues.append(f"{path.name}: {len(unknown)} unknown key(s) absent from en.json (first: {unknown[0]})")
    return issues


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--translations", type=Path, default=DEFAULT_TRANSLATIONS)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--raw-roots", nargs="*", default=list(DEFAULT_RAW_ROOTS))
    ap.add_argument("--unused", action="store_true")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--setters-only", action="store_true")
    ap.add_argument("--check-locales", action="store_true")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--fail-on-unused", action="store_true")
    ap.add_argument("--fail-on-raw", action="store_true")
    args = ap.parse_args()

    if args.all:
        args.unused = True
        args.raw = True
        args.check_locales = True
    if args.fail_on_unused:
        args.unused = True
    if args.fail_on_raw:
        args.raw = True

    if not args.translations.is_file():
        print(f"error: translations file not found: {args.translations}", file=sys.stderr)
        return 2

    catalog: dict[str, str] = {}
    with args.translations.open(encoding="utf-8") as f:
        flatten_json_strings(json.load(f), "", catalog)

    all_tr: set[str] = set()
    all_trp: set[str] = set()
    all_key_literals: set[str] = set()
    all_key_prefixes: set[str] = set()
    raw_hits: list[tuple[Path, int, str, str]] = []

    for path in iter_source_files(args.src, (".cpp", ".h")):
        text = path.read_text(encoding="utf-8")
        tr_keys, trp_bases = collect_tr_keys(text)
        all_tr |= tr_keys
        all_trp |= trp_bases
        if args.unused:
            lits, prefixes = collect_key_literals(text)
            all_key_literals |= lits
            all_key_prefixes |= prefixes

    if args.raw:
        for rel in args.raw_roots:
            base = (ROOT / rel).resolve()
            if not base.is_dir():
                continue
            for path in iter_source_files(base, (".cpp", ".h")):
                for line_no, where, lit in scan_raw_ui_file(
                    path.read_text(encoding="utf-8"), wide_strings=not args.setters_only
                ):
                    raw_hits.append((path, line_no, where, lit))
        raw_hits.sort(key=lambda h: (str(h[0]), h[1], h[2], h[3]))

    trp_required = set()
    for base in all_trp:
        trp_required.add(base)
        trp_required.add(f"{base}-plural")
    missing = sorted(set(check_catalog(all_tr, catalog) + check_catalog(trp_required, catalog)))
    exit_code = 0

    if missing:
        exit_code = 1
        print(f"Missing {len(missing)} translation key(s) in {args.translations}:")
        for k in missing:
            print(f"  - {k}")

    if args.check_locales:
        locale_issues = check_locales(args.translations.parent, catalog)
        if locale_issues:
            exit_code = 1
            print("Locale catalog issue(s):")
            for issue in locale_issues:
                print(f"  - {issue}")

    if args.unused:
        directly_used = all_tr | all_trp | {f"{b}-plural" for b in all_trp}
        unused, possibly_dynamic = find_unused_keys(catalog, directly_used, all_key_literals, all_key_prefixes)
        if possibly_dynamic:
            print(f"\nPossibly dynamic ({len(possibly_dynamic)} key(s)):")
            for k in possibly_dynamic:
                print(f"  ? {k}")
        if unused:
            if args.fail_on_unused:
                exit_code = 1
            print(f"\nUnused catalog key(s): {len(unused)}")
            for k in unused:
                print(f"  - {k}")
        else:
            print(f"\nUnused keys: none (catalog={len(catalog)}, possibly-dynamic={len(possibly_dynamic)}).")

    if args.raw and raw_hits:
        print(f"\nRaw UI string literals ({len(raw_hits)} hit(s)):")
        for path, line_no, where, lit in raw_hits[:400]:
            rel = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
            print(f"  {rel}:{line_no}: {where}: {lit!r}")
        if args.fail_on_raw:
            exit_code = 1

    if exit_code == 0:
        print(
            f"OK: {len(all_tr)} tr keys, {len(all_trp)} trp base keys"
            f", all present in catalog ({len(catalog)} entries)."
        )
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
