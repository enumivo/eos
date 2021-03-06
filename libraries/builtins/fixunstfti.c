/* ===-- fixunstfsi.c - Implement __fixunstfsi -----------------------------===
 *
 *                     The LLVM Compiler Infrastructure
 *
 * This file is dual licensed under the MIT and the University of Illinois Open
 * Source Licenses. See LICENSE for details.
 *
 * ===----------------------------------------------------------------------===
 */

#include "fp128.h"

typedef float128_t fp_t;
typedef unsigned __int128 fixuint_t;
typedef unsigned __int128 tu_int;
typedef __int128 rep_t;

tu_int ___fixunstfti(fp_t a) {
    // Break a into sign, exponent, significand
    const rep_t aRep = toRep(a);
    const rep_t aAbs = aRep & absMask;
    const int sign = aRep & signBit ? -1 : 1;
    const int exponent = (aAbs >> significandBits) - exponentBias;
    const rep_t significand = (aAbs & significandMask) | implicitBit;

    // If either the value or the exponent is negative, the result is zero.
    if (sign == -1 || exponent < 0)
        return 0;

    // If the value is too large for the integer type, saturate.
    if ((unsigned)exponent >= sizeof(fixuint_t) * CHAR_BIT)
        return ~(fixuint_t)0;

    // If 0 <= exponent < significandBits, right shift to get the result.
    // Otherwise, shift left.
    if (exponent < significandBits)
        return significand >> (significandBits - exponent);
    else
        return (fixuint_t)significand << (exponent - significandBits);
}
