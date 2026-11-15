# Picasso unprotected keystore state

Adapted from [UniXNU/AppleKeyStoreState](https://github.com/UniXNU/AppleKeyStoreState)
at `42ff83e08b2265fe741d4adaf084bebe003452ba`, retaining its runtime class and
source filename. See [LICENSE](LICENSE).

The selected iPhone10,3 / 20H392 userland queries the `AppleKeyStore` matching class
while SpringBoard boots. Picasso now publishes this service with the explicit
`picasso-aks-state=1` and either volatile `/chosen/ephemeral-storage=1` with store mode 0, or `/chosen/picasso-unprotected-data=1` with store mode 2, provided an
original AppleKeyStore class is absent. It describes the selected
unprotected developer Data environment. Persistent mode remains unencrypted and without password/keybag protection. Protected Data is unsupported.

The client establishes a session with selector 0. For compatibility with the
reference implementation, selectors 4/5/9/12/13/26/27/55/101/102 now return success
without performing an operation or returning data. Selector 14 returns synthetic
handle 0. Selectors 7/17/35 return the unprotected device state and ignore the
context handle; selector 31 returns an empty DER SET. These are development
compatibility stubs, not authentication, keybag creation/loading, password
protection or asynchronous notifications. They are permitted only
under the existing unprotected Data gate. `KeyOperationsAvailable=true` and
`CompatibilityStubs=true` describe this boundary in the registry.

Output descriptors are prepared before writes and completed afterward, including
short writes. Preparation, short-write and completion errors propagate to callers.

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

Selectors 10 and 11 implement RFC 3394 AES-256 software key wrapping for
system bag 0 and classes 1 through 12, using the existing kernel crypto API.
The wrapping key derives from the stable public chosen chip identity and the
class. The n+8 blob follows the observed unchanged 20H392 libaks ABI. Tampering,
class mismatch, unsupported bags, malformed sizes and short outputs fail.
This permits native Keychain storage without claiming hardware or passcode
protection. Reference-key operations remain unsupported. The board refuses
publication if its identity is absent. No user key bytes are logged.
The Linux host oracle uses OpenSSL libcrypto for an independent RFC3394
comparison; hardware and real SDK checks remain separate.
