# Finds the FConsoleManager vtable: the code that stores the new manager into
# IConsoleManager::Singleton, and the .rdata address it lea's just before.
import struct, sys
d = open(sys.argv[1], 'rb').read(); SING = int(sys.argv[2], 16)
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
secs = {}
for i in range(nsec):
    o = pe + 24 + optsz + i*40; name = d[o:o+8].rstrip(b'\0').decode(); vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o+8); secs[name] = (va, vsz, raw, rsz)
tva, tvsz, traw, trsz = secs['.text']; text = d[traw:traw+trsz]; rva0, rvsz = secs['.rdata'][:2]
i = 0
while True:
    i = text.find(b'\x48\x89', i)
    if i < 0: break
    if (text[i+2] & 0xc7) == 0x05 and tva + i + 7 + struct.unpack_from('<i', text, i+3)[0] == SING:
        print('store to singleton at rva', hex(tva + i))
        for q in range(i - 3, i - 0x80, -1):
            if text[q] in (0x48, 0x4c) and text[q+1] == 0x8d and (text[q+2] & 0xc7) == 0x05:
                t = tva + q + 7 + struct.unpack_from('<i', text, q+3)[0]
                if rva0 <= t < rva0 + rvsz: print('   lea .rdata', hex(t), 'at', hex(tva + q))
    i += 1
