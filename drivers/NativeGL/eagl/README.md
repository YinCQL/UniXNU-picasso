# libNativeGL: CoreAnimation's EAGL renderer on Mesa, for the GL route

The OpenGL ES route runs CoreAnimation's own `EAGLContext` renderer on Mesa 25.0.7 (OSMesa +
freedreno/ir3) over the libdrm stand-in in `drivers/NativeGL/nativegl_drm.c`, which speaks the
`NativeAdreno` user-client ABI and blits the finished frame into the display's IOSurface (one GPU
blit; the display's driver waits for that submission). It lives beside the Metal route
(`drivers/NativeAdrenoMetalPlugin.mm` installs it when `sheng-gpu-gl=1` is on the kernel command line
and the process is `backboardd`).

## The sources

| file | what it is |
|---|---|
| `eagl.c` | the 409-entry `-getMacroContextPrivate` dispatch table (generated layout from `eagl_slots.txt`), the EAGLContext method replacements, the IOSurface image bookkeeping, the perf/beat probes that print over the kernel text channel |
| `mkeagl.py` | generates the table layout from `eagl_slots.txt` and the exported `gl*` symbols of libOSMesa |
| `gl_osmesa.c` | the OSMesa context / framebuffer plumbing |
| `ca_hooks.c` | optional vtable hooks (`glhooks=1`): `CA::WindowServer::EAGLServer::renderer`/`render_update`, `CA::OGL::Context::compile_shader`, `GLContext::function_supported`/`function_uniform` |
| `ngl_test.c` | the host-side check (`ngl_test`) that drives a context, strings and a frame without the window server |
| `hakios.gl.h` | the small interface between the EAGL replacement and the hooks |

The table layout on iOS 16.7.16 (20H392) is the 16.5 layout: the context at `table[0]` and the
functions at the 16.5 offsets. The `+0x20` seen in 20H392's OpenGLES stubs is a bias of the TSD
pointer (`tpidrro_el0 -> [0xf0]` holds `macro-context - 0x20`), not a shift of the table.

## Building

The bundle and the addon are built from `temp/gpu-ios16` (the GL workspace):

```sh
cd temp/gpu-ios16
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 scripts/build-gl-ios16.py
python3 scripts/make-gpu-trust-ios16.py
rm -f ngpu-addon-ios16-gl.bin
python3 scripts/make-gpu-addon-ios16.py --publish --clients backboardd,SpringBoard ngpu-addon-ios16-gl.bin
```

then boot an image built with `scripts/build-wf-payload.sh TAG <addon> sheng-dpu-scanout=2
sheng-gpu-perf-index=4 sheng-gpu-gl=1 glwait=0 [glhooks=1]` (≤ 1000 bytes of kernel command line).
The addon must be rebuilt together with the trust bundle whenever `libNativeGL` changes, or AMFI
refuses the plugin.

## Measured (2026-11-07, home-screen scrolling)

52.0 fps median / 62.3 peak, frame 12 ms. The A/B evidence and the rejected alternatives are in
`docs/experiment-20261107-gl-route-optimization.md`.
