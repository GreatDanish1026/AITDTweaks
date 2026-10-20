# Lists the console-variable vtables FConsoleManager's Register* functions install and
# classifies their first slots (UE 4.27 layout check).
import struct, sys
d = open(sys.argv[1], 'rb').read(); MGR = int(sys.argv[2], 16)
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i*40; name = d[o:o+8].rstrip(b'\0').decode(); vsz, sva, rsz, raw = struct.unpack_from('<IIII', d, o+8); secs.append((name, sva, max(vsz, rsz), raw))
B = 0x140000000
def off(v):
    r = v - B
    for n, sva, sz, raw in secs:
        if sva <= r < sva + sz: return raw + r - sva
def sec(v):
    r = v - B
    for n, sva, sz, raw in secs:
        if sva <= r < sva + sz: return n
def q(v): return struct.unpack_from('<Q', d, off(v))[0]
def classify(f):
    o = off(f)
    if o is None: return '??'
    b = d[o:o+12]
    if b[:3] == b'\xb0\x01\xc3': return 'TRUE'
    if b[:3] == b'\x32\xc0\xc3': return 'false'
    if b[:3] == b'\x33\xc0\xc3': return 'null'
    if b[:3] == b'\x48\x8d\x41' and b[4] == 0xc3: return '&this+%#x' % b[3]
    if b[:4] == b'\x48\x8b\xc1\xc3': return 'this'
    if b[:2] == b'\x8b\x41' and b[3] == 0xc3: return 'mov eax,[rcx+%#x]' % b[2]
    if b[:2] == b'\x89\x51' and b[3] == 0xc3: return 'mov [rcx+%#x],edx' % b[2]
    return b[:10].hex()
print('manager slots:')
for s in range(0, 0x100, 8): print('  +%#x %#x' % (s, q(MGR + s) - B))
seen = []
for s in range(0x08, 0xb0, 8):
    fn = q(MGR + s); o = off(fn); code = d[o:o+0x400]
    for k in range(len(code) - 7):
        if code[k] in (0x48, 0x4c) and code[k+1] == 0x8d and (code[k+2] & 0xc7) == 0x05:
            t = fn + k + 7 + struct.unpack_from('<i', code, k+3)[0]
            if sec(t) != '.rdata' or t in seen: continue
            first = q(t)
            if sec(first) != '.text' or sec(q(t+8)) != '.text' or sec(q(t+0x40)) != '.text': continue
            seen.append(t)
            print('mgr+%#x installs vtable rva %#x' % (s, t - B))
            print('   ' + ' | '.join('%#x:%s' % (i*8, classify(q(t + i*8))) for i in range(3, 20)))
