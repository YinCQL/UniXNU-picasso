"""Malformed firmware inputs and bounded FAT16 chain tests; no vendor data needed."""
import struct
from picasso_adsp_firmware import BASE, SIZE, Fat16, join


def reject(call):
    try:
        call()
    except (ValueError, UnicodeError):
        return
    raise AssertionError('Malformed firmware accepted')


def fixture():
    h = bytearray(52 + 4 * 32)
    h[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHI', h, 16, 2, 164, 1)
    struct.pack_into('<I', h, 28, 52)
    struct.pack_into('<HHH', h, 40, 52, 32, 4)
    for i, values in enumerate(((0,0,0,0,len(h),len(h),0,0),
        (0,0x1000,0,0,32,32,0x02000000,0),
        (1,0x2000,0,BASE,16,0x1000,0x08000000,0x1000),
        (1,0x3000,0,BASE+0x1000,0,SIZE-0x1000,0x08000000,0x1000))):
        struct.pack_into('<8I', h, 52 + i * 32, *values)
    return {'adsp.mdt': bytes(h) + b'H' * 32, 'adsp.b00': bytes(h), 'adsp.b01': b'H' * 32, 'adsp.b02': b'P' * 16}


def changed(at, fmt, value):
    files = fixture()
    h = bytearray(files['adsp.b00'])
    struct.pack_into(fmt, h, at, value)
    files['adsp.b00'] = bytes(h)
    files['adsp.mdt'] = bytes(h) + files['adsp.b01']
    return files


def test():
    image, report = join(fixture())
    assert image[:7] == b'\x7fELF\x01\x01\x01' and image[0x2000:] == b'P'*16
    assert report['reserved_bytes'] == SIZE and not report['authenticated']
    for at, fmt, value in ((18,'<H',40),(28,'<I',0xffffffff),(44,'<H',65),
        (52+64+12,'<I',BASE-1),(52+64+20,'<I',15),(52+64+20,'<I',0),
        (52+96+12,'<I',BASE),(52+96+20,'<I',SIZE),
        (52+64+4,'<I',0x1000),(52+64+4,'<I',0xffffffff),
        (52+64+24,'<I',0),(52+64+28,'<I',3),(52+32+24,'<I',0)):
        reject(lambda: join(changed(at,fmt,value)))
    for name in fixture():
        files=fixture();del files[name];reject(lambda: join(files))
        files=fixture();files[name]=files[name][:-1];reject(lambda: join(files))
    files=fixture();files['adsp.b03']=b'X';reject(lambda: join(files))
    files=fixture();files['adsp.mdt']=b'X'+files['adsp.mdt'][1:];reject(lambda: join(files))
    for length in range(52):
        files=fixture();files['adsp.mdt']=files['adsp.mdt'][:length];reject(lambda: join(files))
    # Tail-trimmed synthetic FAT16; all used clusters are present. Loops and
    # short/missing allocated data must fail rather than being zero-filled.
    raw=bytearray(48*512);raw[510:512]=b'\x55\xaa'
    struct.pack_into('<HBHBHH',raw,11,512,1,1,2,16,5000)
    struct.pack_into('<H',raw,22,20)
    for start in (512,21*512):
        struct.pack_into('<H',raw,start+4,0xffff)
    raw[42*512:42*512+4]=b'DATA'
    fat=Fat16(bytes(raw));assert fat.chain(2,4)==b'DATA' and fat.logical_bytes>len(raw)
    fat.fat=bytearray(fat.fat);struct.pack_into('<H',fat.fat,4,2)
    reject(lambda: fat.chain(2,1024));reject(lambda: fat.chain(2))
    struct.pack_into('<H',fat.fat,4,0xffff);reject(lambda: fat.chain(2,1024))
    struct.pack_into('<H',fat.fat,4,10);struct.pack_into('<H',fat.fat,20,0xffff)
    reject(lambda: fat.chain(2,1024))
    raw[21*512+4]^=1;reject(lambda: Fat16(bytes(raw)))
    # A checked long name must bind to its short entry, not to the next file.
    entry=bytearray(32);entry[:11]=b'ADSP~1  MDT';entry[11]=32
    struct.pack_into('<H',entry,26,2);struct.pack_into('<I',entry,28,4)
    checksum=0
    for c in entry[:11]:checksum=(((checksum&1)<<7)+(checksum>>1)+c)&255
    long=bytearray(32);long[0]=0x41;long[11]=15;long[13]=checksum
    units=[ord(c) for c in 'adsp.mdt']+[0]+[0xffff]*4
    for at,value in zip((1,3,5,7,9,14,16,18,20,22,24,28,30),units):struct.pack_into('<H',long,at,value)
    directory=bytes(long+entry+bytearray(32))
    assert Fat16.entries(directory)=={'adsp.mdt':(2,4,False)}
    for at in (0,12,13,26):
        bad=bytearray(directory);bad[at]^=1;reject(lambda: Fat16.entries(bad))
    reject(lambda: Fat16.entries(bytes(entry+entry)))
    print('PASS ADSP firmware: ELF machine/extent/relocation/overlap/pieces, FAT mirrors/cycles/truncation')


if __name__ == '__main__':
    test()
