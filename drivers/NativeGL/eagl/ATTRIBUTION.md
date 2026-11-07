# Where this comes from

`eagl.c`, `gl_osmesa.c`, `hakios.gl.h`, `eagl_slots.txt` and `mkeagl.py` are ported from the
HakiOS project (`hackios-xnu-kernel`, `userland/`), whose userland is MIT licensed (see its
LICENSE). They are the reference implementation of CoreAnimation's OpenGL ES renderer served by
Mesa's OSMesa over a Darwin GPU driver; here the driver behind OSMesa is `drivers/NativeGL`
(the NativeAdreno ABI) instead of HakiOS's `hakios-drm`, on iOS 16.7.16 (20H392) for Picasso.

Local changes are marked `NATIVEGL:` in the sources.
