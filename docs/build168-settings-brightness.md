# build168: About and brightness requests

The 2026-11-06 11:12:36 capture has Preferences waiting in
`PSGAboutDataSource::_macAddress -> SCNetworkInterfaceCopyAllWithPreferences -> dispatch_once`.
The owning worker waits for `Plugin:InterfaceNamer` via `SCDynamicStoreNotifyWait`.
There are no `set_dir_stats` lines in this capture. Loading IONetworkingFamily
succeeded, but finding IONetworkStack alone did not complete the naming handshake.

## Native network timeout configuration

Apple configd [1241.60.3.0.1](https://github.com/apple-oss-distributions/configd/blob/configd-1241.60.3.0.1/Plugins/InterfaceNamer/ifnamer.c)
reads `WaitStackTimeout` and `WaitQuietTimeout` from the InterfaceNamer bundle.
Its defaults are 300 and 240 seconds. The actual 20H392 configd was also inspected:
string references at 0x100016d60 / 0x100016e84 lead to CFNumber double conversion
and positive-value checks. The shipped plist is 843 bytes, SHA256
`06654ce20ed89d9ccd7aa8e68fce050bd7d2ac0f9e4dedcdb615a62fa0a9c7a9`.

Before launchd, the board verifies that original plist, writes a copy with both
limits set to 5 seconds, and mounts a narrow read-only union over the bundle.
All original metadata is retained, including `Builtin=true`. configd uses its
built-in implementation; no executable or code-signing policy changes. A bounded
reader checks type, requested size, short reads and close errors. The upper
contains only Info.plist; parent types, ownership, modes and the visible bytes
are checked. Failure is logged and the original boot continues.

The original timer publishes its real TIMEOUT. No busy counter is reset, and
no interface, MAC address or QUIET/COMPLETE notification is synthesized. Read-only
configd diagnostics report its real root busy state, up to 24 busy entries, and
IONetworkStack open results. This bounds the startup wait; it does not fix every
unfinished device match or implement Wi-Fi. Slow future hardware can still arrive
through the normal notifications. Runtime acceptance of this view remains to be
verified on the next image.

The [network tutorial](https://unixnu.com/porting/network) targets PCIe WCN7850.
Picasso uses SM7250 integrated WCSS/SNOC; its current NativeWlan has no functional
WLFW QMI client, firmware handoff, data path or Apple80211 interface. The Wi-Fi
offline jobs therefore remain disabled. Enabling those jobs alone cannot solve
this driver gap and is not required to complete the native naming timeout.

## Brightness negotiation

The log samples contain 56 full-property MAC rejections and 85 allowed requests;
none reaches the brightness mailbox or WLED brightness write. Allowed single-key
requests are not proven to be brightness. The original CoreBrightness API sends
top-level brightness / FadePeriod / optional backlight-level numbers.

The old fallback required a platform/name/executable-path heuristic before asking
MAC about the brightness-only subset. This redundant gate could prevent an
otherwise permitted request. Remove that gate: only the live board service and
known, range-checked fields qualify; the subset must still pass the original MAC
check for the actual caller. Unsupported optional fields are dropped, and the
existing driver handles only the accepted brightness request. No process/service
grant is added. A subset rejection remains an error. Logs distinguish whether the
second check ran, its verdict, and the first 16 requests' actual keys/types.

The log cannot distinguish a former caller-gate failure from a second MAC rejection.
This is a compatibility repair candidate, not evidence that physical brightness
has already changed. WLED transport and the tested display-power sequence remain
unchanged. Expect `attempted=1 -> final=0 -> queued=1 -> request -> WLED brightness`
on the next image; a nonzero final verdict must be investigated as such.

## Validation and seed

Host checks execute the replayed policies: 37 brightness MAC cases, 23 network
configuration cases, 9 bounded-reader cases, bounded busy observation, plus the
existing startup, UI, NVRAM and brightness/backend suites. Normal and sanitizers
run in CI. ARM API models are not a complete XNU SDK build or physical test.

No seed recipe inputs changed. Keep the existing 20 GiB Data, UUID
`72443F51-21DE-49BF-AA5A-BDBFEEA830ED` and recipe
`b5fc7d559c3ab0dd1c7fee9fe46674b50457772c63321f01ace72498de5a2832`.
Use `create_data_seed=false` and the matching complete linux-inputs. The generated
configuration is refreshed at boot, so it does not require regenerating the seed.
