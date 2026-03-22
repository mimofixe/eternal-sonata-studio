/* bcdec.h - v1.1 - BC1-7 texture decompression
   Public domain - no warranty implied; use at your own risk.
*/

#ifndef BCDEC_H
#define BCDEC_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

    void bcdec_bc1(const void* compressedBlock, void* decompressedBlock, int destinationPitch);
    void bcdec_bc3(const void* compressedBlock, void* decompressedBlock, int destinationPitch);

#ifdef __cplusplus
}
#endif

#endif /* BCDEC_H */

#ifdef BCDEC_IMPLEMENTATION

static void bcdec__color_block(const uint8_t* src, uint8_t* dst, int dstPitch, int onlyOpaqueMode) {
    uint16_t c0 = src[0] | (src[1] << 8);
    uint16_t c1 = src[2] | (src[3] << 8);

    uint8_t r0 = ((c0 >> 11) & 0x1F) << 3;
    uint8_t g0 = ((c0 >> 5) & 0x3F) << 2;
    uint8_t b0 = (c0 & 0x1F) << 3;

    uint8_t r1 = ((c1 >> 11) & 0x1F) << 3;
    uint8_t g1 = ((c1 >> 5) & 0x3F) << 2;
    uint8_t b1 = (c1 & 0x1F) << 3;

    uint8_t colors[4][4];

    colors[0][0] = r0; colors[0][1] = g0; colors[0][2] = b0; colors[0][3] = 255;
    colors[1][0] = r1; colors[1][1] = g1; colors[1][2] = b1; colors[1][3] = 255;

    if (c0 > c1 || onlyOpaqueMode) {
        colors[2][0] = (2 * r0 + r1) / 3;
        colors[2][1] = (2 * g0 + g1) / 3;
        colors[2][2] = (2 * b0 + b1) / 3;
        colors[2][3] = 255;

        colors[3][0] = (r0 + 2 * r1) / 3;
        colors[3][1] = (g0 + 2 * g1) / 3;
        colors[3][2] = (b0 + 2 * b1) / 3;
        colors[3][3] = 255;
    }
    else {
        colors[2][0] = (r0 + r1) / 2;
        colors[2][1] = (g0 + g1) / 2;
        colors[2][2] = (b0 + b1) / 2;
        colors[2][3] = 255;

        colors[3][0] = 0;
        colors[3][1] = 0;
        colors[3][2] = 0;
        colors[3][3] = 0;
    }

    uint32_t indices = src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);

    for (int i = 0; i < 16; i++) {
        int index = (indices >> (i * 2)) & 3;
        uint8_t* pixel = dst + (i / 4) * dstPitch + (i % 4) * 4;
        memcpy(pixel, colors[index], 4);
    }
}

void bcdec_bc1(const void* compressedBlock, void* decompressedBlock, int destinationPitch) {
    bcdec__color_block((const uint8_t*)compressedBlock, (uint8_t*)decompressedBlock, destinationPitch, 0);
}

void bcdec_bc3(const void* compressedBlock, void* decompressedBlock, int destinationPitch) {
    const uint8_t* src = (const uint8_t*)compressedBlock;
    uint8_t* dst = (uint8_t*)decompressedBlock;

    uint8_t a0 = src[0];
    uint8_t a1 = src[1];

    uint8_t alphas[8];
    alphas[0] = a0;
    alphas[1] = a1;

    if (a0 > a1) {
        for (int i = 0; i < 6; i++)
            alphas[i + 2] = ((6 - i) * a0 + (i + 1) * a1) / 7;
    }
    else {
        for (int i = 0; i < 4; i++)
            alphas[i + 2] = ((4 - i) * a0 + (i + 1) * a1) / 5;
        alphas[6] = 0;
        alphas[7] = 255;
    }

    uint64_t indices = src[2] | ((uint64_t)src[3] << 8) | ((uint64_t)src[4] << 16) |
        ((uint64_t)src[5] << 24) | ((uint64_t)src[6] << 32) | ((uint64_t)src[7] << 40);

    bcdec__color_block(src + 8, dst, destinationPitch, 1);

    for (int i = 0; i < 16; i++) {
        int index = (indices >> (i * 3)) & 7;
        uint8_t* pixel = dst + (i / 4) * destinationPitch + (i % 4) * 4;
        pixel[3] = alphas[index];
    }
}

#endif /* BCDEC_IMPLEMENTATION */