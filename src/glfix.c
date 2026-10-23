// glfix: a thin OpenGL ES 2 layer for LEGO Ninjago: Tournament under THUMB.
//
// Shipped inside the APK as lib/armeabi-v7a/libGLESv2.so. THUMB loads it as a
// sibling of the game's library, and THUMB resolves symbols from app libraries
// before its own thunks, so the functions defined here replace the system
// ones for the game. Everything not defined here still goes straight to the
// real driver. Wrapped originals are fetched with eglGetProcAddress.
//
// What it does:
//   - Decodes texture formats the GPU can't sample (ATC from the Adreno build
//     of the game data, DXT from the Tegra build) in software and uploads the
//     result uncompressed. Without this the 3D world renders black.
//   - Reports shader compile/link failures and texture upload errors to
//     Android/data/com.lego.ninjago.toe/files/graphics.log.

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DBG_FILE "/sdcard/Android/data/com.lego.ninjago.toe/files/graphics.log"
#include "dbglog.h"

#define EXPORT __attribute__((visibility("default")))
#define TAG "glfix"
#define LOGI(...) dbg_log(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) dbg_log(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

#define REAL(ret, name, args)                                                  \
    static ret(*real_##name) args;                                             \
    static void load_##name(void) {                                            \
        if (!real_##name) real_##name = (ret(*) args)eglGetProcAddress(#name); \
    }

REAL(void, glCompileShader, (GLuint))
REAL(void, glLinkProgram, (GLuint))
REAL(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))
REAL(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*))
REAL(const GLubyte*, glGetString, (GLenum))

// ------------------------------------------------------------ diagnostics

static int g_shaders, g_shaderFails, g_programs, g_linkFails, g_glErrors;

EXPORT void glCompileShader(GLuint shader) {
    load_glCompileShader();
    real_glCompileShader(shader);
    g_shaders++;
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok || ++g_shaderFails > 10) return;
    char log[1024] = {0};
    glGetShaderInfoLog(shader, sizeof log, NULL, log);
    LOGW("shader %u failed to compile (%d of %d so far): %s", shader, g_shaderFails, g_shaders, log);
}

EXPORT void glLinkProgram(GLuint program) {
    load_glLinkProgram();
    real_glLinkProgram(program);
    g_programs++;
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok || ++g_linkFails > 10) return;
    char log[1024] = {0};
    glGetProgramInfoLog(program, sizeof log, NULL, log);
    LOGW("program %u failed to link (%d of %d so far): %s", program, g_linkFails, g_programs, log);
}

static void check_error(const char* what, GLenum format) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR && ++g_glErrors <= 20) LOGW("%s (format 0x%x) -> GL error 0x%x", what, format, e);
}

EXPORT const GLubyte* glGetString(GLenum name) {
    load_glGetString();
    const GLubyte* s = real_glGetString(name);
    static int logged;
    if (!logged && name == GL_RENDERER) {
        logged = 1;
        LOGI("GPU: %s", s ? (const char*)s : "(unknown)");
    }
    return s;
}

static int has_extension(const char* ext) {
    load_glGetString();
    const char* all = (const char*)real_glGetString(GL_EXTENSIONS);
    return all && strstr(all, ext);
}

// ------------------------------------------------------- texture decoding
//
// Every decoder fills a 4x4 block of RGBA pixels; upload_decoded() writes the
// blocks out as RGB565 (formats without alpha) or RGBA8888.

typedef uint8_t Px[4];

static inline uint8_t expand5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t expand6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

static void rgb565(uint32_t c, Px out) {
    out[0] = expand5((c >> 11) & 0x1f);
    out[1] = expand6((c >> 5) & 0x3f);
    out[2] = expand5(c & 0x1f);
    out[3] = 255;
}

static void mix(const Px a, const Px b, int wa, int wb, int div, Px out) {
    for (int i = 0; i < 3; i++) out[i] = (uint8_t)((wa * a[i] + wb * b[i]) / div);
    out[3] = 255;
}

