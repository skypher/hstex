#!/usr/bin/env python3
"""Emit TeX assignments turning the labels of aux OLD into those of NEW.

The output is the patch a woken chunk reads before it runs: the difference
between the auxiliary state the chunk was parked with and the state the run
it now serves will read. Only \newlabel and \bibcite lines are compared,
because those are the aux entries that become macro definitions; the .toc
and .bbl are read as files at their document positions and a parked chunk
picks the current ones up from disk by itself. See docs/DECISIONS.md,
the-relay.
"""
import re, sys
old_path, new_path = sys.argv[1], sys.argv[2]
def harvest(path):
    labels = {}
    for line in open(path, encoding="latin-1"):
        m = re.match(r'\\newlabel\{([^}]*)\}(\{.*\})\s*$', line)
        if m:
            labels[("r@", m.group(1))] = m.group(2)
            continue
        m = re.match(r'\\bibcite\{([^}]*)\}(\{.*\})\s*$', line)
        if m:
            labels[("b@", m.group(1))] = m.group(2)
    return labels
old, new = harvest(old_path), harvest(new_path)
emitted = 0
for key, body in sorted(new.items()):
    if old.get(key) != body:
        prefix, name = key
        # body is {...} with one outer group; the definition body is its inside
        inner = body[1:-1]
        sys.stdout.write("\\global\\expandafter\\def\\csname %s%s\\endcsname{%s}%%\n"
                         % (prefix, name, inner))
        emitted += 1
for key in old:
    if key not in new:
        prefix, name = key
        sys.stdout.write("\\global\\expandafter\\let\\csname %s%s\\endcsname\\relax %%\n"
                         % (prefix, name))
        emitted += 1
sys.stderr.write("patch: %d assignments\n" % emitted)
