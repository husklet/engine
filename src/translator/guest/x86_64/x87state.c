#include "x87state.h"

#include <string.h>

double hl_x86_ext80_load(const uint8_t image[10]) {
    uint64_t significand;
    uint16_t sign_exponent;
    int sign;
    int exponent;
    double value;
    memcpy(&significand, image, sizeof(significand));
    memcpy(&sign_exponent, image + 8, sizeof(sign_exponent));
    sign = sign_exponent >> 15;
    exponent = sign_exponent & 0x7fff;
    if (significand == 0 && exponent == 0) {
        uint64_t bits = (uint64_t)sign << 63;
        memcpy(&value, &bits, sizeof(value));
    } else if (exponent == 0x7fff) {
        uint64_t fraction = significand & ((UINT64_C(1) << 63) - 1);
        uint64_t bits = (uint64_t)sign << 63 | UINT64_C(0x7ff) << 52 |
                        (fraction != 0 ? UINT64_C(1) << 51 : 0);
        memcpy(&value, &bits, sizeof(value));
    } else {
        uint64_t bits;
        int scaled_exponent;
        value = (double)significand;
        memcpy(&bits, &value, sizeof(bits));
        scaled_exponent = (int)((bits >> 52) & 0x7ff) + exponent - 16383 - 63;
        if (scaled_exponent <= 0) {
            bits = (uint64_t)sign << 63;
        } else if (scaled_exponent >= 0x7ff) {
            bits = (uint64_t)sign << 63 | UINT64_C(0x7ff) << 52;
        } else {
            bits = (bits & ~(UINT64_C(0x7ff) << 52)) | (uint64_t)scaled_exponent << 52;
            bits = (bits & ~(UINT64_C(1) << 63)) | (uint64_t)sign << 63;
        }
        memcpy(&value, &bits, sizeof(value));
    }
    return value;
}

void hl_x86_ext80_store(double value, uint8_t image[10]) {
    uint64_t bits;
    uint64_t fraction;
    uint64_t significand;
    uint16_t sign_exponent;
    int sign;
    int exponent;
    memcpy(&bits, &value, sizeof(bits));
    sign = (int)(bits >> 63);
    exponent = (int)((bits >> 52) & 0x7ff);
    fraction = bits & ((UINT64_C(1) << 52) - 1);
    if (exponent == 0) {
        significand = 0;
        sign_exponent = (uint16_t)(sign != 0 ? 0x8000 : 0);
    } else if (exponent == 0x7ff) {
        significand = (UINT64_C(1) << 63) | (fraction << 11);
        sign_exponent = (uint16_t)((sign << 15) | 0x7fff);
    } else {
        significand = (UINT64_C(1) << 63) | (fraction << 11);
        sign_exponent = (uint16_t)((sign << 15) | ((exponent - 1023 + 16383) & 0x7fff));
    }
    memcpy(image, &significand, sizeof(significand));
    memcpy(image + 8, &sign_exponent, sizeof(sign_exponent));
}