static void apply_indices(const uint8_t* idx4, const Px pal[4], Px block[16]) {
    uint32_t idx = idx4[0] | (idx4[1] << 8) | (idx4[2] << 16) | ((uint32_t)idx4[3] << 24);
    for (int i = 0; i < 16; i++) memcpy(block[i], pal[(idx >> (2 * i)) & 3], 4);
}

// ATC colour block (AMD_compressed_ATC_texture, Adreno only). Layout as in
// bimg's decodeBlockATC: endpoint 0 is RGB555 plus a mode bit, endpoint 1 is
// RGB565, then 2-bit indices.
static void atc_color(const uint8_t* src, Px block[16]) {
    uint32_t c0 = src[0] | (src[1] << 8);
    uint32_t c1 = src[2] | (src[3] << 8);
    Px pal[4], e0, e1;
    e0[0] = expand5((c0 >> 10) & 0x1f), e0[1] = expand5((c0 >> 5) & 0x1f), e0[2] = expand5(c0 & 0x1f), e0[3] = 255;
    rgb565(c1, e1);
    if (!(c0 & 0x8000)) {
        memcpy(pal[0], e0, 4);
        mix(e0, e1, 5, 3, 8, pal[1]);
        mix(e0, e1, 3, 5, 8, pal[2]);
    } else {
        pal[0][0] = pal[0][1] = pal[0][2] = 0, pal[0][3] = 255;
        for (int i = 0; i < 3; i++) {
            int v = e0[i] - e1[i] / 4;
            pal[1][i] = (uint8_t)(v < 0 ? 0 : v);
        }
        pal[1][3] = 255;
        memcpy(pal[2], e0, 4);
    }
    memcpy(pal[3], e1, 4);
    apply_indices(src + 4, (const Px*)pal, block);
}

// S3TC colour block. DXT1 switches to 3 colours + black when c0 <= c1 (black
// is transparent in the alpha variant); DXT3 and DXT5 always use 4 colours.
enum { DXT_4COLOR, DXT1_OPAQUE, DXT1_ALPHA };

static void dxt_color(const uint8_t* src, int mode, Px block[16]) {
    uint32_t c0 = src[0] | (src[1] << 8);
    uint32_t c1 = src[2] | (src[3] << 8);
    Px pal[4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || mode == DXT_4COLOR) {
        mix(pal[0], pal[1], 2, 1, 3, pal[2]);
        mix(pal[0], pal[1], 1, 2, 3, pal[3]);
    } else {
        mix(pal[0], pal[1], 1, 1, 2, pal[2]);
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        pal[3][3] = mode == DXT1_ALPHA ? 0 : 255;
    }
    apply_indices(src + 4, (const Px*)pal, block);
}

// 4 bits of alpha per texel (ATC explicit alpha, DXT3).
static void alpha_explicit(const uint8_t* src, Px block[16]) {
    for (int i = 0; i < 16; i++) block[i][3] = (uint8_t)(((src[i >> 1] >> ((i & 1) * 4)) & 0xf) * 17);
}

