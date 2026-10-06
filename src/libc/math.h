#ifndef KERNEL_LIBC_MATH_H
#define KERNEL_LIBC_MATH_H

/*
 * Freestanding <math.h>.
 *
 * Pulls in the kernel's macro header (src/math.h -> macros.h: MIN/MAX/ALIGN_*)
 * and declares the maths functions implemented in src/libc_shim.c.
 *
 * The HUGE_VAL / HUGE_VALF / INFINITY / NAN macros matter to Lua: luaconf.h
 * checks "defined(HUGE_VAL) && !defined(HUGE_VALF)" to decide whether the
 * typed math function variants are available, so HUGE_VALF must be defined to
 * keep the same configuration the Linux build had with the host math.h.
 */

#include "../math.h"

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#define M_PI     3.14159265358979323846
#define M_E      2.7182818284590452354

double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double a, double b);
double pow(double a, double b);
double sqrt(double x);

double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);

double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);

#endif /* KERNEL_LIBC_MATH_H */
