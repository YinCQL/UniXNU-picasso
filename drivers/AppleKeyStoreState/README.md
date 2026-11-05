# Picasso unprotected keystore state

Adapted from [UniXNU/AppleKeyStoreState](https://github.com/UniXNU/AppleKeyStoreState)
at `42ff83e08b2265fe741d4adaf084bebe003452ba`, retaining its runtime class and
source filename. See [LICENSE](LICENSE).

The selected iPhone10,3 / 20H392 userland queries the `AppleKeyStore` matching class
while SpringBoard boots. Picasso now publishes this service with the explicit
`picasso-aks-state=1` and either volatile `/chosen/ephemeral-storage=1` with store mode 0, or `/chosen/picasso-unprotected-data=1` with store mode 2, provided an
original AppleKeyStore class is absent. It describes the selected
unprotected developer Data environment. Persistent mode remains unencrypted and without password/keybag protection. Protected Data is unsupported.

The client establishes a real session with selector 0. For device context 0,
selector 7 returns the unlocked-since-boot state and selectors 17/35 return the
original DER state layout, including no passcode. Other contexts and operations
return errors. In particular, selector 14 does **not** manufacture a system
keybag handle, and bag creation, loading, saving, unlocking, key operations,
assertions and asynchronous notifications are unsupported. Matching publication
uses ordinary IOKit. `KeyOperationsAvailable=false` and `StateQueriesOnly=true`
describe this boundary in the registry.

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
