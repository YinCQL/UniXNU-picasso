"""Exact 20H392/D22 optional alternate-route adaptation, performed offline.

The original arm64 ad-hoc SHA256 CodeDirectory already has 4K slots. Preserve
its identifier, special slots, requirements, UUID and complete Mach-O layout;
replace one constructor argument and its one code-page digest. The ordinary
code-signature verifier must accept both inputs and outputs.
"""
import hashlib
import os
from pathlib import Path
import struct
import tomllib

from picasso_ui_trust import library_identity

STOCK_SHA256 = '9a0ad6191f3d61ab148810895520b41f23dcaf5ecac0541e2406d8e82f3eb8ff'
INFO_SHA256 = 'd193b4bbbbc8e565830540b2d67fddc04d8a05d8c4d647eb1571a8f09a65c1ce'
SITE = 0x32aef0
OLD = 0x52800003  # mov w3, #0: mandatory alternate VAD
NEW = 0x52800023  # mov w3, #1: optional alternate VAD
CONTEXT = bytes.fromhex('6101805203008052e42e8c5204cdae72e52e8c52056dac72c68c8c52a62dad7207008052bb150094')
SIGNATURE = 0x6483e0
DIRECTORY = SIGNATURE + 36
HASHES = DIRECTORY + 223
SLOT = SITE // 4096
DIGEST = HASHES + SLOT * 32
DEFAULT = Path('/home/y/audio-reference-20H392/root/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')


def require_target(root):
    board = tomllib.loads((root / 'board.toml').read_text())
    target = board['ipsw']
    if board['board']['name'] != 'picasso' or any(target[k] != v for k, v in
            dict(product='iPhone10,3', device_class='d22ap', build='20H392').items()):
        raise ValueError('Optional audio route requires Picasso with D22/20H392')


def prepare_bytes(original, info):
    if hashlib.sha256(original).hexdigest() != STOCK_SHA256:
        raise ValueError('VirtualAudio is not the exact original 20H392 input')
    if hashlib.sha256(info).hexdigest() != INFO_SHA256:
        raise ValueError('VirtualAudio Info.plist differs from its signed input')
    before = library_identity(original)
    if before['code_page_bytes'] != 4096 or before['code_slots'] != 1609:
        raise ValueError('Unexpected original code signing layout')
    if original[SITE-4:SITE+36] != CONTEXT:
        raise ValueError('Alternate route constructor changed')
    # w3 passes through 0x3305fc to spec+0x38 at 0x190ab8. The original
    # recursion at 0x877c4/0x877c8 skips a failed alternate only for value 1.
    # phki/pnli, vhaw/chaw/imdf and every other candidate remain unchanged.
    patched = bytearray(original)
    struct.pack_into('<I', patched, SITE, NEW)
    page = bytes(patched[SLOT*4096:(SLOT+1)*4096])
    patched[DIGEST:DIGEST+32] = hashlib.sha256(page).digest()
    patched = bytes(patched)
    identity = library_identity(patched)
    changes = [i for i, (a, b) in enumerate(zip(original, patched)) if a != b]
    if any(i != SITE and not DIGEST <= i < DIGEST+32 for i in changes):
        raise ValueError('Changes outside the instruction and code-page digest')
    if patched[:SITE] != original[:SITE] or patched[SITE+4:DIGEST] != original[SITE+4:DIGEST]:
        raise ValueError('Unexpected Mach-O modification')
    report = dict(stock_sha256=STOCK_SHA256, stock_cdhash=before['cdhash'],
                  info_sha256=INFO_SHA256, identity=identity, instruction_offset=SITE,
                  original_instruction=OLD, patched_instruction=NEW,
                  signature_digest_offset=DIGEST, changed_byte_offsets=changes,
                  optional_alternate='vhaw', missing_port='phki', hardware_tested=False)
    return patched, report


def build(root, output):
    require_target(root)
    source = Path(os.environ.get('PICASSO_AUDIO_VIRTUAL_AUDIO', str(DEFAULT)))
    original = (source / 'VirtualAudio').read_bytes()
    info = (source / 'Info.plist').read_bytes()
    patched, report = prepare_bytes(original, info)
    bundle = output / 'VirtualAudio.plugin'
    bundle.mkdir(parents=True, exist_ok=True)
    (bundle / 'VirtualAudio').write_bytes(patched)
    (bundle / 'Info.plist').write_bytes(info)
    return report, {str(source / 'VirtualAudio'): STOCK_SHA256,
                    str(source / 'Info.plist'): INFO_SHA256}


def verify(output):
    # Reverse only the declared adaptation, then require the full original
    # file hash. This proves there are no extra code/data/signature changes.
    patched = (output / 'VirtualAudio.plugin/VirtualAudio').read_bytes()
    info = (output / 'VirtualAudio.plugin/Info.plist').read_bytes()
    if len(patched) != 6657136 or struct.unpack_from('<I', patched, SITE)[0] != NEW:
        raise ValueError('Missing optional alternate route')
    original = bytearray(patched)
    struct.pack_into('<I', original, SITE, OLD)
    original[DIGEST:DIGEST+32] = hashlib.sha256(original[SLOT*4096:(SLOT+1)*4096]).digest()
    expected, report = prepare_bytes(bytes(original), info)
    if patched != expected:
        raise ValueError('Patched VirtualAudio differs from exact signed output')
    return report