// Two alpha endpoints and 3-bit indices (ATC interpolated alpha, DXT5).
static void alpha_interp(const uint8_t* src, Px block[16]) {
    uint8_t v[8];
    v[0] = src[0];
    v[1] = src[1];
    if (v[0] > v[1]) {
        for (int i = 1; i < 7; i++) v[i + 1] = (uint8_t)(((7 - i) * v[0] + i * v[1]) / 7);
    } else {
        for (int i = 1; i < 5; i++) v[i + 1] = (uint8_t)(((5 - i) * v[0] + i * v[1]) / 5);
        v[6] = 0;
        v[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)src[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) block[i][3] = v[(bits >> (3 * i)) & 7];
}

typedef struct {
    GLenum format;
    const char* name;
    const char* extension;  // GPUs with this extension get the data as-is
    int blockBytes;
    int hasAlpha;
} Format;

static const Format g_formats[] = {
    {GL_ATC_RGB_AMD, "ATC", "GL_AMD_compressed_ATC_texture", 8, 0},
    {GL_ATC_RGBA_EXPLICIT_ALPHA_AMD, "ATC explicit alpha", "GL_AMD_compressed_ATC_texture", 16, 1},
    {GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD, "ATC interpolated alpha", "GL_AMD_compressed_ATC_texture", 16, 1},
    {GL_COMPRESSED_RGB_S3TC_DXT1_EXT, "DXT1", "GL_EXT_texture_compression_dxt1", 8, 0},
    {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, "DXT1 alpha", "GL_EXT_texture_compression_dxt1", 8, 1},
    {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, "DXT3", "GL_EXT_texture_compression_s3tc", 16, 1},
    {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, "DXT5", "GL_EXT_texture_compression_s3tc", 16, 1},
};

static void decode_block(GLenum f, const uint8_t* src, Px block[16]) {
    switch (f) {
    case GL_ATC_RGB_AMD: atc_color(src, block); break;
    case GL_ATC_RGBA_EXPLICIT_ALPHA_AMD: atc_color(src + 8, block), alpha_explicit(src, block); break;
    case GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD: atc_color(src + 8, block), alpha_interp(src, block); break;
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: dxt_color(src, DXT1_OPAQUE, block); break;
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: dxt_color(src, DXT1_ALPHA, block); break;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: dxt_color(src + 8, DXT_4COLOR, block), alpha_explicit(src, block); break;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: dxt_color(src + 8, DXT_4COLOR, block), alpha_interp(src, block); break;
    }
}

static int g_decoded[sizeof g_formats / sizeof g_formats[0]];
static signed char g_native[sizeof g_formats / sizeof g_formats[0]];  // 0 unknown, 1 native, -1 decode

static int upload_decoded(const Format* fmt, GLenum target, GLint level, GLsizei w, GLsizei h, GLsizei size,
                          const uint8_t* data) {
    if (w <= 0 || h <= 0 || !data) return 0;
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    if ((size_t)size < (size_t)bw * bh * fmt->blockBytes) {
        LOGW("%s upload %dx%d: %d bytes is too small", fmt->name, w, h, size);
        return 0;
    }
    int bpp = fmt->hasAlpha ? 4 : 2;
    uint8_t* px = malloc((size_t)w * h * bpp);
    if (!px) return 0;
    Px block[16];
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            decode_block(fmt->format, data + (size_t)(by * bw + bx) * fmt->blockBytes, block);
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                size_t p = (size_t)y * w + x;
                if (bpp == 2)
                    ((uint16_t*)px)[p] =
                        (uint16_t)(((block[i][0] >> 3) << 11) | ((block[i][1] >> 2) << 5) | (block[i][2] >> 3));
                else
                    memcpy(px + p * 4, block[i], 4);
            }
        }
    }
    GLint align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    load_glTexImage2D();
    if (bpp == 2) real_glTexImage2D(target, level, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
    else real_glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    free(px);
    return 1;
}

EXPORT void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                                   GLint border, GLsizei imageSize, const void* data) {
    for (size_t i = 0; i < sizeof g_formats / sizeof g_formats[0]; i++) {
        const Format* fmt = &g_formats[i];
        if (fmt->format != internalformat) continue;
        if (!g_native[i]) {
            g_native[i] = has_extension(fmt->extension) ? 1 : -1;
            LOGI("%s textures: %s", fmt->name, g_native[i] > 0 ? "supported by the GPU" : "decoding in software");
        }
        if (g_native[i] < 0 && upload_decoded(fmt, target, level, width, height, imageSize, data)) {
            if (++g_decoded[i] % 2000 == 0) LOGI("%d %s texture levels decoded", g_decoded[i], fmt->name);
            check_error("decoded texture upload", internalformat);
            return;
        }
        break;
    }
    load_glCompressedTexImage2D();
    real_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    check_error("glCompressedTexImage2D", internalformat);
}
