#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

/** 17.14 Fixed-Point Arithmetic definitions.
    Pintos does not support floating point in the kernel.
    We represent fixed-point real numbers with 1 sign bit,
    17 integer bits, and 14 fractional bits. */

#define FP_SHIFT 14
#define FP_F (1 << FP_SHIFT)
#define FP_HALF (FP_F / 2)

/** Convert integer n to fixed point. */
#define INT_TO_FP(n) ((int) ((n) * FP_F))

/** Convert fixed point x to integer (rounding toward zero). */
#define FP_TO_INT_ZERO(x) ((int) ((x) / FP_F))
#define FP_TO_INT(x) FP_TO_INT_ZERO(x)

/** Convert fixed point x to integer (rounding to nearest). */
#define FP_TO_INT_ROUND(x) ((int) ((x) >= 0 ? (((x) + FP_HALF) / FP_F) : (((x) - FP_HALF) / FP_F)))

/** Add two fixed-point numbers. */
#define FP_ADD(x, y) ((x) + (y))

/** Subtract fixed-point y from fixed-point x. */
#define FP_SUB(x, y) ((x) - (y))

/** Add fixed-point x and integer n. */
#define FP_ADD_INT(x, n) ((x) + ((n) * FP_F))

/** Subtract integer n from fixed-point x. */
#define FP_SUB_INT(x, n) ((x) - ((n) * FP_F))

/** Multiply two fixed-point numbers. */
#define FP_MUL(x, y) ((int) (((int64_t) (x)) * (y) / FP_F))

/** Multiply fixed-point x by integer n. */
#define FP_MUL_INT(x, n) ((x) * (n))

/** Divide fixed-point x by fixed-point y. */
#define FP_DIV(x, y) ((int) (((int64_t) (x)) * FP_F / (y)))

/** Divide fixed-point x by integer n. */
#define FP_DIV_INT(x, n) ((x) / (n))

#endif /**< threads/fixed-point.h */
