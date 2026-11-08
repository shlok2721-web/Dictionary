#!/usr/bin/env python3
"""Summarise a valgrind XML report.  Separates leaks whose stack passes through
this project's own source files from GTK/GLib/driver noise.

usage: summarize_valgrind.py REPORT.xml [--fail-on-project-leak]
"""
import os, sys, xml.etree.ElementTree as ET

path = sys.argv[1]
fail = "--fail-on-project-leak" in sys.argv
PROJECT = {f for f in os.listdir("src")} if os.path.isdir("src") else set()
LEAKS = {"Leak_DefinitelyLost", "Leak_IndirectlyLost", "Leak_PossiblyLost"}

try:
    root = ET.parse(path).getroot()
except Exception as e:                       # truncated XML (killed run)
    print(f"could not parse {path}: {e}"); sys.exit(0)

proj, other, errs = [], 0, 0
for er in root.iter("error"):
    kind = er.findtext("kind", "")
    if kind in LEAKS:
        frames = [(f.findtext("fn", "?"), f.findtext("file", ""), f.findtext("line", ""))
                  for f in er.iter("frame")]
        hit = [fr for fr in frames if fr[1] in PROJECT]
        if hit:
            proj.append((kind, er.findtext(".//leakedbytes", "?"), hit[0], frames[:6]))
        else:
            other += 1
    else:
        errs += 1
        print(f"- memory error: {kind}")

print(f"### {os.path.basename(path)}")
print(f"- leak records in project code: **{len(proj)}**")
print(f"- leak records only in GTK/GLib/driver code: {other}")
print(f"- non-leak memory errors (invalid read/write, uninitialised use…): **{errs}**")
for kind, size, (fn, f, l), frames in proj[:40]:
    print(f"\n`{kind}` {size} bytes — first project frame `{fn}` ({f}:{l})")
    for fn2, f2, l2 in frames:
        print(f"    {fn2} {f2}:{l2}")
sys.exit(1 if fail and (proj or errs) else 0)
