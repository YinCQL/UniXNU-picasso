# Picasso ephemeral keystore state

Adapted from [UniXNU/AppleKeyStoreState](https://github.com/UniXNU/AppleKeyStoreState)
at `42ff83e08b2265fe741d4adaf084bebe003452ba`, retaining its runtime class and
source filename. See [LICENSE](LICENSE).

The fixed iPad7,1 / 21H461 userland queries the `AppleKeyStore` matching class
while SpringBoard boots. Picasso now publishes this service with the explicit
`picasso-aks-state=1` argument and `/chosen/ephemeral-storage=1`, provided an
original AppleKeyStore class is absent. It describes the selected unprotected
RAM Data environment. It cannot be used with persistent or protected Data.

The client establishes a real session with selector 0. Selectors 4/5/9/12/13/
26/27/55/101/102 are no-output bookkeeping calls. Selector 14 returns the
upstream-compatible synthetic system keybag handle 0 so keybagd can continue
without a `systembag.kb`; it does not create or load a keybag. Selector 7
returns the unlocked-since-boot state, and selectors 17/35 return the original
DER state layout, including no passcode. Selector 31 is the libaks device
configuration query (`aks_get_configuration`); it returns an empty DER
dictionary because this environment has no persistent keybag policy. Key
operations, bag contents and asynchronous notifications remain unsupported.
Matching publication uses ordinary IOKit. `KeyOperationsAvailable=false` and
`StateQueriesOnly=true` describe this boundary in the registry.

`scripts/build-picasso-display.py` compiles the component with the real platform
SDK command into the existing late archive and removes its automatic initializer
section. `package/PicassoKeyStore.cpp` initializes the metaclasses explicitly
before publication. The post-IOBSD display hook calls the probe even if the
display instance is unavailable. The artifact guard checks the initializer,
archive membership, source hashes and final exported lifecycle symbols.

Run `python scripts/check-picasso-keystore.py --output <new-directory>` for
ordinary and sanitizer host checks. These use IOKit stand-ins; real SDK linking
and device startup are separate checks. The target ABI evidence and remaining
boot dependencies are recorded in [the investigation](../../docs/keystore-state-investigation.md).
