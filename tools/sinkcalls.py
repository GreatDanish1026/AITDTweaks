# Counts call [rax+X] sites shortly after `mov rcx/rax, [rip+Singleton]`, per X, to find which
# manager slots the engine calls (CallAllConsoleVariableSinks is called from a few places).
import struct, sys, collections
d = open(sys.argv[1], 'rb').read(); SING = int(sys.argv[2], 16)
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
for i in range(nsec):
    o = pe + 24 + optsz + i*40
    if d[o:o+8].rstrip(b'\0') == b'.text': vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o+8)
text = d[raw:raw+rsz]; cnt = collections.Counter(); sites = collections.defaultdict(list)
i = 0
while True:
    i = text.find(b'\x48\x8b', i)
    if i < 0: break
    if text[i+2] in (0x0d, 0x05) and va + i + 7 + struct.unpack_from('<i', text, i+3)[0] == SING:
        w = text[i+7:i+0x20]
        for k in range(len(w)-6):
            if w[k] == 0xff and w[k+1] in (0x50, 0x90):
                off = w[k+2] if w[k+1] == 0x50 else struct.unpack_from('<i', w, k+2)[0]
                cnt[off] += 1; sites[off].append(hex(va+i)); break
    i += 1
for off, n in sorted(cnt.items()): print(hex(off), n, sites[off][:6] if n < 12 else '')
