"""Check the reference's own table of hyphen breaks against HSTeX's.

Eight breaks in cmr10, chosen so that what joins the letter before the break
to the letter after it differs: nothing, a kern, or a ligature.
\\hyphenpenalty=10000 keeps each discretionary whole so \\showbox prints its
replace count and both its lists.

    python3 -u tests/trip/check-break-table.py [-v]

Run it from a directory holding cmr10 (any will do -- kpsewhich finds it);
it writes its working files into the directory it is run from.

See docs/DECISIONS.md, where-a-rebuilt-run-begins.
"""
import argparse
import difflib
import os
import re
import subprocess
import sys

parser = argparse.ArgumentParser(
    description="Compare selected cmr10 hyphen-break tables with pdfTeX."
)
parser.add_argument("-v", "--verbose", action="store_true", help="show diffs")
arguments = parser.parse_args()

ENGINE = os.environ.get("HSTEX") or os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "build",
    "hstex",
)
CASES = [
 ('avatar',      'avatar',      'a1t'),
 ('abcd',        'abcd',        'b1c'),
 ('sentant',     'sentant',     'n1t'),
 ('xavx',        'xavx',        'a1v'),
 ('co-eff',      'coefficient', 'o1e'),
 ('different',   'different',   'f1f'),
 ('aflame',      'aflame',      'f1l'),
 ('coef-fi',     'coefficient', 'f1f'),
]
HEAD = (r'\catcode`\{=1 \catcode`\}=2 \catcode`\#=6' '\n'
        r'\showboxbreadth=9999 \showboxdepth=9999 \tracingonline=1' '\n'
        r'\font\f=cmr10 \f \hyphenchar\f=45 \hsize=400pt \parindent=0pt' '\n'
        r'\uchyph=0 \lefthyphenmin=1 \righthyphenmin=1 \spaceskip=4pt' '\n'
        r'\hyphenpenalty=10000 \exhyphenpenalty=10000' '\n'
        r'\hbadness=10000 \hfuzz=1000pt \pretolerance=-1 \tolerance=10000' '\n'
        + ''.join('\\lccode`\\%s=`\\%s ' % (c, c) for c in 'abcdefghilmnorstuvxyz') + '\n')

# INITEX writes an unprintable character in ^^ notation; the TeX Live format
# is built with a translate file that makes 9, 10, 11 and the upper half
# printable, and HSTeX follows the format. That is a property of the format
# rather than of either engine, so it is normalised away here.
FORMAT_RAW = {9: '^^I', 10: '^^J', 11: '^^K'}

def normalise(text):
    for code, caret in FORMAT_RAW.items():
        text = text.replace(chr(code), caret)
    return text

def box(path):
    t = open(path, errors='replace').read()
    m = re.search(r'> \\box0=\n(.*?)\n\n! OK\.', t, re.S)
    return normalise(m.group(1)) if m else None

bad = 0
for name, word, pat in CASES:
    body = HEAD + '\\patterns{%s}\n' % pat
    body += r'\setbox0=\vbox{\noindent xx %s\par}\showbox0' % word + '\n\\end\n'
    open('mx.tex','w').write(body)
    open('run-mx.tex','w').write('\\input mx \\end\n')
    for f in ('mx.log',):
        if os.path.exists(f): os.remove(f)
    subprocess.run(['pdftex','-ini','-interaction=nonstopmode','mx.tex'], capture_output=True)
    with open('mx-hstex.log','w') as out:
        subprocess.run([ENGINE, '--run-ini', 'mx.tex'],
                       stdout=out, stderr=subprocess.STDOUT)
    r, h = box('mx.log'), box('mx-hstex.log')
    if r is None or h is None:
        print("%-11s MISSING (ref=%s hstex=%s)" % (name, r is not None, h is not None)); bad += 1; continue
    if r == h:
        cnt = re.search(r'discretionary(?: replacing (\d+))?', r)
        print("%-11s ok      (%s)" % (name, cnt.group(0) if cnt else '?'))
    else:
        bad += 1
        print("%-11s DIFFERS" % name)
        if arguments.verbose:
            for l in list(difflib.unified_diff(r.split('\n'), h.split('\n'), 'ref','hstex', lineterm='', n=2))[:18]:
                print("     ", l)
print("differing:", bad, "of", len(CASES))
sys.exit(1 if bad else 0)
