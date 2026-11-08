/*
 * ngl_test: a check of the OpenGL ES route on the phone, without the window server.
 *
 * It makes an OSMesa context of OpenGL ES 3 (Mesa's freedreno driver behind it, through
 * drivers/NativeGL's libdrm stand-in on the NativeAdreno ABI), says which renderer answers,
 * draws a frame of 1080 x 2400 into a texture and reads pixels of it back, times frames of that
 * size, and imports an IOSurface as a texture and reads it to see whether the pixels are its own.
 *
 * Lines are "ngl_test: ..."; the exit status is 0 when every check passed.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);
extern void OSMesaDestroyContext(OSMesaContext context);
extern void (*OSMesaGetProcAddress(const char *name))(void);

extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
extern void glGenTextures(int n, unsigned *names);
extern void glBindTexture(unsigned target, unsigned texture);
extern void glTexImage2D(unsigned target, int level, int internal, int width, int height, int border, unsigned format,
    unsigned type, const void *pixels);
extern void glTexParameteri(unsigned target, unsigned name, int value);
extern void glGenFramebuffers(int n, unsigned *names);
extern void glBindFramebuffer(unsigned target, unsigned framebuffer);
extern void glFramebufferTexture2D(unsigned target, unsigned attachment, unsigned textarget, unsigned texture, int level);
extern unsigned glCheckFramebufferStatus(unsigned target);
extern void glViewport(int x, int y, int width, int height);
extern void glClearColor(float red, float green, float blue, float alpha);
extern void glClear(unsigned mask);
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned shader, int count, const char *const *strings, const int *lengths);
extern void glCompileShader(unsigned shader);
extern void glGetShaderiv(unsigned shader, unsigned name, int *value);
extern void glGetShaderInfoLog(unsigned shader, int size, int *length, char *log);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned program, unsigned shader);
extern void glBindAttribLocation(unsigned program, unsigned index, const char *name);
extern void glLinkProgram(unsigned program);
extern void glGetProgramiv(unsigned program, unsigned name, int *value);
extern void glUseProgram(unsigned program);
extern void glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized, int stride,
    const void *pointer);
extern void glEnableVertexAttribArray(unsigned index);
extern void glDrawArrays(unsigned mode, int first, int count);
extern void glFinish(void);
extern void glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);
extern void glPixelStorei(unsigned name, int value);
extern int glGetUniformLocation(unsigned program, const char *name);
extern void glUniform1i(int location, int value);

#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_DEPTH_BITS               0x30
#define OSMESA_STENCIL_BITS             0x31
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_UNSIGNED_BYTE                0x1401
#define GL_TEXTURE_2D                   0x0de1
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_NEAREST                      0x2600
#define GL_FRAMEBUFFER                  0x8d40
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_TRIANGLE_STRIP               0x0005
#define GL_VERTEX_SHADER                0x8b31
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_SHADING_LANGUAGE_VERSION     0x8b8c
#define GL_EXTENSIONS                   0x1f03
#define GL_PACK_ALIGNMENT               0x0d05

static uint64_t
now_us(void)
{
    static mach_timebase_info_data_t base;
    if (base.denom == 0)
        mach_timebase_info(&base);
    return mach_absolute_time() * base.numer / base.denom / 1000;
}

static int failures;
/* The host check runs against a simulated GPU that retires submissions without drawing: there, what pixels hold
 * is not the question, only that the path took them (NGL_IGNORE_PIXELS=1). */
static int ignore_pixels;

static void
check(int good, const char *what)
{
    printf("ngl_test: %s: %s\n", good ? "PASS" : "FAIL", what);
    if (!good)
        failures++;
}

static unsigned
shader(unsigned type, const char *text)
{
    unsigned name = glCreateShader(type);
    int good = 0;
    char log[512] = "";
    glShaderSource(name, 1, &text, NULL);
    glCompileShader(name);
    glGetShaderiv(name, GL_COMPILE_STATUS, &good);
    if (!good) {
        glGetShaderInfoLog(name, sizeof log, NULL, log);
        printf("ngl_test: a shader does not compile: %s\n", log);
    }
    return name;
}

static const char *vertex_text =
    "attribute vec2 where; varying vec2 at;\n"
    "void main() { gl_Position = vec4(where, 0.0, 1.0); at = where * 0.5 + 0.5; }\n";
static const char *fragment_text =
    "precision mediump float; varying vec2 at;\n"
    "void main() { gl_FragColor = vec4(at.x, at.y, 0.25, 1.0); }\n";

