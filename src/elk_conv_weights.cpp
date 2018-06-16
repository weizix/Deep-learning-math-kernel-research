#include <assert.h>
#include <x86intrin.h>
#include "el_def.hpp"
#include "el_utils.hpp"
#include "elx_conv.hpp"
#include "elk_conv.hpp"

#undef ADD
#undef SUB
#undef FMADD
#undef FMSUB
#undef MUL
#define ADD _mm512_add_ps
#define SUB _mm512_sub_ps
#define MUL _mm512_mul_ps
#define FMADD _mm512_fmadd_ps
#define FMSUB _mm512_fmsub_ps

namespace euler {

template <D_WEIGHTS(
    typename Type, const int A, const int K, const int V, const int I)>
void convolution_winograd_kernel<R_WEIGHTS(Type, A, K, V, I)>::trans_weights(
    Type atweights[A][A][V][V], Type aweights[K][K][V][V])
{
  __trans_weights(winograd_template_parameter_t<R_WEIGHTS(Type, A, K, V, I)>(),
      atweights, aweights);
}

template <D_WEIGHTS(
    typename Type, const int A, const int K, const int V, const int I)>
void convolution_winograd_kernel<R_WEIGHTS(Type, A, K, V, I)>::__trans_weights(
    winograd_template_parameter_t<S_WEIGHTS(float, 4, 3, 16, ISA_GENERIC)>,
    Type atweights[A][A][V][V], Type aweights[K][K][V][V])
{
  const float r4 = 1.0f / 4.0f;
  const float r2 = 1.0f / 2.0f;

  float C10[16], C11[16], C12[16], C20[16], C21[16], C22[16];
#undef F
#undef T
#define F(h, w) aweights[h][w][_IV][_OV]
#define T(h, w) atweights[h][w][_IV][_OV]
#define C(c, n) C##c##n[_OV]
  for (int _IV = 0; _IV < 16; ++_IV) {
#pragma omp simd
    for (int _OV = 0; _OV < 16; ++_OV) {
      T(0,0) = F(0,0);
      T(1,0) = r2 * (F(0,0) - F(1,0) + F(2,0));
      T(2,0) = r2 * (F(0,0) + F(1,0) + F(2,0));
      T(3,0) = F(2,0);

      C(1,0) = r4 * (F(0,0) - F(0,1) + F(0,2));
      C(1,1) = r4 * (F(1,0) - F(1,1) + F(1,2));
      C(1,2) = r4 * (F(2,0) - F(2,1) + F(2,2));
      T(0,1) = 2 * C(1,0);
      T(1,1) = C(1,0) - C(1,1) + C(1,2);
      T(2,1) = C(1,0) + C(1,1) + C(1,2);
      T(3,1) = 2 * C(1,2);

      C(2,0) = r4 * (F(0,0) + F(0,1) + F(0,2));
      C(2,1) = r4 * (F(1,0) + F(1,1) + F(1,2));
      C(2,2) = r4 * (F(2,0) + F(2,1) + F(2,2));
      T(0,2) = 2 * C(2,0);
      T(1,2) = C(2,0) - C(2,1) + C(2,2);
      T(2,2) = C(2,0) + C(2,1) + C(2,2);
      T(3,2) = 2 * C(2,2);

      T(0,3) = F(0,2);
      T(1,3) = r2 * (F(0,2) - F(1,2) + F(2,2));
      T(2,3) = r2 * (F(0,2) + F(1,2) + F(2,2));
      T(3,3) = F(2,2);
    }
  }
}

template <D_WEIGHTS(
    typename Type, const int A, const int K, const int V, const int I)>
void convolution_winograd_kernel<R_WEIGHTS(Type, A, K, V, I)>::__trans_weights(
    winograd_template_parameter_t<S_WEIGHTS(float, 4, 3, 16, ISA_SKX_AVX512)>,
    Type atweights[A][A][V][V], Type aweights[K][K][V][V])
{
  ENABLE_AVX512F();

  // Constants
  __m512 r4 = _mm512_set_ps(IMM_BCAST16(1.0f / 4.0f));
  __m512 r2 = _mm512_set_ps(IMM_BCAST16(1.0f / 2.0f));
  __m512 z2 = _mm512_set_ps(IMM_BCAST16(2.0f));

  // Inputs
  __m512 f00, f10, f20, f01, f11, f21, f02, f12, f22;
  // Cache
  __m512 c10, c11, c12, c20, c21, c22;
  // Outputs
  __m512 /* t00, */t10, t20, /* t30, */t01, t11, t21, t31, t02, t12, t22,
      t32, /* t03, */t13, t23/*, t33 */;
#undef F
#undef T
#define F(h, w) aweights[h][w][_V]
#define T(h, w) atweights[h][w][_V]
  for (int _V = 0; _V < 16; ++_V) {
    f00 = _mm512_load_ps(F(0, 0));
    f01 = _mm512_load_ps(F(0, 1));
    f02 = _mm512_load_ps(F(0, 2));
    f10 = _mm512_load_ps(F(1, 0));
    f11 = _mm512_load_ps(F(1, 1));
    f12 = _mm512_load_ps(F(1, 2));
    f20 = _mm512_load_ps(F(2, 0));
    f21 = _mm512_load_ps(F(2, 1));
    f22 = _mm512_load_ps(F(2, 2));

    _mm512_store_ps(T(0, 0), f00);
    t10 = MUL(r2, ADD(SUB(f00, f10), f20));
    _mm512_store_ps(T(1, 0), t10);
    t20 = MUL(r2, ADD(ADD(f00, f10), f20));
    _mm512_store_ps(T(2, 0), t20);
    _mm512_store_ps(T(3, 0), f20);

    c10 = MUL(r4, ADD(SUB(f00, f01), f02));
    c11 = MUL(r4, ADD(SUB(f10, f11), f12));
    c12 = MUL(r4, ADD(SUB(f20, f21), f22));
    t01 = MUL(z2, c10);
    _mm512_store_ps(T(0, 1), t01);
    t11 = ADD(SUB(c10, c11), c12);
    _mm512_store_ps(T(1, 1), t11);
    t21 = ADD(ADD(c10, c11), c12);
    _mm512_store_ps(T(2, 1), t21);
    t31 = MUL(z2, c12);
    _mm512_store_ps(T(3, 1), t31);

    c20 = MUL(r4, ADD(ADD(f00, f01), f02));
    c21 = MUL(r4, ADD(ADD(f10, f11), f12));
    c22 = MUL(r4, ADD(ADD(f20, f21), f22));
    t02 = MUL(z2, c20);
    _mm512_store_ps(T(0, 2), t02);
    t12 = ADD(SUB(c20, c21), c22);
    _mm512_store_ps(T(1, 2), t12);
    t22 = ADD(ADD(c20, c21), c22);
    _mm512_store_ps(T(2, 2), t22);
    t32 = MUL(z2, c22);
    _mm512_store_ps(T(3, 2), t32);

    _mm512_store_ps(T(0, 3), f02);
    t13 = MUL(r2, ADD(SUB(f02, f12), f22));
    _mm512_store_ps(T(1, 3), t13);
    t23 = MUL(r2, ADD(ADD(f02, f12), f22));
    _mm512_store_ps(T(2, 3), t23);
    _mm512_store_ps(T(3, 3), f22);
  }
}

template <D_WEIGHTS(
    typename Type, const int A, const int K, const int V, const int I)>
void convolution_winograd_kernel<R_WEIGHTS(Type, A, K, V, I)>::__trans_weights(
    winograd_template_parameter_t<S_WEIGHTS(float, 5, 3, 16, ISA_GENERIC)>,
    Type atweights[A][A][V][V], Type aweights[K][K][V][V])
{
  const float r12 = 1.0f / 12.0f;
  const float r6 = 1.0f / 6.0f;
  const float r3 = 1.0f / 3.0f;
  const float r4 = 1.0f / 4.0f;
  const float r2 = 1.0f / 2.0f;
  const float r2_3 = 2.0f / 3.0f;

  float C10[16], C11[16], C12[16], C20[16], C21[16], C22[16], C30[16], C31[16],
      C32[16];
#undef F
#undef T
#define F(h, w) aweights[h][w][_IV][_OV]
#define T(h, w) atweights[h][w][_IV][_OV]
#define C(c, n) C##c##n[_OV]
  for (int _IV = 0; _IV < 16; ++_IV) {
#pragma omp simd
    for (int _OV = 0; _OV < 16; ++_OV) {
      T(0, 0) = r4 * F(0, 0);
      T(1, 0) = -r12 * (F(0, 0) - F(1, 0) + F(2, 0));
      T(2, 0) = -r4 * (F(0, 0) + F(1, 0) + F(2, 0));
      T(3, 0) = r12 * F(0, 0) + r6 * F(1, 0) + r3 * F(2, 0);
      T(4, 0) = r2 * F(2, 0);

      C(1, 0) = -r6 * (F(0, 0) - F(0, 1) + F(0, 2));
      C(1, 1) = -r6 * (F(1, 0) - F(1, 1) + F(1, 2));
      C(1, 2) = -r6 * (F(2, 0) - F(2, 1) + F(2, 2));

      T(0, 1) = r2 * C(1, 0);
      T(1, 1) = -r6 * (C(1, 0) - C(1, 1) + C(1, 2));
      T(2, 1) = -r2 * (C(1, 0) + C(1, 1) + C(1, 2));
      T(3, 1) = r6 * C(1, 0) + r3 * C(1, 1) + r2_3 * C(1, 2);
      T(4, 1) = C(1, 2);

      C(2, 0) = -r2 * (F(0, 0) + F(0, 1) + F(0, 2));
      C(2, 1) = -r2 * (F(1, 0) + F(1, 1) + F(1, 2));
      C(2, 2) = -r2 * (F(2, 0) + F(2, 1) + F(2, 2));

      T(0, 2) = r2 * C(2, 0);
      T(1, 2) = -r6 * (C(2, 0) - C(2, 1) + C(2, 2));
      T(2, 2) = -r2 * (C(2, 0) + C(2, 1) + C(2, 2));
      T(3, 2) = r6 * C(2, 0) + r3 * C(2, 1) + r2_3 * C(2, 2);
      T(4, 2) = C(2, 2);

      C(3, 0) = r6 * F(0, 0) + r3 * F(0, 1) + r2_3 * F(0, 2);
      C(3, 1) = r6 * F(1, 0) + r3 * F(1, 1) + r2_3 * F(1, 2);
      C(3, 2) = r6 * F(2, 0) + r3 * F(2, 1) + r2_3 * F(2, 2);

      T(0, 3) = r2 * C(3, 0);
      T(1, 3) = -r6 * (C(3, 0) - C(3, 1) + C(3, 2));
      T(2, 3) = -r2 * (C(3, 0) + C(3, 1) + C(3, 2));
      T(3, 3) = r6 * C(3, 0) + r3 * C(3, 1) + r2_3 * C(3, 2);
      T(4, 3) = C(3, 2);

      T(0, 4) = r2 * F(0, 2);
      T(1, 4) = -r6 * (F(0, 2) - F(1, 2) + F(2, 2));
      T(2, 4) = -r2 * (F(0, 2) + F(1, 2) + F(2, 2));
      T(3, 4) = r6 * F(0, 2) + r3 * F(1, 2) + r2_3 * F(2, 2);
      T(4, 4) = F(2, 2);
    }
  }
}

template <D_WEIGHTS(
    typename Type, const int A, const int K, const int V, const int I)>
void convolution_winograd_kernel<R_WEIGHTS(Type, A, K, V, I)>::__trans_weights(
    winograd_template_parameter_t<S_WEIGHTS(float, 5, 3, 16, ISA_SKX_AVX512)>,
    Type atweights[A][A][V][V], Type aweights[K][K][V][V])
{
  ENABLE_AVX512F();

  // Constants
  __m512 r12 = _mm512_set_ps(IMM_BCAST16(1.0f / 12.0f));
  __m512 r6 = _mm512_set_ps(IMM_BCAST16(1.0f / 6.0f));
  __m512 r4 = _mm512_set_ps(IMM_BCAST16(1.0f / 4.0f));
  __m512 r_4 = _mm512_set_ps(IMM_BCAST16(-1.0f / 4.0f));
  __m512 r3 = _mm512_set_ps(IMM_BCAST16(1.0f / 3.0f));
  __m512 r2 = _mm512_set_ps(IMM_BCAST16(1.0f / 2.0f));
  __m512 r_2 = _mm512_set_ps(IMM_BCAST16(-1.0f / 2.0f));
  __m512 r2_3 = _mm512_set_ps(IMM_BCAST16(2.0f / 3.0f));

  // Inputs
  __m512 f00, f10, f20, f01, f11, f21, f02, f12, f22;
  // Cache
  __m512 c10, c11, c12, c20, c21, c22, c30, c31, c32;
  // Outputs
  __m512 t00, t10, t20, t30, t40, t01, t11, t21, t31 /*, t41 */, t02, t12, t22,
      t32 /*, t42 */, t03, t13, t23, t33 /*, t43 */, t04, t14, t24,
      t34 /*, t44 */;
#undef F
#undef T
#define F(h, w) aweights[h][w][_V]
#define T(h, w) atweights[h][w][_V]
  for (int _V = 0; _V < 16; ++_V) {
    f00 = _mm512_load_ps(F(0, 0));
    f01 = _mm512_load_ps(F(0, 1));
    f02 = _mm512_load_ps(F(0, 2));
    f10 = _mm512_load_ps(F(1, 0));
    f11 = _mm512_load_ps(F(1, 1));
    f12 = _mm512_load_ps(F(1, 2));
    f20 = _mm512_load_ps(F(2, 0));
    f21 = _mm512_load_ps(F(2, 1));
    f22 = _mm512_load_ps(F(2, 2));

    c10 = MUL(r6, SUB(f01, ADD(f00, f02)));
    c11 = MUL(r6, SUB(f11, ADD(f10, f12)));
    c12 = MUL(r6, SUB(f21, ADD(f20, f22)));
    c20 = MUL(r_2, ADD(ADD(f00, f01), f02));
    c21 = MUL(r_2, ADD(ADD(f10, f11), f12));
    c22 = MUL(r_2, ADD(ADD(f20, f21), f22));

    t00 = MUL(r4, f00);
    _mm512_store_ps(T(0, 0), t00);
    t10 = MUL(r12, SUB(f10, ADD(f00, f20)));
    _mm512_store_ps(T(1, 0), t10);
    t20 = MUL(r_4, ADD(f10, ADD(f00, f20)));
    _mm512_store_ps(T(2, 0), t20);
    t30 = FMADD(r12, f00, FMADD(r6, f10, MUL(r3, f20)));
    _mm512_store_ps(T(3, 0), t30);
    t40 = MUL(r2, f20);
    _mm512_store_ps(T(4, 0), t40);

    t01 = MUL(r2, c10);
    _mm512_store_ps(T(0, 1), t01);
    t11 = MUL(r6, SUB(c11, ADD(c10, c12)));
    _mm512_store_ps(T(1, 1), t11);
    t21 = MUL(r_2, ADD(c11, ADD(c10, c12)));
    _mm512_store_ps(T(2, 1), t21);
    t31 = FMADD(r6, c10, FMADD(r3, c11, MUL(r2_3, c12)));
    _mm512_store_ps(T(3, 1), t31);
    _mm512_store_ps(T(4, 1), c12);

    t02 = MUL(r2, c20);
    _mm512_store_ps(T(0, 2), t02);
    t12 = MUL(r6, SUB(c21, ADD(c20, c22)));
    _mm512_store_ps(T(1, 2), t12);
    t22 = MUL(r_2, ADD(c21, ADD(c20, c22)));
    _mm512_store_ps(T(2, 2), t22);
    t32 = FMADD(r6, c20, FMADD(r3, c21, MUL(r2_3, c22)));
    _mm512_store_ps(T(3, 2), t32);
    _mm512_store_ps(T(4, 2), c22);

    c30 = FMADD(r6, f00, FMADD(r3, f01, MUL(r2_3, f02)));
    c31 = FMADD(r6, f10, FMADD(r3, f11, MUL(r2_3, f12)));
    c32 = FMADD(r6, f20, FMADD(r3, f21, MUL(r2_3, f22)));

    t03 = MUL(r2, c30);
    _mm512_store_ps(T(0, 3), t03);
    t13 = MUL(r6, SUB(c31, ADD(c30, c32)));
    _mm512_store_ps(T(1, 3), t13);
    t23 = MUL(r_2, ADD(c31, ADD(c30, c32)));
    _mm512_store_ps(T(2, 3), t23);
    t33 = FMADD(r6, c30, FMADD(r3, c31, MUL(r2_3, c32)));
    _mm512_store_ps(T(3, 3), t33);
    _mm512_store_ps(T(4, 3), c32);

    t04 = MUL(r2, f02);
    _mm512_store_ps(T(0, 4), t04);
    t14 = MUL(r6, SUB(f12, ADD(f02, f22)));
    _mm512_store_ps(T(1, 4), t14);
    t24 = MUL(r_2, ADD(f12, ADD(f02, f22)));
    _mm512_store_ps(T(2, 4), t24);
    t34 = FMADD(r6, f02, FMADD(r3, f12, MUL(r2_3, f22)));
    _mm512_store_ps(T(3, 4), t34);
    _mm512_store_ps(T(4, 4), f22);
  }
}

template void convolution_winograd_kernel<S_WEIGHTS(float, 4, 3, 16,
    ISA_GENERIC)>::trans_weights(float[4][4][16][16], float[3][3][16][16]);

template void convolution_winograd_kernel<S_WEIGHTS(float, 4, 3, 16,
    ISA_SKX_AVX512)>::trans_weights(float[4][4][16][16], float[3][3][16][16]);

template void convolution_winograd_kernel<S_WEIGHTS(float, 5, 3, 16,
    ISA_GENERIC)>::trans_weights(float[5][5][16][16], float[3][3][16][16]);

template void convolution_winograd_kernel<S_WEIGHTS(float, 5, 3, 16,
    ISA_SKX_AVX512)>::trans_weights(float[5][5][16][16], float[3][3][16][16]);

} // namespace euler
