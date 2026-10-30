# iPhone X root policy

This is the host-verified signed policy for the 20H392 migration branch.
It is a build input, but the Darwin 22 port is incomplete and builds are gated.
See [the migration status](../../docs/iphone-x-migration.md).

| Property | Value |
| --- | --- |
| Root image SHA256 | `2c2aca3feb7585c91b80ec6b50cc1888ac9315f4c77d981566de5dd902eabefa` |
| Image bytes | 8,380,219,392 |
| OS whole disk | 5,635,081,728 bytes, including original GPT |
| SystemOS | 2,743,074,816 bytes |
| OS UUID | `DADD93F8-DBBF-4B17-A1DF-0F11E64172F7` |
| SystemOS UUID | `96096B28-B6DC-4BC7-9D3A-2632A0FE53F1` |
| Authentication record | 128 KiB, SHA256 |
| Records | 42,993 (OS) + 20,928 (SystemOS) |
| Signatures | RSA-3072 PKCS#1 v1.5 SHA256, independently verified with OpenSSL |
| Owner anchor | Existing key retained; SHA256 `68c19aa4796e89fb48e0cc2f8243131fe23d31d3cf59554f7298a6cf8882e704` |

The Windows reconstruction checks the original UDIF block CRCs and rereads
all output windows. Native hdiutil comparison, SDK compilation and device
root authentication remain pending. The attestation correctly records
`hardware_tested: false`, `boot_root_authenticated: false`, and no policy object.
The unchanged Apple images and private key are not committed.

`policy.S` embeds the public anchor and the two complete manifests with relative
`.incbin` paths. Each manifest contains a 160-byte header, 384-byte signature and
ordered SHA256 table. Split headers, signatures and hash tables are retained
for independent checking. `scripts/check-picasso-target.py` pins both complete
signed manifests and verifies the split files and target identity.

`scripts/build-arm-driver.py NativeUfs` takes the device policy directory first,
then the assembled-tree policy, then `build/verified-root-picasso`. A scratch
policy does not override a present device policy. The build compiles `policy.S`
from this directory so the relative paths stay valid.

Regenerate into a **new** directory with `scripts/build-picasso-verified-policy.py`
and the exact root image and existing owner key. On Windows use `--skip-object`.
Existing outputs are rejected, and failed staging directories are retained.
After validation, back up the previous policy and copy the generated files into
this directory; keep this README. Do not hand-edit signed artifacts.

The previous 21H461 policy is retained in local evidence at
`out/iphone-x-migration-20261031/policy-before/` and in Git history.
The owner private key stays in ignored `build/verified-root-host/keys/owner.pem`.
