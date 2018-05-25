#include <type_traits>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include "elt_utils.hpp"
#include "euler.hpp"

#ifndef __ELT_CONV_UTILS_HPP__
#define __ELT_CONV_UTILS_HPP__

namespace euler {
namespace test {

  template <typename Type>
  void prepare_conv_data(eld_conv_t<Type> &desc, Type **input, Type **weights,
      Type **output, Type **bias);

  void teardown_conv_data(
      void *input, void *weights, void *output, void *bias);

  template <typename Type>
  int compare_conv_results_block16(eld_conv_t<Type> &, Type *out, Type *ref);

  size_t cal_ops(eld_conv_t<float> &desc);

  template <typename Type, const int dst_fmt, const int src_fmt,
      typename... Args>
  struct reorder {
    reorder(Type *dst, Type *src, Args...)
    {
      assert(dst != nullptr && src != nullptr);
    }
  };

  template <typename Type> struct reorder<Type, nchw, nChw16c> {
    reorder(Type *dst, Type *src, int n, int c, int h, int w);
  };

  template <typename Type> struct reorder<Type, nChw16c, nchw> {
    reorder(Type *dst, Type *src, int n, int c, int h, int w);
  };

  template <typename Type> struct reorder<Type, oihw, OIhw16i16o> {
    reorder(Type *dst, Type *src, int o, int i, int h, int w);
  };

  template <typename Type> struct reorder<Type, OIhw16i16o, oihw> {
    reorder(Type *dst, Type *src, int o, int i, int h, int w);
  };

  template <typename Type>
  int ref_convolution2d(eld_conv_t<Type> &desc, Type *output, Type *input,
      Type *weights, Type *bias);

  template <typename Type>
  int ref_convolution2d_block16(eld_conv_t<Type> &desc, Type *output,
      Type *input, Type *weights, Type *bias);
}
}

#endif // __ELT_CONV_UTILS_HPP__
