# policy/verified-root-picasso/

The qualified VerifiedRoot policy tree for picasso. **This is a build input, not
a build product.** It is committed because it cannot be regenerated in CI:

| Property | Value |
| --- | --- |
| Root image | the 21H461 `userdata-root.img`, SHA-256 `fd43ee70...03093` |
| Size | 9,382,658,048 bytes (8.74 GiB) |
| Windows | `OS` 6,077,580,800 B, `SystemOS` 3,303,014,400 B |
| Volume UUIDs | `04769D26-2CA0-4825-B443-E2E5F88300F2` (OS), `8846FE78-2910-4297-AF97-D59BB6F8FC16` (SystemOS) |
| Auth record | 128 KiB, SHA-256 per record |
| Records | 46,369 (OS) + 25,200 (SystemOS) |
| Signatures | RSA-3072 PKCS#1 v1.5 SHA-256, one per window |
| Total | 4.38 MiB / 15 generated files |

Regenerating it needs the 8.74 GiB root image **and the owner private key that
signed it** (the anchor in `trusted-key.bin` is that key's public half). A fresh
key produces a different anchor, so a regenerated tree will not match any
manifest that was already qualified.

## Layout

```
policy.S                     the assembly that embeds the blobs below
trusted-key.bin              the 395-byte owner anchor (public key)
os/manifest.bin              160 B header + 384 B signature + 46,369 x 32 B hashes
os/hashes.bin                the 1,483,808-byte hash table alone
os/{header,signature,header-signature}.bin   the signed pieces, kept for audit
systemos/...                 the same five files for SystemOS
attestation.json             what was checked, by what, and the digests
```

`.incbin` operands in `policy.S` are **relative to this directory**, so the tree
keeps working after a checkout on any machine. `scripts/build-arm-driver.py`
assembles it with this directory as the working directory.

## Consumers

- `scripts/build-arm-driver.py NativeUfs` — reads this tree first
  (`POLICY_COMMITTED`), and falls back to `build/verified-root-picasso/` so a
  developer regeneration still takes effect locally.
- `scripts/build-picasso-verified-policy.py` — regenerates the tree. Run it with
  `--output policy/verified-root-picasso` to refresh the committed copy, and
  `--skip-object` on a host without the Apple cross toolchain (the object is
  built by `build-arm-driver.py` when the kernel links). Regeneration replaces
  only the generated payload and keeps this README.

## Rules

- **Never commit the owner private key.** It lives in
  `build/verified-root-host/keys/owner.pem` on the build host; `build/` is
  ignored. Losing it means no future tree can extend this anchor.
- **Never hand-edit** `policy.S`, `*.bin` or `attestation.json`.
  `build-arm-driver.py` refuses a tree whose `policy.S` digest disagrees with
  `attestation.json`, so an edit fails the build rather than weakening the check.
- The attestation records `hardware_tested: false` and
  `boot_root_authenticated: false`; the policy was signed and self-verified, but
  a real boot against it still has to be observed on device.
- Produced on Windows from the retail 21H461 IPSW: the LZFSE chunks of the OS
  disk image decode with the `lzfse` PyPI wheel, so the generator no longer
  needs a macOS host for the hashing and signing half.
