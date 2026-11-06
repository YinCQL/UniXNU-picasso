# Picasso 20 GiB developer Data

The owner authorized replacing the old Data on 2026-11-06. This is unencrypted
development storage. It does not implement keybags, passcodes or secure erasure.
No host tool in this change boots, flashes or writes the phone.

## Allocation and initialization

The measured build164 LUN 0 uses 4096-byte blocks, userdata 2883584..15330298,
root image 8380219392 bytes, GPT CRCs 3c17a352/d82988c2. These values and the
authenticated root must match before replacement is allowed.

| Region | First LBA | Last LBA | Size |
| --- | ---: | ---: | ---: |
| Root image | 2883584 | 4929535 | 8380219392 bytes |
| Auxiliary cache | 9759578 | 9825113 | 256 MiB |
| Data overlay | 9825114 | 15068154 | 20 GiB + 644 KiB |
| Existing tail reserve | 15068155 | 15330298 | 1 GiB |
| NVRAM record | 15256571 | 15256576 | 24 KiB |
| Auxiliary owner | 15256577 | 15256580 | 16 KiB |
| Initialization journal | 15256581 | 15256584 | 16 KiB |
| Console record | 15322107 | 15322236 | 520 KiB |

Data is 5242880 pages plus one header and 160 bitmap blocks. The generic
quarter-partition cap is replaced by this explicit maximum; partition/root
guard checks and per-command write windows remain. There is no 20 GiB RAM
allocation: the immutable packed seed and a 640 KiB bitmap back direct UFS I/O.
The 4 GiB volatile fallback remains a legacy path and cannot serve 20 GiB.

`picasso-data-replace-20g=1` permits creation of a journal only in an entirely
blank four-block journal window. The record binds the partition, root length,
GPT CRCs, Data location, page count and exact Setup seed SHA256. It uses the
existing two-slot CRC/FUA record format, kind 4, 4096-byte slot payload capacity.
Its 92-byte payload has five geometry fields (40 bytes plus two CRCs), page count
at offset 48, seed at 56 and phase at 88 (1 pending, 2 complete).

The pending record must be durable before resetting the Data bitmap/header.
Reset invalidates the header and flushes, clears the bitmap and flushes, then
commits the new seed header and flushes. Old payload blocks are ignored until
overwritten; this is logical replacement, not physical sanitization. Both
journal slots must contain complete records before IOMedia is published.
Otherwise a damaged complete slot could revive an obsolete pending record.
Completion refreshes both slots on a later resume as well.

An interrupted pending reset can restart because Data has never been published.
A complete record allows **resume only**; a changed seed, damaged Data header,
foreign journal or I/O/GPT failure is refused. A torn first journal with no
valid slot is also refused; it is never inferred to be empty. Such errors need
inspection, not repeated formatting. NVRAM/console startup shares the already
opened record transport window, and still requires persistent Data state 2/3.
Auxiliary storage keeps its independent blank/owner checks.

## Building and retaining the seed

For the first Compose of this revision, select `create_data_seed=true`.
Subsequent Compose runs normally leave it false and reuse the exact audited
base/Setup seed, UUID and signed UI library. Capacity and recipe changes require
a new seed; a previously completed on-device journal will refuse a different
identity even when this input is selected. Do not use it as a Data reset switch.

Missing reusable inputs now fail by default. The separate
`picasso-data-seed-<run>` artifact retains only packed seeds, library and audit
JSON for 90 days; the combined boot artifact still contains linux-inputs.
Save linux-inputs locally too: Actions retention is not a permanent backup.
The two expanded audit `.cdr` files remain excluded from upload.

The pinned upstream Setup tool is patched to read 36 bytes for its APFS magic
check rather than allocating the entire 20 GiB image. Full hashes, packed
identity, native macOS APFS checks and kernel UUID checks remain required.

## Device acceptance still required

First boot should report `PICASSO_DATA_INIT phase=1`, `complete=1`, Data state 3,
`ready=1 direct=1 volatile=0`, and a successful owner mount. Next boot should
report phase 2, state 2, the same UUID/seed, and retain a file or changed setting.
Check NVRAM generations, console records, and auxiliary qualification separately.
Host tests do not establish APFS power-loss recovery or device performance.

Build164 confirmed successful volatile 4 GiB Data and the user confirmed working
backlight off/on. The user reports Preferences waits only on the first About
page visit, alongside `set_dir_stats`; other UI remains responsive and later
visits are immediate. First-use directory/capacity statistics are a plausible
cause, not a proven stack attribution. Verify whether persistent Data retains
that cache across boots before altering APFS statistics.

References: [UFS porting](https://unixnu.com/porting/ufs), local UniXNU
docs/porting/ufs.md, docs/drivers/ram-block.md, docs/guide/userdata.md;
RamBlock 4be2d7eb05ef5a98126c91e0302c149140c0e3e1 and UniXNU
c740efb5b23e94453bb179f7801eb592baafacf6. Board geometry is measured Picasso
data, not copied Sheng hardware policy.