/* A triangle covering the viewport, drawn into a texture of the size given; a few pixels read back. */
static void
draw_frame(int width, int height, const char *what, int frames)
{
    static const float corners[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    unsigned target = 0, framebuffer = 0, program;
    uint32_t pixels[4];
    uint64_t least = ~0ull;
    int good = 0;

    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, vertex_text));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER, fragment_text));
    glBindAttribLocation(program, 0, "where");
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &good);
    printf("ngl_test: %s: %d x %d, program linked %d, framebuffer %#x\n", what, width, height, good,
        glCheckFramebufferStatus(GL_FRAMEBUFFER));
    if (!good || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        check(0, "a frame can be set up");
        return;
    }
    glUseProgram(program);
    glViewport(0, 0, width, height);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, corners);
    for (int frame = 0; frame < frames; frame++) {
        uint64_t began = now_us(), took;
        glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        took = now_us() - began;
        least = took < least ? took : least;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(width / 2, height / 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    printf("ngl_test: %s: the shortest of %d frames %llu us; middle pixels %08x %08x %08x %08x\n", what, frames,
        (unsigned long long)least, pixels[0], pixels[1], pixels[2], pixels[3]);
    check(glGetError() == 0, "no error from the frame");
}

/*
 * The memory of an IOSurface as a texture: what glEGLImageTargetTexture2DOES does here, the surface's ID
 * as the image. The surface is filled with a colour, drawn from, and the pixels drawn are read back.
 */
static void
surface_test(OSMesaContext context)
{
    void (*image_target)(unsigned, void *);
    CFMutableDictionaryRef properties;
    IOSurfaceRef surface;
    uint32_t wanted = 0xff332211u;   /* R G B A read back of BGRA bytes 11 22 33 ff */
    uint32_t *rows, read[16];
    int width = 64, height = 64, good = 0;
    unsigned texture = 0, target = 0, framebuffer = 0, program;

    image_target = (void (*)(unsigned, void *))OSMesaGetProcAddress("glEGLImageTargetTexture2DOES");
    if (image_target == NULL) {
        check(0, "the library has glEGLImageTargetTexture2DOES");
        return;
    }
    properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &width);
    CFDictionarySetValue(properties, kIOSurfaceWidth, number); CFRelease(number);
    CFNumberRef height_number = CFNumberCreate(NULL, kCFNumberIntType, &height);
    CFDictionarySetValue(properties, kIOSurfaceHeight, height_number); CFRelease(height_number);
    int bytes_per_row = width * 4;
    CFNumberRef row_number = CFNumberCreate(NULL, kCFNumberIntType, &bytes_per_row);
    CFDictionarySetValue(properties, kIOSurfaceBytesPerRow, row_number); CFRelease(row_number);
    int format = 'BGRA';
    CFNumberRef format_number = CFNumberCreate(NULL, kCFNumberIntType, &format);
    CFDictionarySetValue(properties, kIOSurfacePixelFormat, format_number); CFRelease(format_number);
    surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    if (surface == NULL) {
        check(0, "an IOSurface can be made");
        return;
    }
    IOSurfaceLock(surface, 0, NULL);
    rows = (uint32_t *)IOSurfaceGetBaseAddress(surface);
    for (int i = 0; i < width * height; i++)
        rows[i] = wanted;
    IOSurfaceUnlock(surface, 0, NULL);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    while (glGetError() != 0) { }
    image_target(GL_TEXTURE_2D, (void *)(uintptr_t)IOSurfaceGetID(surface));
    printf("ngl_test: surface %u as a texture: glGetError %#x\n", (unsigned)IOSurfaceGetID(surface), glGetError());
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, vertex_text));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER,
        "precision mediump float; varying vec2 at; uniform sampler2D picture;\n"
        "void main() { gl_FragColor = texture2D(picture, at); }\n"));
    glBindAttribLocation(program, 0, "where");
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &good);
    if (!good) {
        check(0, "the surface's program links");
        CFRelease(surface);
        return;
    }
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "picture"), 0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glViewport(0, 0, width, height);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, (const float[]){ -1, -1, 1, -1, -1, 1, 1, 1 });
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    glReadPixels(width / 2, height / 2, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, read);
    printf("ngl_test: surface: drawn pixels %08x %08x %08x\n", read[0], read[1], read[5]);
    if (ignore_pixels)
        printf("ngl_test: INFO: an IOSurface's pixels, not checked: %08x %08x %08x\n", read[0], read[1], read[5]);
    else
        check(read[0] == wanted || read[1] == wanted || read[5] == wanted, "an IOSurface's pixels are drawn from");
    CFRelease(surface);
}

int
main(void)
{
    const int attributes[] = {
        OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
        OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, 3, OSMESA_CONTEXT_MINOR_VERSION, 0,
        0
    };
    static unsigned char window[16 * 16 * 4];
    OSMesaContext context;
    ignore_pixels = getenv("NGL_IGNORE_PIXELS") != NULL;
    context = OSMesaCreateContextAttribs(attributes, NULL);

    if (context == NULL) {
        printf("ngl_test: no OpenGL ES 3 context (is libOSMesa.8.dylib beside this program?)\n");
        return 1;
    }
    if (!OSMesaMakeCurrent(context, window, GL_UNSIGNED_BYTE, 16, 16)) {
        printf("ngl_test: the context cannot be made current\n");
        return 1;
    }
    printf("ngl_test: vendor   %s\n", (const char *)glGetString(GL_VENDOR));
    printf("ngl_test: renderer %s\n", (const char *)glGetString(GL_RENDERER));
    printf("ngl_test: version  %s\n", (const char *)glGetString(GL_VERSION));
    printf("ngl_test: GLSL     %s\n", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    printf("ngl_test: extensions %.900s\n", (const char *)glGetString(GL_EXTENSIONS));
    draw_frame(1080, 2400, "a full frame", 8);
    if (getenv("NGL_SKIP_SURFACE"))
        printf("ngl_test: INFO: the IOSurface test is not run (NGL_SKIP_SURFACE)\n");
    else
        surface_test(context);
    OSMesaDestroyContext(context);
    printf("ngl_test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
