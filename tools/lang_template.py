#!/usr/bin/env python3
"""Write lang/template.json: every English string the menu can show, for translating ootgz.

    python3 tools/lang_template.py

Copy the template to sd:/ootgz/lang/<code>.json (es, fr, de, it, pt, nl, pl, sv, da, no, fi, cs, hu,
tr, ro, ca, id, or custom), set "language" to the language's own name and fill in the translations.
Strings left "" stay English. Keep every %s / %d / %.2f of a string, in the same order: a translation
whose conversions don't match is ignored (lang.c TF). The font has ASCII only: accented letters are
shown without the accent; other scripts can't be shown.

The strings are the C string literals in src/ootgz that read as text (lower case words, spaces,
punctuation), not identifiers or the game's names (Weapon_Sword_Kokiri, Dm_SY_BancChangeGo ...).
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "ootgz"
OUT = ROOT / "lang" / "template.json"

LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
SKIP_FILES = {"font.c", "lang.c"}
SPEC = re.compile(r"%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|z)?[a-zA-Z%]")


def is_text(s):
    if not s or not re.search(r"[a-z]{2}", SPEC.sub("", s)):  # words, not just %s / %d
        return False
    if "_" in s or "/" in s and " " not in s or "\\x" in s or s.startswith("%") and " " not in s:
        return False
    if re.fullmatch(r"[A-Za-z0-9]+", s) and re.search(r"[A-Z]", s) and re.search(r"[a-z]", s):
        return False  # CamelCase: an identifier
    if re.search(r"[A-Z][a-z]+[A-Z]", s):
        return False
    if ":/" in s or s.endswith((".json", ".ini", ".c", ".h")):
        return False
    return bool(re.fullmatch(r"[ -~]+", s))


def main():
    found = {}
    for path in sorted(SRC.glob("*.c")):
        if path.name in SKIP_FILES:
            continue
        code = path.read_text(encoding="utf-8", errors="replace")
        code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)  # comments
        for line in code.splitlines():
            if line.lstrip().startswith(("#include", "log_printf", "__asm__")) or "__asm__(" in line \
                    or "log_printf(" in line:
                continue
            for m in LITERAL.finditer(line):
                s = bytes(m.group(1), "utf-8").decode("unicode_escape")
                if is_text(s):
                    found.setdefault(s, path.name)
    template = {"language": "the language's own name", "strings": {s: "" for s in sorted(found)}}
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps(template, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{OUT.relative_to(ROOT)}: {len(found)} strings", file=sys.stderr)


if __name__ == "__main__":
    main()
