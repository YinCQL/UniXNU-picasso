# Picasso software AES provider

Based on UniXNU/NativeAES commit
`a34964b8c2fec2e83c8e4f68fb7d6fb7e4b2227b`, the component selected by this
board's fixed UniXNU revision. The local source adds only two startup log
messages. OriginalAESABI.h and LICENSE are unchanged upstream files.

This is a genuine `IOAESAccelerator` subclass using XNU's registered
corecrypto AES-CBC functions with supplied 128/192/256-bit software keys.
It explicitly refuses hardware-key selectors. It does not emulate UID/GID
keys, SEP or a Qualcomm crypto device.

Before publishing, it runs the upstream NIST known-answer tests and public
descriptor/raw/in-place interface tests. A failure prevents publication.
`build-picasso-aes.py` binds the exact original kernelcache/slide, audits all
181 virtual slots, and creates `build/native-aes/libNativeAES.a` through the
existing `XNU_NATIVE_AES` recipe. The original IOBSD personality and normal
class initialization order are retained, as in the pinned implementation.

Both CI workflows verify the delivered source/object/archive and all linked
dispatch slots. Host tests use real Windows CNG CBC with IOKit stand-ins;
they do not qualify the Apple SDK or the target's corecrypto execution.
