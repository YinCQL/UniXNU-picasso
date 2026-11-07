/*
 * ca_hooks: the CoreAnimation-side half of the OpenGL ES route (NATIVEGL).
 *
 * CoreAnimation's OpenGL ES renderer keeps the current "function" (one of its ninety fragment programs)
 * in a byte of its context, and the numbers a function is given go through GLContext::function_uniform().
 * libNativeGL's eagl.c draws some functions CoreAnimation's own shaders do not with programs of its own
 * (variable blur, vibrant matrices, downsample, shadow blur), and needs to be told which function a draw
 * is and what numbers it was given. HakiOS's inject.c does this by replacing vtable slots of
 * CA::WindowServer::EAGLServer and CA::OGL::GLESContext; the same slots on iOS 16.7.16 are:
 *
 *   EAGLServer::renderer              vtable 0x1d5b39ed0 + 0x1a0   function 0x1882eaf3c
 *   EAGLServer::render_update         vtable 0x1d5b39ed0 + 0x178   function 0x1883601dc
 *   Context::compile_shader           vtable 0x1d5b2f6e8 + 0x060   function 0x1884053fc
 *   GLContext::function_supported()   vtable 0x1d5b2f6e8 + 0x378   function 0x1884b2164
 *   GLContext::function_uniform       vtable 0x1d5b2f6e8 + 0x388   function 0x1884b21ac
 *
 * (verify: GLContext::function_supported() loads the function byte at [[context+0x10]+0x10], exactly what
 * HakiOS's hook reads). Off unless the kernel command line carries glhooks=1; each slot is validated
 * against its expected address plus the QuartzCore slide, and left alone if it holds anything else.
 */
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

/* From eagl.c: the per-function numbers, and a function CoreAnimation says it does not have. */
extern void hakios_gl_function_values(void *context, unsigned function, unsigned index, size_t components,
    size_t count, const float *values);
extern void hakios_gl_function_refused(unsigned function);
/* From eagl.c: a line to the kernel's console (the window server's stderr is not the console). */
extern void nativegl_text(const char *text);

#define VT_EAGL_SERVER       0x1d5b39ed0UL
#define VT_GLES_CONTEXT      0x1d5b2f6e8UL
#define SLOT(vtable, offset) ((vtable) + 0x10 + (offset))

static intptr_t slide;
static int said;

static void
say(const char *what)
{
    fprintf(stderr, "nativegl-ca: %s\n", what);
    fflush(stderr);
}

/* The function byte CoreAnimation keeps for the draw it is making. */
static unsigned
function_of(void *context)
{
    const uint8_t *context_state = *(const uint8_t **)((const char *)context + 0x10);
    return context_state ? context_state[0x10] : 0xff;
}

static int
boot_argument(const char *name)
{
    char arguments[1024] = "";
    size_t size = sizeof(arguments) - 1;
    if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0)
        return 0;
    return strstr(arguments, name) != NULL;
}

/*
 * Replace one vtable entry, if it still holds the function iOS 16.7.16 puts there: the tables live in
 * read-only memory shared by every process, so the page is copied private before it is written.
 */
static void *
replace_virtual(const char *what, uintptr_t vtable, unsigned slot, uintptr_t expected, void *replacement)
{
    uintptr_t *entry = (uintptr_t *)(vtable + slide + 0x10 + slot);
    const uintptr_t want = expected + (uintptr_t)slide;
    vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
    void *original;

    if ((*entry & 0x00FFFFFFFFFFFFFFull) != (want & 0x00FFFFFFFFFFFFFFull)) {
        char text[160];
        snprintf(text, sizeof text, "%s: found %#llx, expected %#llx; left alone", what,
                 (unsigned long long)(*entry - (uintptr_t)slide), (unsigned long long)expected);
        say(text);
        return NULL;
    }
    if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
        KERN_SUCCESS) {
        char text[96];
        snprintf(text, sizeof text, "%s: cannot make the table writable", what);
        say(text);
        return NULL;
    }
    original = (void *)*entry;
    *entry = (uintptr_t)replacement;
    vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
    return original;
}

static void *(*orig_renderer)(void *server);
static void (*orig_render_update)(void *server, void *update, unsigned *flags);
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static int (*orig_function_supported)(void *context);
static uintptr_t (*orig_compile_shader)(void *context, const char *source);

/* A server with no renderer tries again every frame, which costs a Metal and an EAGL context each time. */
static void *
my_renderer(void *server)
{
    static int failed;
    void *renderer;
    if (failed)
        return NULL;
    renderer = orig_renderer(server);
    if (renderer == NULL) {
        failed = 1;
        say("no accelerated renderer; the renderer is not tried again");
    } else if (!said) {
        said = 1;
        say("an accelerated renderer exists");
    }
    return renderer;
}

static void
my_render_update(void *server, void *update, unsigned *flags)
{
    orig_render_update(server, update, flags);
}

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
    unsigned function = function_of(context);
    if (function < 90)
        hakios_gl_function_values(context, function, index, components, count, values);
    orig_function_uniform(context, index, components, count, values);
}

static int
my_function_supported(void *context)
{
    int supported = orig_function_supported(context);
    unsigned function = function_of(context);
    if (!supported && function < 90)
        hakios_gl_function_refused(function);
    return supported;
}

static uintptr_t
my_compile_shader(void *context, const char *source)
{
    static unsigned seen;
    if (seen++ == 0)
        say("CoreAnimation compiles its shaders through the renderer's table");
    return orig_compile_shader(context, source);
}

__attribute__((constructor)) static void
install(void)
{
    char text[128];
    uint32_t count = _dyld_image_count(), i;

    if (!boot_argument("glhooks=1")) {
        return;
    }
    for (i = 0; i < count; i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, "QuartzCore")) {
            slide = _dyld_get_image_vmaddr_slide(i);
            break;
        }
    }
    if (!slide) {
        say("QuartzCore is not there to find; no hooks");
        return;
    }
    orig_renderer = replace_virtual("renderer", VT_EAGL_SERVER, 0x1a0, 0x1882eaf3cUL, (void *)my_renderer);
    orig_render_update = replace_virtual("render_update", VT_EAGL_SERVER, 0x178, 0x1883601dcUL,
                                         (void *)my_render_update);
    orig_compile_shader = replace_virtual("compile_shader", VT_GLES_CONTEXT, 0x060, 0x1884053fcUL,
                                          (void *)my_compile_shader);
    orig_function_supported = replace_virtual("function_supported", VT_GLES_CONTEXT, 0x378, 0x1884b2164UL,
                                              (void *)my_function_supported);
    orig_function_uniform = replace_virtual("function_uniform", VT_GLES_CONTEXT, 0x388, 0x1884b21acUL,
                                            (void *)my_function_uniform);
    snprintf(text, sizeof text, "hooks: renderer %s, render_update %s, compile_shader %s, function_supported %s, "
             "function_uniform %s", orig_renderer ? "yes" : "no", orig_render_update ? "yes" : "no",
             orig_compile_shader ? "yes" : "no", orig_function_supported ? "yes" : "no",
             orig_function_uniform ? "yes" : "no");
    say(text);
    {
        char visible[64];
        snprintf(visible, sizeof visible, "ca hk r%u u%u c%u s%u n%u", orig_renderer != NULL,
                 orig_render_update != NULL, orig_compile_shader != NULL, orig_function_supported != NULL,
                 orig_function_uniform != NULL);
        nativegl_text(visible);
    }
}
