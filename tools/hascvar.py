# Prints which console variable names exist as UTF-16 strings in the exe, plus the
# UTF-16 string that follows each (usually its help text).
import sys, re
d = open(sys.argv[1], 'rb').read()
for n in sys.argv[2:]:
    pat = b'\0\0' + (n + '\0').encode('utf-16le')
    i = d.find(pat)
    while i >= 0 and (i % 2) != 0: i = d.find(pat, i + 1)
    if i < 0: print('MISSING', n); continue
    print('ok     ', n)
