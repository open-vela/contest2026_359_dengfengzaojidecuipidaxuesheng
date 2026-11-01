/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cmath>
#ifdef __NuttX__
/* The prepared NuttX cmath imports expf etc. but omits the standard float
 * overloads. Supply those only to this private SDK compilation, so float
 * lambdas and MFCC arithmetic have the same types as the host SDK. */
namespace std {
inline float exp(float x){return ::expf(x);}
inline float log(float x){return ::logf(x);}
inline float log10(float x){return ::log10f(x);}
inline float log2(float x){return ::log2f(x);}
inline float sqrt(float x){return ::sqrtf(x);}
inline float pow(float x,float y){return ::powf(x,y);}
inline float sin(float x){return ::sinf(x);}
inline float cos(float x){return ::cosf(x);}
inline float tanh(float x){return ::tanhf(x);}
inline float floor(float x){return ::floorf(x);}
inline float ceil(float x){return ::ceilf(x);}
inline float round(float x){return ::roundf(x);}
inline float fabs(float x){return ::fabsf(x);}
}
#endif
