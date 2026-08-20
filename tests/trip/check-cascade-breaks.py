"""Check a discretionary whole, in a font where every letter ligatures.

The trip font puts a character between every pair of digits, so one short word
becomes forty-odd nodes.  \\hyphenpenalty=10000 forbids the break as a
breakpoint without stopping hyphenation from inserting it, so \\showbox prints
the discretionary's replace count and both its lists instead of the emptied
one a taken break leaves behind.  A probe that takes its break cannot test
what the break replaces.

    python3 tests/trip/check-cascade-breaks.py

Run it from a directory holding trip.tfm; tests/trip/run-trip.sh builds one
in build/trip.

See docs/DECISIONS.md, a-break-inside-a-cascade-of-ligatures.
"""
import subprocess, re, os, sys

HEAD = (r'\catcode`\{=1 \catcode`\}=2 \catcode`\#=6' '\n'
        r'\font\rip=trip \rip' '\n'
        r'\lccode`A=`1 \lccode`B=`5 \lccode`C=`2' '\n'
        r'\lccode`1=`1 \lccode`5=`5 \lccode`2=`2 \lccode`7=`7' '\n'
        r'\language=0' '\n')
TAIL = (r'\lefthyphenmin=1 \righthyphenmin=1' '\n'
        r'\hyphenchar\rip=`-' '\n'
        r'\showboxbreadth=9999 \showboxdepth=9999 \tracingonline=1' '\n'
        r'\hbadness=10000 \hfuzz=1000pt' '\n'
        r'\hyphenpenalty=10000 \exhyphenpenalty=10000' '\n')
CASES = [('A1BAC', '1512', 'break after 1'),
         ('AB1AC', '1512', 'break after 15'),
         ('ABA1C', '1512', 'break after 151')]

def discretionary(path):
    text = open(path, errors='replace').read()
    found = re.search(r'> \\box0=\n(.*?)\n\n! OK\.', text, re.S)
    if not found:
        return None
    lines = found.group(1).split('\n')
    for index, line in enumerate(lines):
        if 'discretionary' in line:
            out = [line.strip()]
            for more in lines[index + 1:index + 16]:
                if more.startswith('...') or more.startswith('..|'):
                    out.append(more.strip())
                else:
                    break
            return out
    return ['(no discretionary)']

bad = 0
for patterns, word, what in CASES:
    body = HEAD + '\\patterns{%s}\n' % patterns + TAIL
    body += (r'\setbox0=\vbox{\hsize=400pt \parindent=0pt \parfillskip=0pt' '\n'
             r'\rightskip=0pt plus 1fil \pretolerance=-1 \tolerance=10000' '\n'
             '7 %s\\par}' % word + '\n' + r'\showbox0' '\n' + r'\end' '\n')
    open('cb.tex', 'w').write(body)
    if os.path.exists('cb.log'):
        os.remove('cb.log')
    subprocess.run(['pdftex', '-ini', '-interaction=nonstopmode', 'cb.tex'],
                   capture_output=True)
    with open('cb-hstex.log', 'w') as out:
        subprocess.run(['./build/hstex', '--run-ini', 'cb.tex'], stdout=out,
                       stderr=subprocess.STDOUT)
    reference, hstex = discretionary('cb.log'), discretionary('cb-hstex.log')
    if reference == hstex:
        print("%-16s ok      %s" % (what, reference[0]))
    else:
        bad += 1
        print("%-16s DIFFERS" % what)
        print("     ref  :", reference)
        print("     hstex:", hstex)
print("differing:", bad, "of", len(CASES))
sys.exit(1 if bad else 0)
