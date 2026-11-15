#!/usr/bin/env python3
"""Offline type-0 (THIRD_PARTY_DONGLE) adaptation of the audited 20H392 bluetoothd.

Source: Renoir Bluetooth, pinned in NOTICE.md. Code goes into the unused,
r-x mapped Mach-O header padding of __TEXT (file 0x4000.. = VA 0x100004000..);
the type-0 capability logic lives in /usr/lib/PicassoBTType0.dylib (added
LC_LOAD_DYLIB, appended so existing bind ordinals are unchanged), reached
through an RW slot at the zero-fill tail of __DATA. Signing is done afterwards.
"""
import hashlib,struct,sys

ORIGINAL='8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f'
BASE=0x100000000
CAVE=0x100004000
DYLIB=b'/usr/lib/PicassoBTType0.dylib'

def u32(x):return struct.pack('<I',x&0xffffffff)
def rel(pc,target,bits,shift=2):
    off=(target-pc)>>shift
    if not -(1<<(bits-1))<=off<(1<<(bits-1)):raise ValueError(f'branch out of range {pc:#x}->{target:#x}')
    return off&((1<<bits)-1)
def b(pc,t):return 0x14000000|rel(pc,t,26)
def bl(pc,t):return 0x94000000|rel(pc,t,26)
def cbz_w(pc,rt,t):return 0x34000000|rel(pc,t,19)<<5|rt
def cbnz_w(pc,rt,t):return 0x35000000|rel(pc,t,19)<<5|rt
def cbz_x(pc,rt,t):return 0xb4000000|rel(pc,t,19)<<5|rt
def b_eq(pc,t):return 0x54000000|rel(pc,t,19)<<5|0
def tbz(pc,rt,bit,t):return 0x36000000|(bit>>5)<<31|(bit&31)<<19|rel(pc,t,14)<<5|rt
def adrp(pc,rd,t):
    imm=((t&~0xfff)-(pc&~0xfff))>>12
    if not -(1<<20)<=imm<(1<<20):raise ValueError('adrp range')
    imm&=(1<<21)-1
    return 0x90000000|(imm&3)<<29|(imm>>2)<<5|rd
