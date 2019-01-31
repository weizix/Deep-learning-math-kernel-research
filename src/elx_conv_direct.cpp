#include "el_intrin.hpp"
#include "el_utils.hpp"
#include "elx_conv_direct.hpp"
#include "el_def.hpp"
#include "el_utils.hpp"
#include "elx_conv.hpp"
#include "euler.hpp"

namespace euler {

Template_elx_conv_direct_t
Instance_elx_conv_direct_t::elx_conv_direct_t(eld_conv_t &dc)
    : elx_conv_t(dc)
{
  // user input
  xopt_ = this->execution_mode;

  this->Vx = 1;
  this->IC = ALIGNUP(this->ic, V);
  this->OC = ALIGNUP(this->oc, V);

  if (this->I2 == 0) this->I2 = this->ic2;
  if (this->T == 0)  this->T = 1;
  if (this->O == 0)  this->O = 1;
  if (this->O1 == 0) this->O1 = 1;
  this->O2 = this->O * this->O1;

  this->oc4 = this->oc4 == 0 ? 1 : this->oc4;
  this->ic4 = this->ic4 == 0 ? 1 : this->ic4;

  this->ic2 = this->IC / V;
  this->oc2 = this->OC / V;

  // t3, t2, (T, Tr)
  if (xopt_ == 0xa060 || xopt_ == 0xd060) {
    this->t3 = this->n;
    this->ht = this->oh;
    this->wt = (this->ow + this->T - 1)/ this->T;
    this->Tr = this->ow % this->T ? this->ow % this->T : this->T;
    this->nt = this->oh * this->ow;
    this->t2 = this->nt / this->T;
    this->t = this->nt * this->n;

    if (this->T <= this->lp || this->Tr <= this->rp) {
      el_error("Unimplemented T: (T,Tr) must greater than (lp,rp)");
    }
    bool format_ok =
        estl::any_of(this->weights_fmt, hwio, OIhw16i16o) &&
        (((this->input_fmt == nhwc) && (this->output_fmt == nhwc)) ||
         (V == 16 && xopt_ == 0xa060 &&
          (estl::any_of(this->input_fmt, nchw, nChw16c)) &&
          (this->output_fmt == nChw16c)) ||
         (V == 16 && xopt_ == 0xd060 && (this->input_fmt == nChw16c) &&
          (this->output_fmt == nChw16c)));
    if (!format_ok) {
      el_error("direct: format not supported");
    }

    if (xopt_ == 0xa060) {
      bool shape_ok = estl::any_of(this->kh, 3, 5, 7)
          && estl::any_of(this->kw, 3, 5, 7) && this->ih == this->oh
          && this->iw == this->ow && this->hs == 1 && this->ws == 1
          && this->lp == (this->kw / 2) && (this->tp == this->kh / 2);
      if (!shape_ok) {
        el_error("direct: a060: shape not supported");
      }
    }
  }

  this->Ir = this->ic % V ? this->ic % V : V;
  this->Or = this->oc % V ? this->oc % V : V;
  this->ormask = (1 << this->Or) - 1;

  // oc4, (oc3, oc3r), (O2, O2r)
  this->oc34 = (this->oc2 + this->O2 - 1) / this->O2;
  this->O2r = this->oc2 % this->O2;
  if (this->O2r == 0) this->O2r = this->O2;
  this->oc3 = this->oc4; // FIXME, swap order
  this->oc4 = (this->oc34 + this->oc3 - 1) / this->oc3;
  this->oc3r = this->oc34 % this->oc3;
  if (this->oc3r == 0) this->oc3r = this->oc3;

  if (this->O2r != this->O2 || this->oc3r != this->oc3) {
    el_error("No oc tailing support");
  }

  // ic4, ic3, I3
  this->ic34 = this->ic2 / this->I2;
  this->ic3 = this->ic34 / this->ic4;
  if (this->ic4 * this->ic3 * this->I2 * V != this->IC) {
    el_error("IC blocking error");
  }

  attr_ = 0x0;
  is_first_run_ = true;
  inference_acc_ = false;
  mthr_ = omp_get_max_threads();
  inference_acc_ = this->prop_kind == forward_inference;

  attr_ = this->with_bias ? set_attr(attr_, bias_idx) : attr_;
  attr_ = this->with_ip_sum ? set_attr(attr_, ip_sum_idx) : attr_;

  prepare_execute_opt();
  bind_execute_functions();

  // dbg
  printf("T=%d, Tr=%d, t2=%d, ht=%d, wt=%d, t=%d\n",
      this->T, this->Tr, this->t2, this->ht, this->wt, this->t);
  printf("V=%d, Ir=%d, I2=%d, ic3=%d, ic4=%d, IC=%d\n",
      V, this->Ir, this->I2, this->ic3, this->ic4, this->IC);
  printf("V=%d, Or=%d, O2=%d (O=%d, O1=%d), oc3=%d, oc4=%d, O2r=%d, oc3r=%d, OC=%d\n",
      V, this->Or, this->O2, this->O, this->O1,
      this->oc3, this->oc4, this->O2r, this->oc3r, this->OC);
}

Template_elx_conv_direct_t
int Instance_elx_conv_direct_t::prepare_execute_opt()
{
  if (this->with_ip_sum && this->with_relu && !this->output_fmt != nChw16c) {
    el_error("Unimplemented: fuse sum (plain format) and relu together");
  }

  tweights_ = nullptr;
  switch (xopt_) {
  case 0xa060:
  case 0xd060:
    tweights_size_ = this->kh * this->kw * this->IC * this->OC * sizeof(TweightsType);
    break;
  default:
    el_error("Unknown xopt!");
    return -1;
    break;
  }

#define WEIGHTS_MAX_PRELOAD 4
  if (tweights_size_ > 0)
    tweights_size_ += WEIGHTS_MAX_PRELOAD * V;

  scratch_ = nullptr;
  workspace_ = nullptr;
  size_t workspace_size = tweights_size_;
  // TODO: user provided buffer
  if (workspace_size != 0) {
#if 0 // TODO
    MEMALIGN64(&workspace_, workspace_size);
    tweights_ = (TweightsType *)workspace_;
#else
    tweights_ = (TweightsType *)galloc::acquire(workspace_size);
#endif
  }

  return 0;
}

Template_elx_conv_direct_t
Instance_elx_conv_direct_t::~elx_conv_direct_t()
{
  if (workspace_ != nullptr)
    free(workspace_);
}

// weights (hwio): kh, kw, ic, oc
// weights (blocked): oc2, ic2, kh, kw, V, V
// tweights: ic4, oc4, kh, kw, oc3, _ic3, O1, I2, V, O, V
Template_elx_conv_direct_t
void Instance_elx_conv_direct_t::trans_weights_to_compact(
    TweightsType *tweights, WeightsType *weights)
{
  MD11(TweightsType, atweights, tweights, this->ic4, this->oc4, this->kh,
      this->kw, this->oc3, this->ic3, this->O1, this->I2, V, this->O, V);

  if (this->weights_fmt == OIhw16i16o) {
    MD11(WeightsType, aweights, weights, this->oc4, this->oc3, this->O1, this->O,
        this->ic4, this->ic3, this->I2, this->kh, this->kw, V, V);
#pragma omp parallel num_threads(mthr_) proc_bind(close)
#pragma omp for nowait collapse(6)
    iter_each (_oc4, this->oc4) {
    iter_each (_oc3, this->oc3) {
    iter_each (_O1, this->O1) {
    iter_each (_O, this->O) {
    iter_each (_ic4, this->ic4) {
    iter_each (_ic3, this->ic3) {
    iter_each (_I2, this->I2) {
    iter_each (_kh, this->kh) {
    iter_each (_kw, this->kw) {
    iter_each (_iV, V) {
      if (I == ISA_SKX_AVX512 && std::is_same<WeightsType, float>::value) {
        if (std::is_same<TweightsType, float>::value) {
          _mm<V>::store_ps(
              &md11(atweights, _ic4, _oc4, _kh, _kw, _oc3, _ic3, _O1, _I2, _iV, _O, 0),
              *(__m<V> *)&md11(aweights, _oc4, _oc3, _O1, _O, _ic4, _ic3, _I2, _kh, _kw, _iV, 0));
        } else {
          auto fp16v = _mm<V>::cvtps_ph(
              *(__m<V> *)&md11(aweights, _oc4, _oc3, _O1, _O, _ic4, _ic3, _I2, _kh, _kw, _iV, 0),
              _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
          _mm<V/2>::store_si256(
              (__m256i *)&md11(atweights, _ic4, _oc4, _kh, _kw, _oc3, _ic3, _O1, _I2, _iV, _O, 0),
              fp16v);
        }
      } else {
#pragma omp simd
        iter_each (_oV, V) {
          md11(atweights, _ic4, _oc4, _kh, _kw, _oc3, _ic3, _O1, _I2, _iV, _O, _oV)
            = md11(aweights, _oc4, _oc3, _O1, _O, _ic4, _ic3, _I2, _kh, _kw, _iV, _oV);
        }
      }
    }}}}}}}}}}
  } else if (this->weights_fmt == hwio) {
    MD4(WeightsType, aweights0, weights, this->kh, this->kw, this->ic, this->oc);
#pragma omp parallel num_threads(mthr_) proc_bind(close)
#pragma omp for nowait collapse(4)
    iter_each (_kh, this->kh) {
    iter_each (_kw, this->kw) {
    iter_each (_ic4, this->ic4) {
    iter_each (_ic3, this->ic3) {
    iter_each (_I2, this->I2) {
      auto Ir = _ic4 == this->ic4 - 1 && _ic3 == this->ic3 - 1
           && _I2 == this->I2 - 1 ? this->Ir : V;
      iter_each (_iV, Ir) {
      iter_each (_oc4, this->oc4) {
      iter_each (_oc3, this->oc3) {
      iter_each (_O1, this->O1) {
        // handling ic/oc != 16x
        bool is_Or = this->Or != V && _oc4 == this->oc4 - 1
            && _oc3 == this->oc3 - 1 && _O1 == this->O1 - 1;
        auto O = is_Or ? this->O - 1: this->O;
        auto Or = is_Or ? this->Or : 0;
        MD5(WeightsType, aweights1, &md4(aweights0, _kh, _kw, 0, 0), this->ic4,
            this->ic3, this->I2, V, this->oc);
        MD5(WeightsType, aweights2, &md5(aweights1, _ic4, _ic3, _I2, _iV, 0),
            this->oc4, this->oc3, this->O1, this->O, V);
        iter_each(_O, O) {
          if (I == ISA_SKX_AVX512 && std::is_same<WeightsType, float>::value) {
            if (std::is_same<TweightsType, float>::value) {
              _mm<V>::store_ps(
                  &md11(atweights, _ic4, _oc4, _kh, _kw, _oc3, _ic3, _O1, _I2,
                        _iV, _O, 0),
                  *(__m<V> *)&md5(aweights2, _oc4, _oc3, _O1, _O, 0));
            } else {
              auto fp16v = _mm<V>::cvtps_ph(
                  *(__m<V> *)&md5(aweights2, _oc4, _oc3, _O1, _O, 0),
                  _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
              _mm<V / 2>::store_si256(
                  (__m256i *)&md11(atweights, _ic4, _oc4, _kh, _kw, _oc3, _ic3,
                                   _O1, _I2, _iV, _O, 0), fp16v);
            }
          } else {
#pragma omp simd
            iter_each(_oV, V) {
              md11(atweights, _ic4, _oc4, _kh, _kw, _oc3, _ic3, _O1, _I2, _iV,
                   _O, _oV) = md5(aweights2, _oc4, _oc3, _O1, _O, _oV);
            }
          }
        }
        iter_each(_oV, Or) {
          md11(atweights, _ic4, _oc4, _kh, _kw, _oc3, _ic3, _O1, _I2, _iV,
               this->O - 1, _oV) =
              md5(aweights2, _oc4, _oc3, _O1, this->O - 1, _oV);
        }
      }}}}
    }}}}}
  } else {
    el_error("Unimplemented weights format\n");
  }
}

// kh,kw=odd, lp=rp=standard, ih=oh*hs, iw=ow*ws, hs=ws=1
Template_elx_conv_direct_t void
Instance_elx_conv_direct_t::conv_a060(OutputType *output,
    InputType *input, TweightsType *weights, BiasType *bias, int _ic4, int _oc4,
    int _ht, int _wt)
{
  // input:   ic3*, I2, V, ht*, hs*, wt*, T, ws
  // output:  oc3*, O2, ht*, wt*, T, V
  MD4(TweightsType, aweights, weights, this->kh * this->kw, this->oc3, this->ic3,
      this->O2 * this->I2 * V * V);
  MD2(BiasType, abias, bias, this->oc3, this->O2 * V);

  auto ker_conv = _wt == this->wt - 1 ? ker_conv_Tr_ : ker_conv_;

  int khs = estl::max(0, this->tp - _ht);
  int khe = estl::min(this->kh, this->ih + this->tp - _ht);
  int kws = _wt == 0 ? this->lp : 0;
  int kwe = _wt == this->wt - 1 ? this->kw - this->lp : this->kw;
  assert(this->T > this->lp && this->Tr > this->rp);

  if (this->input_fmt == nhwc) {
    MD2(InputType, ainput, input, this->ic3, this->I2 * V);
    MD2(OutputType, aoutput, output, this->oc3, this->O2 * V);

    iter_each(_oc3, this->oc3) {
    iter_each(_ic3, this->ic3) {
      int attr = (_ic4 == 0 && _ic3 == 0) ? set_attr(attr_, r_output_idx) : attr_;
      if (_ic4 == this->ic4 - 1 && _ic3 == this->ic3 - 1) {
        if (this->Ir != V) attr = set_attr(attr, has_Ir_idx);
        if (this->with_relu) attr = set_attr(attr, relu_idx);
      }
      if (this->Or != V && _oc4 == this->oc4 - 1 && _oc3 == this->oc3 - 1) {
        attr = set_attr(attr, has_Or_idx);
      }
      ker_conv(*this, &md2(aoutput, _oc3, 0),
          &md2(ainput, _ic3, 0), &md4(aweights, 0, _oc3, _ic3, 0),
          &md2(abias, _oc3, 0), _wt, khs, khe, kws, kwe, attr);
    }}
  } else {
    // blocked or nchw
    MD2(InputType, ainput, input, this->ic3, this->I2 * V * this->ih * this->iw);
    MD2(OutputType, aoutput, output, this->oc3, this->O2 * this->ht * this->ow * V);

    iter_each(_oc3, this->oc3) {
    iter_each(_ic3, this->ic3) {
      int attr = (_ic4 == 0 && _ic3 == 0) ? set_attr(attr_, r_output_idx) : attr_;
      if (_ic4 == this->ic4 - 1 && _ic3 == this->ic3 - 1) {
        if (this->Ir != V) attr = set_attr(attr, has_Ir_idx);
        if (this->with_relu) attr = set_attr(attr, relu_idx);
      }
      ker_conv(*this, &md2(aoutput, _oc3, 0),
          &md2(ainput, _ic3, 0), &md4(aweights, 0, _oc3, _ic3, 0),
          &md2(abias, _oc3, 0), _wt, khs, khe, kws, kwe, attr);
    }}
  }
}

// slow path
Template_elx_conv_direct_t
void Instance_elx_conv_direct_t::gemm_d060(OutputType *output, InputType *input,
    TweightsType *weights, BiasType *bias, int _ic4, int _oc4, int _ht, int _wt)
{
  // input:   ic3*, I2, ht*, hs*, wt*, T, ws, V
  // output:  oc3*, O2, ht*, wt*, T, V
  MD5(TweightsType, aweights, weights, this->kh, this->kw, this->oc3, this->ic3, this->O2 * this->I2 * V * V);
  MD3(BiasType, abias, bias, this->oc3, this->O2, V);

  int Tz = _wt == this->wt - 1 ? this->Tr : this->T;
  int ows0 = _wt * this->T;
  int khs = estl::max(0, this->tp - this->hs * _ht);
  int khe = estl::min(this->kh, this->ih + this->tp - this->hs * _ht);
  assert(this->T > this->lp);
  assert(this->Tr > this->rp);

  if (this->input_fmt == nhwc) {
    MD3(InputType, ainput0, input, this->ih, this->iw, this->ic);
    MD3(OutputType, aoutput0, output, this->ht, this->ow, this->oc);

    iter_each (_oc3, this->oc3) {
    iter_each (_ic3, this->ic3) {
      bool oc3_has_Or
          = this->Or != V && _oc4 == this->oc4 - 1 && _oc3 == this->oc3 - 1;
      if (_ic4 == 0 && _ic3 == 0) {
        iter_each (_O2, this->O2) {
          bool O2_has_Or = oc3_has_Or && (_O2 == this->O2 - 1);
          __m<V> s = this->with_bias ? *(__m<V> *)&md3(abias, _oc3, _O2, 0)
                                     : _mm<V>::setzero_ps();
          if (I == ISA_SKX_AVX512 && std::is_same<OutputType, float>::value) {
            __mmask16 k = _mm512_int2mask(O2_has_Or ? this->ormask : 0xFFFF);
            iter_each (_T, Tz) {
              MD4(OutputType, aoutput1, &md3(aoutput0, _ht, ows0 + _T, 0),
                  this->oc4, this->oc3, this->O2, V);
              _mm512_mask_store_ps(&md4(aoutput1, 0, _oc3, _O2, 0), k, s);
            }
          } else el_error("direct: d060: unimplemented");
        }
      }
      int attr = attr_;
      if (_ic4 == this->ic4 - 1 && _ic3 == this->ic3 - 1) {
        if (this->Ir != V) attr = set_attr(attr, has_Ir_idx);
        if (this->with_relu) attr = set_attr(attr, relu_idx);
      }
      if (oc3_has_Or) {
        attr = set_attr(attr, has_Or_idx);
      }
      for (int _kh = khs; _kh < khe; ++_kh) {
        auto _ih = this->hs * _ht + _kh - this->tp;
        for (int _kw = 0; _kw < this->kw; ++_kw) {
          auto _iws = this->ws * ows0 + _kw - this->lp;
          while (_iws < 0) _iws += this->ws;
          auto _ows = (_iws + this->lp - _kw) / this->ws;

          MD4(InputType, ainput1, &md3(ainput0, _ih, _iws, 0), this->ic4,
              this->ic3, this->I2, V);
          MD4(OutputType, aoutput1, &md3(aoutput0, _ht, _ows, 0), this->oc4,
              this->oc3, this->O2, V);
          ker_gemm_[_wt][_kw](
              *this, &md4(aoutput1, 0, _oc3, 0, 0), &md4(ainput1, 0, _ic3, 0, 0),
              &md5(aweights, _kh, _kw, _oc3, _ic3, 0), &md3(abias, _oc3, 0, 0),
              attr, 0, nullptr, nullptr, nullptr);
        }
      }
    }}
  } else {
    MD5(InputType, ainput, input, this->ic3, this->I2, this->ih, this->iw, V);
    MD5(OutputType, aoutput, output, this->oc3, this->O2, this->ht, this->ow, V);

    iter_each (_oc3, this->oc3) {
    iter_each (_ic3, this->ic3) {
      if (_ic4 == 0 && _ic3 == 0) {
        iter_each (_O2, this->O2) {
          __m<V> s = this->with_bias ? *(__m<V> *)&md3(abias, _oc3, _O2, 0)
                                     : _mm<V>::setzero_ps();
          iter_each (_T, Tz) {
            if (I == ISA_SKX_AVX512 && std::is_same<OutputType, float>::value)
              _mm<V>::store_ps(&md5(aoutput, _oc3, _O2, _ht, ows0 + _T, 0), s);
            else
              el_error("direct: d060: unimplemented");
          }
        }
      }
      int attr = attr_;
      if (_ic4 == this->ic4 - 1 && _ic3 == this->ic3 - 1) {
        if (this->Ir != V) attr = set_attr(attr, has_Ir_idx);
        if (this->with_relu) attr = set_attr(attr, relu_idx);
      }
      for (int _kh = khs; _kh < khe; ++_kh) {
        auto _ih = this->hs * _ht + _kh - this->tp;
        for (int _kw = 0; _kw < this->kw; ++_kw) {
          auto _iws = this->ws * ows0 + _kw - this->lp;
          while (_iws < 0) _iws += this->ws;
          auto _ows = (_iws + this->lp - _kw) / this->ws;
          ker_gemm_[_wt][_kw](*this, &md5(aoutput, _oc3, 0, _ht, _ows, 0),
              &md5(ainput, _ic3, 0, _ih, _iws, 0),
              &md5(aweights, _kh, _kw, _oc3, _ic3, 0), &md3(abias, _oc3, 0, 0),
              attr, 0, nullptr, nullptr, nullptr);
        }
      }
    }}
  }
}

} // namespace euler
