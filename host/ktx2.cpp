// MHFU-Android -- KTX2 pictures for texture packs (host/texpack.c), through the
// Basis Universal transcoder (UASTC or ETC1S inside, zstd-supercompressed or
// not). PPSSPP's packs ship this way. Transcoded to what the GPU takes as it
// is, which keeps a high-resolution pack in a phone's memory: ASTC 4x4
// (phones), BC7 (PC graphics cards), else plain RGBA8.
#include <cstdint>
#include <cstdlib>
#include <mutex>

// basisu's asserts do not compile where uint64_t is unsigned long (Android).
#ifndef NDEBUG
#define NDEBUG
#endif
#include "basisu_transcoder.h"

// want: psprecomp's PSP_GL_ASTC4x4 (1) | PSP_GL_BC7 (2). *gl_fmt is the GL
// format of what came out (0 for RGBA8), *bytes its size.
extern "C" uint32_t *mhfu_ktx2_decode(const uint8_t *data, size_t size, int want, int *w, int *h,
                                      uint32_t *gl_fmt, uint32_t *bytes) {
    static std::once_flag once;
    std::call_once(once, [] { basist::basisu_transcoder_init(); });

    basist::ktx2_transcoder t;
    if (!t.init(data, (uint32_t)size) || !t.start_transcoding()) return nullptr;
    const uint32_t width = t.get_width(), height = t.get_height();
    if (!width || !height || width > 8192 || height > 8192) return nullptr;

    basist::transcoder_texture_format fmt = basist::transcoder_texture_format::cTFRGBA32;
    uint32_t gl = 0;
    if (want & 1) { fmt = basist::transcoder_texture_format::cTFASTC_4x4_RGBA; gl = 0x93B0; }
    else if (want & 2) { fmt = basist::transcoder_texture_format::cTFBC7_RGBA; gl = 0x8E8C; }

    uint32_t units, out_bytes, pitch = 0, rows = 0;                 // blocks, or pixels for RGBA
    if (gl) {
        const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
        units = bw * bh;
        out_bytes = units * 16;
    } else {
        units = width * height;
        out_bytes = units * 4;
        pitch = width;
        rows = height;
    }
    uint32_t *out = (uint32_t *)malloc(out_bytes);
    if (!out) return nullptr;
    if (!t.transcode_image_level(0, 0, 0, out, units, fmt, 0, pitch, rows)) {
        free(out);
        return nullptr;
    }
    *w = (int)width;
    *h = (int)height;
    *gl_fmt = gl;
    *bytes = out_bytes;
    return out;
}