def add_imm(rd,rn,imm):assert 0<=imm<4096;return 0x91000000|imm<<10|rn<<5|rd
def ldr_x(rt,rn,off):assert off%8==0 and off<0x8000;return 0xf9400000|(off//8)<<10|rn<<5|rt
def str_x(rt,rn,off):assert off%8==0 and off<0x8000;return 0xf9000000|(off//8)<<10|rn<<5|rt
def ldrb_w(rt,rn,off):assert off<4096;return 0x39400000|off<<10|rn<<5|rt
def ldr_w(rt,rn,off):assert off%4==0;return 0xb9400000|(off//4)<<10|rn<<5|rt
def movz_w(rd,imm):assert imm<0x10000;return 0x52800000|imm<<5|rd
def movz_x(rd,imm):assert imm<0x10000;return 0xd2800000|imm<<5|rd
def cmp_w_imm(rn,imm):assert imm<4096;return 0x7100001f|imm<<10|rn<<5
NOP=0xd503201f;RET=0xd65f03c0;BLR_X16=0xd63f0200
STP_FP=0xa9bf7bfd;MOV_FP=0x910003fd;LDP_FP=0xa8c17bfd
XZR=31

class Image:
    def __init__(self,data):self.d=bytearray(data)
    def word(self,va):return struct.unpack_from('<I',self.d,va-BASE)[0]
    def expect(self,va,words):
        for i,w in enumerate(words):
            got=self.word(va+4*i)
            if got!=w:raise SystemExit(f'patch site {va+4*i:#x}: {got:#010x}, expected {w:#010x}')
    def put(self,va,words):
        for i,w in enumerate(words):struct.pack_into('<I',self.d,va-BASE+4*i,w)

def assemble(image,va,program):
    """program: list of callables(pc)->word or ints; labels resolved by caller."""
    words=[p(va+4*i) if callable(p) else p for i,p in enumerate(program)]
    image.put(va,words);return va+4*len(words)

def patch(data):
    if hashlib.sha256(data).hexdigest()!=ORIGINAL:raise SystemExit('unexpected bluetoothd: expected audited 20H392 original')
    im=Image(data)
    if any(im.d[0x2a60:0x693c]):raise SystemExit('header padding not empty')
    cave=CAVE

    # Platform factory: the three BCM_4357 constructions -> THIRD_PARTY_DONGLE (0).
    for va in (0x10029b36c,0x10029c104,0x10029c148):
        im.expect(va,[movz_w(1,0xf)]);im.put(va,[movz_w(1,0)])

    # 1. Registration 0x100166c74 accepts 0..0xf9f (was 1..0xf9f) and stores it.
    im.expect(0x100166c74,[0x51000409,0x713e793f]);im.put(0x100166c74,[cmp_w_imm(0,0xf9f),NOP])

    # 2. Vendor initialisation 0x100166a04, chip 0: an explicit vendor-less backend.
    #    Empty operation table (0x100850030, 0x750 bytes), no vendor event handler
    #    (0x100850780) and no command-complete hooks (0x10084f930/950). Other
    #    out-of-range values keep the original "Invalid chipset" path.
    im.expect(0x100166a90,[movz_w(0,2)])
    entry_a=cave;orig_a=cave+11*4
    cave=assemble(im,cave,[
        lambda pc:cbnz_w(pc,19,orig_a),
        lambda pc:adrp(pc,0,0x100850000),add_imm(0,0,0x30),movz_x(1,0x750),
        lambda pc:bl(pc,0x10065ff7c),                        # _bzero stub
        lambda pc:adrp(pc,8,0x100850000),str_x(XZR,8,0x780),
        lambda pc:adrp(pc,8,0x10084f000),str_x(XZR,8,0x930),str_x(XZR,8,0x950),
        lambda pc:b(pc,0x100166ac8),                         # epilogue
        movz_w(0,2),lambda pc:b(pc,0x100166a94)])            # other values: original path
    type0_a=entry_a
    im.put(0x100166a90,[b(0x100166a90,entry_a)])

    # 3. Vendor callbacks 0x100166e48, chip 0: the generic callback is already saved
    #    at 0x1008507b8; no BCM/ACI callback table. Other values unchanged.
    im.expect(0x100166eb0,[movz_w(0,2)])
    cb=cave;orig_b=cave+3*4
    cave=assemble(im,cave,[ldr_w(8,19,0xf20),lambda pc:cbnz_w(pc,8,orig_b),lambda pc:b(pc,0x100166eec),
        movz_w(0,2),lambda pc:b(pc,0x100166eb4)])
    im.put(0x100166eb0,[b(0x100166eb0,cb)])

    # 4. Init step 0x10010bb44 (0x0C5B Write Default Erroneous Data Reporting) is
    #    selected only when its support bit (byte 18, mask 0x08 of 0x100863938) is
    #    set; otherwise the chain goes to its next step (0x1001c2220), the same
    #    call the native optional steps use. No completion is generated.
    im.expect(0x10010bb44,[0xd100c3ff])
    step=cave;skip=cave+5*4
    cave=assemble(im,cave,[
        lambda pc:adrp(pc,16,0x100863000),ldrb_w(16,16,0x94a),lambda pc:tbz(pc,16,3,skip),
        0xd100c3ff,lambda pc:b(pc,0x10010bb48),
        lambda pc:adrp(pc,8,0x10084f000),ldr_x(0,8,0x598),lambda pc:b(pc,0x1001c2220)])
    im.put(0x10010bb44,[b(0x10010bb44,step)])

    # 5. Capability publication at the end of initCmdReadLeLocalSupportedFeaturesCB
    #    (0x10010c114): the type-0 library decides when the chain advances.
    im.expect(0x10010c114,[0xb0002f01,0x912f0021,movz_w(0,0),bl(0x10010c120,0x1001094ac)])
    pub=cave;fallback=cave+8*4
    cave=assemble(im,cave,[
        STP_FP,MOV_FP,lambda pc:adrp(pc,16,0x100867000),ldr_x(16,16,0xff0),lambda pc:cbz_x(pc,16,fallback),
        BLR_X16,LDP_FP,RET,
        LDP_FP,lambda pc:adrp(pc,1,0x1006ed000),add_imm(1,1,0xbc0),movz_w(0,0),lambda pc:b(pc,0x1001094ac)])
    im.put(0x10010c114,[bl(0x10010c114,pub),b(0x10010c118,0x10010c124)])
    # 6. Transport pre-stop 0x1000e6aac: the selected HCI transport's +0x60 extension.
    #    H4 (type 1, table 0x100844228) has none (+0x60 = 0; H4BC's 0x10006a444 acts
    #    on H4BC state only), so H4 returns to the stack termination, which then
    #    runs the real transport close (0x1000f3b5c -> ... -> 0x1000674d0).
    if im.d[0x0e6aac+3]&0x9f!=0x90:raise SystemExit('pre-stop site is not adrp')
    if im.word(0x1000e6ab0)!=ldr_x(8,8,0x850) or im.word(0x1000e6abc)!=ldr_x(0,8,0x60):raise SystemExit('pre-stop site differs')
    pre=cave;ret_e=cave+7*4
    cave=assemble(im,cave,[lambda pc:adrp(pc,8,0x100844000),ldr_x(8,8,0x850),ldr_w(9,8,0),cmp_w_imm(9,1),
        lambda pc:b_eq(pc,ret_e),lambda pc:cbz_w(pc,9,ret_e),lambda pc:b(pc,0x1000e6abc),RET])
    im.put(0x1000e6aac,[b(0x1000e6aac,pre)])

    # 7. LeBroadcaster stack-start: refresh the mode cached before HCI probing.
    #    Keep the original this pointer and return address, invoke the type-0
    #    library, then execute the displaced prologue and the original method.
    #    The new slot is in the same unused RW tail as SLOT_PUBLISH; no section
    #    occupies 0x100864478..0x100868000 in this exact original image.
    im.expect(0x10050a18c,[0xd10183ff])              # sub sp, sp, #0x60
    adv_start=cave;adv_resume=cave+5*4
    cave=assemble(im,cave,[
        0xa9bf7be0,                              # stp x0,x30,[sp,#-16]!
        lambda pc:adrp(pc,16,0x100867000),ldr_x(16,16,0xff8),
        lambda pc:cbz_x(pc,16,adv_resume),BLR_X16,
        0xa8c17be0,                              # ldp x0,x30,[sp],#16
        0xd10183ff,lambda pc:b(pc,0x10050a190)])
    im.put(0x10050a18c,[b(0x10050a18c,adv_start)])
    # Observe the real BTKC root-key read/write status; no key material is copied.
    # Tail-call the library wrapper, or run the unchanged original prologue.
    # Fixed continuation locations are checked by the library's native tests.
    for site,slot,continuation,prologue in [
        (0x100257654,0x100867fc0,0x100006000,0xd10183ff),
        (0x100257820,0x100867fc8,0x100006010,0xa9bc5ff8),
    ]:
        im.expect(site,[prologue])
        im.expect(continuation,[0,0])
        im.put(continuation,[prologue,b(continuation+4,site+4)])
        entry=cave;fallback=cave+4*4
        cave=assemble(im,cave,[lambda pc,slot=slot:adrp(pc,16,slot),ldr_x(16,16,slot&0xfff),
            lambda pc,fallback=fallback:cbz_x(pc,16,fallback),0xd61f0200,
            prologue,lambda pc,site=site:b(pc,site+4)])
        im.put(site,[b(site,entry)])
    if cave>BASE+0x6000:raise SystemExit('cave overflow')

    # 8. LC_LOAD_DYLIB /usr/lib/PicassoBTType0.dylib, appended after the last command.
    magic,cpu,sub,ft,ncmds,sizeofcmds,flags,res=struct.unpack_from('<IiiIIIII',im.d,0)
    end=32+sizeofcmds
    size=(24+len(DYLIB)+1+7)&~7
    if end+size>0x4000:raise SystemExit('no load-command space')
    cmd=struct.pack('<IIIIII',0xc,size,24,2,0x10000,0x10000)+DYLIB
    im.d[end:end+size]=cmd+b'\0'*(size-len(cmd))
    struct.pack_into('<II',im.d,16,ncmds+1,sizeofcmds+size)
    return bytes(im.d),{'cave_end':hex(cave),'publish_trampoline':hex(pub),'step_0c5b':hex(step),'vendor_init_type0':hex(type0_a),'callbacks':hex(cb),'h4_prestop':hex(pre),'advertising_start':hex(adv_start)}

if __name__=='__main__':
    out,info=patch(open(sys.argv[1],'rb').read())
    open(sys.argv[2],'wb').write(out)
    for k,v in info.items():print(k,v)
