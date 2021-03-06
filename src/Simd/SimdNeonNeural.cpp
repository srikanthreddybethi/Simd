/*
* Simd Library (http://ermig1979.github.io/Simd).
*
* Copyright (c) 2011-2017 Yermalayeu Ihar.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy 
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell 
* copies of the Software, and to permit persons to whom the Software is 
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in 
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#include "Simd/SimdMemory.h"
#include "Simd/SimdStore.h"
#include "Simd/SimdExtract.h"

namespace Simd
{
#ifdef SIMD_NEON_ENABLE    
    namespace Neon
    {
        template <bool inversion> uint8x16_t Invert(const uint8x16_t & value);

        template <> uint8x16_t Invert<true>(const uint8x16_t & value)
        {
            return vsubq_u8(K8_FF, value);
        }

        template <> uint8x16_t Invert<false>(const uint8x16_t & value)
        {
            return value;
        }

        template <bool align> void Convert(const uint16x8_t & src, const float32x4_t &_1_255, float * dst)
        {
            Store<align>(dst + 0, vmulq_f32(ToFloat<0>(src), _1_255));
            Store<align>(dst + F, vmulq_f32(ToFloat<1>(src), _1_255));
        }

        template <bool inversion, bool align> void Convert(const uint8_t * src, const float32x4_t &_1_255, float * dst)
        {
            uint8x16_t _src = Invert<inversion>(Load<align>(src));
            Convert<align>(UnpackU8<0>(_src), _1_255, dst + 0);
            Convert<align>(UnpackU8<1>(_src), _1_255, dst + DF);
        }

        template <bool inversion, bool align> void NeuralConvert(const uint8_t * src, size_t srcStride, size_t width, size_t height, float * dst, size_t dstStride)
        {
            assert(width >= A);
            if (align)
                assert(Aligned(src) && Aligned(srcStride) && Aligned(dst) && Aligned(dstStride));

            size_t alignedWidth = AlignLo(width, A);
            float32x4_t _1_255 = vdupq_n_f32(1.0f / 255.0f);

            for (size_t row = 0; row < height; ++row)
            {
                for (size_t col = 0; col < alignedWidth; col += A)
                    Convert<inversion, align>(src + col, _1_255, dst + col);
                if (width != alignedWidth)
                    Convert<inversion, false>(src + width - A, _1_255, dst + width - A);
                src += srcStride;
                dst += dstStride;
            }
        }

        template <bool inversion> void NeuralConvert(const uint8_t * src, size_t srcStride, size_t width, size_t height, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride) && Aligned(dst) && Aligned(dstStride))
                NeuralConvert<inversion, true>(src, srcStride, width, height, dst, dstStride);
            else
                NeuralConvert<inversion, false>(src, srcStride, width, height, dst, dstStride);
        }

        void NeuralConvert(const uint8_t * src, size_t srcStride, size_t width, size_t height, float * dst, size_t dstStride, int inversion)
        {
            if (inversion)
                NeuralConvert<true>(src, srcStride, width, height, dst, dstStride);
            else
                NeuralConvert<false>(src, srcStride, width, height, dst, dstStride);
        }

        template <bool align> SIMD_INLINE void NeuralProductSum(const float * a, const float * b, size_t offset, float32x4_t & sum)
        {
            float32x4_t _a = Load<align>(a + offset);
            float32x4_t _b = Load<align>(b + offset);
            sum = vmlaq_f32(sum, _a, _b);
        }

        template <bool align> SIMD_INLINE void NeuralProductSum(const float * a, const float * b, size_t size, float * sum)
        {
            if(align)
                assert(Aligned(a) && Aligned(b));

            *sum = 0;
            size_t partialAlignedSize = AlignLo(size, F);
            size_t fullAlignedSize = AlignLo(size, DF);
            size_t i = 0;
            if(partialAlignedSize)
            {
                float32x4_t sums[2] = {vdupq_n_f32(0), vdupq_n_f32(0)};
                if (fullAlignedSize)
                {
                    for (; i < fullAlignedSize; i += DF)
                    {
                        NeuralProductSum<align>(a, b, i + 0, sums[0]);
                        NeuralProductSum<align>(a, b, i + F, sums[1]);
                    }
                    sums[0] = vaddq_f32(sums[0], sums[1]);
                }
                for(; i < partialAlignedSize; i += F)
                    NeuralProductSum<align>(a, b, i, sums[0]);
                *sum += ExtractSum32f(sums[0]);
            }
            for(; i < size; ++i)
                *sum += a[i]*b[i];
        }

        void NeuralProductSum(const float * a, const float * b, size_t size, float * sum)
        {
            if(Aligned(a) && Aligned(b))
                NeuralProductSum<true>(a, b, size, sum);
            else
                NeuralProductSum<false>(a, b, size, sum);
        }

        template <bool align> SIMD_INLINE void AddMultiplied(const float * src, const float32x4_t & value, float * dst)
        {
            Store<align>(dst, vmlaq_f32(Load<align>(dst), value, Load<align>(src)));
        }

        template <bool align> SIMD_INLINE void AddMultiplied(const float * src, size_t aligned, size_t partial, size_t full, float value, float * dst)
        {
            if (align)
                assert(Aligned(src) && Aligned(dst) && Aligned(aligned, QF) && Aligned(partial, F));
            size_t i = 0;
            if (partial)
            {
                float32x4_t _value = vdupq_n_f32(value);
                for (; i < aligned; i += QF)
                {
                    AddMultiplied<align>(src + i + F * 0, _value, dst + i + F * 0);
                    AddMultiplied<align>(src + i + F * 1, _value, dst + i + F * 1);
                    AddMultiplied<align>(src + i + F * 2, _value, dst + i + F * 2);
                    AddMultiplied<align>(src + i + F * 3, _value, dst + i + F * 3);
                }
                for (; i < partial; i += F)
                    AddMultiplied<align>(src + i, _value, dst + i);
            }
            for (; i < full; ++i)
                dst[i] += src[i] * value;
        }

        void NeuralAddVectorMultipliedByValue(const float * src, size_t size, const float * value, float * dst)
        {
            size_t aligned = AlignLo(size, QF);
            size_t partial = AlignLo(size, F);
            if (Aligned(src) && Aligned(dst))
                AddMultiplied<true>(src, aligned, partial, size, *value, dst);
            else
                AddMultiplied<false>(src, aligned, partial, size, *value, dst);
        }

        template <bool align> SIMD_INLINE void AddVector(const float * src, float * dst)
        {
            Store<align>(dst, vaddq_f32(Load<align>(dst), Load<align>(src)));
        }

        template <bool align> SIMD_INLINE void AddVector(const float * src, size_t aligned, size_t partial, size_t full, float * dst)
        {
            size_t i = 0;
            for (; i < aligned; i += QF)
            {
                AddVector<align>(src + i + F * 0, dst + i + F * 0);
                AddVector<align>(src + i + F * 1, dst + i + F * 1);
                AddVector<align>(src + i + F * 2, dst + i + F * 2);
                AddVector<align>(src + i + F * 3, dst + i + F * 3);
            }
            for (; i < partial; i += F)
                AddVector<align>(src + i, dst + i);
            for (; i < full; ++i)
                dst[i] += src[i];
        }

        void NeuralAddVector(const float * src, size_t size, float * dst)
        {
            size_t aligned = AlignLo(size, QF);
            size_t partial = AlignLo(size, F);
            if (Aligned(src) && Aligned(dst))
                AddVector<true>(src, aligned, partial, size, dst);
            else
                AddVector<false>(src, aligned, partial, size, dst);
        }

        template <bool align> SIMD_INLINE void AddValue(const float32x4_t & value, float * dst)
        {
            Store<align>(dst, vaddq_f32(Load<align>(dst), value));
        }

        template <bool align> SIMD_INLINE void AddValue(const float * value, float * dst, size_t aligned, size_t partial, size_t full)
        {
            size_t i = 0;
            if (partial)
            {
                float32x4_t _value = vdupq_n_f32(value[0]);
                for (; i < aligned; i += QF)
                {
                    AddValue<align>(_value, dst + i + F * 0);
                    AddValue<align>(_value, dst + i + F * 1);
                    AddValue<align>(_value, dst + i + F * 2);
                    AddValue<align>(_value, dst + i + F * 3);
                }
                for (; i < partial; i += F)
                    AddValue<align>(_value, dst + i);
            }
            for (; i < full; ++i)
                dst[i] += value[0];
        }

        void NeuralAddValue(const float * value, float * dst, size_t size)
        {
            size_t aligned = AlignLo(size, QF);
            size_t partial = AlignLo(size, F);
            if (Aligned(dst))
                AddValue<true>(value, dst, aligned, partial, size);
            else
                AddValue<false>(value, dst, aligned, partial, size);
        }

        template <bool align> SIMD_INLINE void NeuralRoughSigmoid(const float * src, size_t size, const float * slope, float * dst)
        {
            if (align)
                assert(Aligned(src) && Aligned(dst));
            size_t alignedSize = Simd::AlignLo(size, F);
            float32x4_t _slope = vdupq_n_f32(*slope);
            float32x4_t _0 = vdupq_n_f32(-0.0f);
            float32x4_t _1 = vdupq_n_f32(1.0f);
            float32x4_t _a = vdupq_n_f32(0.5417f);
            float32x4_t _b = vdupq_n_f32(0.1460f);
            size_t i = 0;
            for (; i < alignedSize; i += F)
            {
                float32x4_t _src = Load<align>(src + i);
                float32x4_t x = vabsq_f32(vmulq_f32(_src, _slope));
                float32x4_t x2 = vmulq_f32(x, x);
                float32x4_t x4 = vmulq_f32(x2, x2);
                float32x4_t series = vaddq_f32(vmlaq_f32(_1, x2, _a), vmlaq_f32(x, x4, _b));
                uint32x4_t mask = vcgtq_f32(_src, _0);
                float32x4_t exp = vbslq_f32(mask, Reciprocal<1>(series), series);
                float32x4_t sigmoid = Reciprocal<1>(vaddq_f32(_1, exp));
                Store<align>(dst + i, sigmoid);
            }
            for (; i < size; ++i)
                dst[i] = Base::RoughSigmoid(src[i] * slope[0]);
        }

        void NeuralRoughSigmoid(const float * src, size_t size, const float * slope, float * dst)
        {
            if (Aligned(src) && Aligned(dst))
                NeuralRoughSigmoid<true>(src, size, slope, dst);
            else
                NeuralRoughSigmoid<false>(src, size, slope, dst);
        }

        template <bool align> SIMD_INLINE void NeuralRoughSigmoid2(const float * src, const float32x4_t & k, const float32x4_t & o, const float32x4_t & m, float * dst)
        {
            float32x4_t _src = Load<align>(src);
            float32x4_t e1 = vmaxq_f32(m, vmlsq_f32(o, _src, k));
            float32x4_t e2 = vmulq_f32(e1, e1);
            float32x4_t e4 = vmulq_f32(e2, e2);
            float32x4_t e8 = vmulq_f32(e4, e4);
            float32x4_t e16 = vmulq_f32(e8, e8);
            float32x4_t e32 = vmulq_f32(e16, e16);
            float32x4_t e64 = vmulq_f32(e32, e32);
            float32x4_t sigmoid = Reciprocal<1>(vmlaq_f32(o, e64, e64));
            Store<align>(dst, sigmoid);
        }

        template <bool align> SIMD_INLINE void NeuralRoughSigmoid2(const float * src, size_t size, const float * slope, float * dst)
        {
            if (align)
                assert(Aligned(src) && Aligned(dst));
            size_t partialAlignedSize = Simd::AlignLo(size, F);
            size_t fullAlignedSize = Simd::AlignLo(size, QF);
            float32x4_t _k = vdupq_n_f32((*slope)*0.0078125f);
            float32x4_t _1 = vdupq_n_f32(1.0f);
            float32x4_t _05 = vdupq_n_f32(0.5f);
            size_t i = 0;
            for (; i < fullAlignedSize; i += QF)
            {
                NeuralRoughSigmoid2<align>(src + i + 0 * F, _k, _1, _05, dst + i + 0 * F);
                NeuralRoughSigmoid2<align>(src + i + 1 * F, _k, _1, _05, dst + i + 1 * F);
                NeuralRoughSigmoid2<align>(src + i + 2 * F, _k, _1, _05, dst + i + 2 * F);
                NeuralRoughSigmoid2<align>(src + i + 3 * F, _k, _1, _05, dst + i + 3 * F);
            }
            for (; i < partialAlignedSize; i += F)
                NeuralRoughSigmoid2<align>(src + i, _k, _1, _05, dst + i);
            for (; i < size; ++i)
                dst[i] = Base::RoughSigmoid2(src[i] * slope[0]);
        }

        void NeuralRoughSigmoid2(const float * src, size_t size, const float * slope, float * dst)
        {
            if (Aligned(src) && Aligned(dst))
                NeuralRoughSigmoid2<true>(src, size, slope, dst);
            else
                NeuralRoughSigmoid2<false>(src, size, slope, dst);
        }

        template <bool align> SIMD_INLINE void NeuralDerivativeSigmoid(const float * src, size_t size, const float * slope, float * dst)
        {
            if (align)
                assert(Aligned(src) && Aligned(dst));
            size_t alignedSize = Simd::AlignLo(size, F);
            float32x4_t _slope = vdupq_n_f32(*slope);
            float32x4_t _1 = vdupq_n_f32(1.0f);
            size_t i = 0;
            for (; i < alignedSize; i += F)
            {
                float32x4_t _src = Load<align>(src + i);
                float32x4_t _dst = Load<align>(dst + i);
                Store<align>(dst + i, vmulq_f32(vmulq_f32(_dst, _slope), vmulq_f32(vsubq_f32(_1, _src), _src)));
            }
            for (; i < size; ++i)
                dst[i] *= slope[0] * Base::DerivativeSigmoid(src[i]);
        }

        void NeuralDerivativeSigmoid(const float * src, size_t size, const float * slope, float * dst)
        {
            if (Aligned(src) && Aligned(dst))
                NeuralDerivativeSigmoid<true>(src, size, slope, dst);
            else
                NeuralDerivativeSigmoid<false>(src, size, slope, dst);
        }

        template <bool align> SIMD_INLINE void NeuralRoughTanh(const float * src, size_t size, const float * slope, float * dst)
        {
            if (align)
                assert(Aligned(src) && Aligned(dst));
            size_t alignedSize = Simd::AlignLo(size, F);
            float32x4_t _slope = vdupq_n_f32(*slope);
            float32x4_t _0 = vdupq_n_f32(-0.0f);
            float32x4_t _1 = vdupq_n_f32(1.0f);
            float32x4_t _a = vdupq_n_f32(0.5658f);
            float32x4_t _b = vdupq_n_f32(0.1430f);
            size_t i = 0;
            for (; i < alignedSize; i += F)
            {
                float32x4_t _src = Load<align>(src + i);
                float32x4_t x = vabsq_f32(vmulq_f32(_src, _slope));
                float32x4_t x2 = vmulq_f32(x, x);
                float32x4_t x4 = vmulq_f32(x2, x2);
                float32x4_t pe = vaddq_f32(vmlaq_f32(_1, x2, _a), vmlaq_f32(x, x4, _b));
                float32x4_t ne = Reciprocal<1>(pe);
                float32x4_t absTanh = vmulq_f32(vsubq_f32(pe, ne), Reciprocal<1>(vaddq_f32(pe, ne)));
                float32x4_t tanh = (float32x4_t)veorq_u32((uint32x4_t)absTanh, vandq_u32((uint32x4_t)_0, vcgtq_f32(_0, _src)));
                Store<align>(dst + i, tanh);
            }
            for (; i < size; ++i)
                dst[i] = Base::RoughTanh(src[i] * slope[0]);
        }

        void NeuralRoughTanh(const float * src, size_t size, const float * slope, float * dst)
        {
            if (Aligned(src) && Aligned(dst))
                NeuralRoughTanh<true>(src, size, slope, dst);
            else
                NeuralRoughTanh<false>(src, size, slope, dst);
        }

        template <bool align> SIMD_INLINE void NeuralDerivativeTanh(const float * src, size_t size, const float * slope, float * dst)
        {
            if (align)
                assert(Aligned(src) && Aligned(dst));
            size_t alignedSize = Simd::AlignLo(size, F);
            float32x4_t _slope = vdupq_n_f32(*slope);
            float32x4_t _1 = vdupq_n_f32(1.0f);
            size_t i = 0;
            for (; i < alignedSize; i += F)
            {
                float32x4_t _src = Load<align>(src + i);
                float32x4_t _dst = Load<align>(dst + i);
                Store<align>(dst + i, vmulq_f32(vmulq_f32(_dst, _slope), vsubq_f32(_1, vmulq_f32(_src, _src))));
            }
            for (; i < size; ++i)
                dst[i] *= slope[0] * Base::DerivativeTanh(src[i]);
        }

        void NeuralDerivativeTanh(const float * src, size_t size, const float * slope, float * dst)
        {
            if (Aligned(src) && Aligned(dst))
                NeuralDerivativeTanh<true>(src, size, slope, dst);
            else
                NeuralDerivativeTanh<false>(src, size, slope, dst);
        }

        template <bool align> SIMD_INLINE void NeuralRelu(const float * src, size_t size, const float * slope, float * dst)
        {
            if (align)
                assert(Aligned(src) && Aligned(dst));
            float s = slope[0];
            assert(s >= 0.0f && s <= 1.0f);
            size_t alignedSize = Simd::AlignLo(size, F);
            size_t i = 0;
            if (s == 0)
            {
                float32x4_t _0 = vdupq_n_f32(0.0f);
                for (; i < alignedSize; i += F)
                {
                    float32x4_t _src = Load<align>(src + i);
                    float32x4_t relu = vmaxq_f32(_0, _src);
                    Store<align>(dst + i, relu);
                }
                for (; i < size; ++i)
                    dst[i] = Simd::Max(0.0f, src[i]);
            }
            else
            {
                float32x4_t _s = vdupq_n_f32(s);
                for (; i < alignedSize; i += F)
                {
                    float32x4_t _src = Load<align>(src + i);
                    float32x4_t relu = vmaxq_f32(vmulq_f32(_src, _s), _src);
                    Store<align>(dst + i, relu);
                }
                for (; i < size; ++i)
                    dst[i] = Simd::Max(src[i] * s, src[i]);
            }
        }

        void NeuralRelu(const float * src, size_t size, const float * slope, float * dst)
        {
            if (Aligned(src) && Aligned(dst))
                NeuralRelu<true>(src, size, slope, dst);
            else
                NeuralRelu<false>(src, size, slope, dst);
        }

        template <bool align> void NeuralDerivativeRelu(const float * src, size_t size, const float * slope, float * dst)
        {
            if (align)
                assert(Aligned(src) && Aligned(dst));
            float s = slope[0];
            float32x4_t _0 = vdupq_n_f32(0.0f);
            float32x4_t _1 = vdupq_n_f32(1.0f);
            float32x4_t _s = vdupq_n_f32(s);
            size_t alignedSize = Simd::AlignLo(size, F);
            size_t i = 0;
            for (; i < alignedSize; i += F)
            {
                uint32x4_t mask = vcgtq_f32(Load<align>(src + i), _0);
                float32x4_t _dst = Load<align>(dst + i);
                Store<align>(dst + i, vmulq_f32(vbslq_f32(mask, _1, _s), _dst));
            }
            for (; i < size; ++i)
                dst[i] *= src[i] > 0 ? 1.0f : s;
        }

        void NeuralDerivativeRelu(const float * src, size_t size, const float * slope, float * dst)
        {
            if (Aligned(src) && Aligned(dst))
                NeuralDerivativeRelu<true>(src, size, slope, dst);
            else
                NeuralDerivativeRelu<false>(src, size, slope, dst);
        }

        template <bool align> SIMD_INLINE void UpdateWeights(const float * x, const float32x4_t & a, const float32x4_t & b, float * d, float * w)
        {
            float32x4_t _d = vaddq_f32(vmulq_f32(a, Load<align>(d)), vmulq_f32(b, Load<align>(x)));
            Store<align>(d, _d);
            Store<align>(w, vaddq_f32(Load<align>(w), _d));
        }

        template <bool align> SIMD_INLINE void UpdateWeights(const float * x, size_t offset, const float32x4_t & a, const float32x4_t & b, float * d, float * w)
        {
            UpdateWeights<align>(x + offset, a, b, d + offset, w + offset);
        }

        template <bool align> void NeuralUpdateWeights(const float * x, size_t size, const float & a, const float & b, float * d, float * w)
        {
            if (align)
                assert(Aligned(x) && Aligned(d) && Aligned(w));

            size_t partialAlignedSize = AlignLo(size, F);
            size_t fullAlignedSize = AlignLo(size, QF);
            float32x4_t _a = vdupq_n_f32(a);
            float32x4_t _b = vdupq_n_f32(b);
            size_t i = 0;
            if (partialAlignedSize)
            {
                if (fullAlignedSize)
                {
                    for (; i < fullAlignedSize; i += QF)
                    {
                        UpdateWeights<align>(x, i + F * 0, _a, _b, d, w);
                        UpdateWeights<align>(x, i + F * 1, _a, _b, d, w);
                        UpdateWeights<align>(x, i + F * 2, _a, _b, d, w);
                        UpdateWeights<align>(x, i + F * 3, _a, _b, d, w);
                    }
                }
                for (; i < partialAlignedSize; i += F)
                    UpdateWeights<align>(x, i, _a, _b, d, w);
            }
            for (; i < size; ++i)
                Base::UpdateWeights(x, i, a, b, d, w);
        }

        void NeuralUpdateWeights(const float * x, size_t size, const float * a, const float * b, float * d, float * w)
        {
            if (Aligned(x) && Aligned(d) && Aligned(w))
                NeuralUpdateWeights<true>(x, size, *a, *b, d, w);
            else
                NeuralUpdateWeights<false>(x, size, *a, *b, d, w);
        }


        template <bool align> SIMD_INLINE void AdaptiveGradientUpdate(const float * delta, const float32x4_t & norm, const float32x4_t & alpha, const float32x4_t & epsilon, float * gradient, float * weight)
        {
            float32x4_t d = vmulq_f32(Load<align>(delta), norm);
            float32x4_t _gradient = vaddq_f32(Load<align>(gradient), vmulq_f32(d, d));
            Store<align>(gradient, _gradient);
            Store<align>(weight, vsubq_f32(Load<align>(weight), vmulq_f32(vmulq_f32(alpha, d), ReciprocalSqrt<1>(vaddq_f32(_gradient, epsilon)))));
        }

        template <bool align> SIMD_INLINE void AdaptiveGradientUpdate(const float * delta, size_t offset, const float32x4_t & norm, const float32x4_t & alpha, const float32x4_t & epsilon, float * gradient, float * weight)
        {
            AdaptiveGradientUpdate<align>(delta + offset, norm, alpha, epsilon, gradient + offset, weight + offset);
        }

        template <bool align> void NeuralAdaptiveGradientUpdate(const float * delta, size_t size, size_t batch, const float * alpha, const float * epsilon, float * gradient, float * weight)
        {
            if (align)
                assert(Aligned(delta) && Aligned(gradient) && Aligned(weight));

            size_t partialAlignedSize = AlignLo(size, F);
            size_t fullAlignedSize = AlignLo(size, QF);
            const float norm = (float)(1.0 / batch);
            float32x4_t _norm = vdupq_n_f32(norm);
            float32x4_t _alpha = vdupq_n_f32(*alpha);
            float32x4_t _epsilon = vdupq_n_f32(*epsilon);
            size_t i = 0;
            if (partialAlignedSize)
            {
                if (fullAlignedSize)
                {
                    for (; i < fullAlignedSize; i += QF)
                    {
                        AdaptiveGradientUpdate<align>(delta, i + F * 0, _norm, _alpha, _epsilon, gradient, weight);
                        AdaptiveGradientUpdate<align>(delta, i + F * 1, _norm, _alpha, _epsilon, gradient, weight);
                        AdaptiveGradientUpdate<align>(delta, i + F * 2, _norm, _alpha, _epsilon, gradient, weight);
                        AdaptiveGradientUpdate<align>(delta, i + F * 3, _norm, _alpha, _epsilon, gradient, weight);
                    }
                }
                for (; i < partialAlignedSize; i += F)
                    AdaptiveGradientUpdate<align>(delta, i, _norm, _alpha, _epsilon, gradient, weight);
            }
            for (; i < size; ++i)
                Base::AdaptiveGradientUpdate(delta, i, norm, *alpha, *epsilon, gradient, weight);
        }

        void NeuralAdaptiveGradientUpdate(const float * delta, size_t size, size_t batch, const float * alpha, const float * epsilon, float * gradient, float * weight)
        {
            if (Aligned(delta) && Aligned(gradient) && Aligned(weight))
                NeuralAdaptiveGradientUpdate<true>(delta, size, batch, alpha, epsilon, gradient, weight);
            else
                NeuralAdaptiveGradientUpdate<false>(delta, size, batch, alpha, epsilon, gradient, weight);
        }

        template <size_t size> SIMD_INLINE void LoadWeightsForward(const float * src, float32x4_t * dst)
        {
            for (size_t i = 0; i < size; ++i)
                dst[i] = vdupq_n_f32(src[i]);
        }

        template <size_t size> SIMD_INLINE void LoadWeightsBackward(const float * src, float32x4_t * dst)
        {
            for (size_t i = 0; i < size; ++i)
                dst[i] = vdupq_n_f32(src[size - i - 1]);
        }

        namespace
        {
            template<int count> struct Buffer
            {
                Buffer(size_t width)
                {
                    _size = width*sizeof(float);
                    size_t stride = AlignHi(width + 2 * (count - 1), F);
                    size_t full = count*stride*sizeof(float);
                    _ptr = Allocate(full);
                    memset(_ptr, 0, full);
                    rows[0] = (float*)_ptr;
                    for (size_t i = 1; i < count; ++i)
                        rows[i] = rows[i - 1] + stride;
                }

                void Update(const float * src)
                {
                    float * tmp = rows[0];
                    if (src == NULL)
                        memset(tmp + count - 1, 0, _size);
                    else
                        memcpy(tmp + count - 1, src, _size);
                    for (size_t i = 0; i < count - 1; ++i)
                        rows[i] = rows[i + 1];
                    rows[count - 1] = tmp;
                }

                ~Buffer()
                {
                    Free(_ptr);
                }

                float * rows[count];
            private:
                size_t _size;
                void * _ptr;
            };
        }

        template<size_t coreX, size_t coreY> struct Convolution
        {
            template<bool align> static SIMD_INLINE  float32x4_t Forward(const float * src, size_t stride, const  float32x4_t * weights);

            template<bool align> static SIMD_INLINE float32x4_t Backward(const Buffer<coreX> & buffer, size_t offset, const float32x4_t * weights);

            template <bool align> static SIMD_INLINE void Sum(const float * src, const float32x4_t & dst, float32x4_t * sums);
        };

        template<> struct Convolution<2, 2>
        {
            template <bool align> static SIMD_INLINE float32x4_t Convolution2(const float * src, const float32x4_t * weights)
            {
                float32x4_t _src[2];
                _src[0] = Load<align>(src + 0);
                _src[1] = vld1q_f32(src + 1);
                return vmlaq_f32(vmulq_f32(_src[0], weights[0]), _src[1], weights[1]);
            }

            template<bool align> static SIMD_INLINE  float32x4_t Forward(const float * src, size_t stride, const  float32x4_t * weights)
            {
                return vaddq_f32(Convolution2<align>(src, weights),
                    Convolution2<align>(src + stride, weights + 2));
            }

            template<bool align> static SIMD_INLINE float32x4_t Backward(const Buffer<2> & buffer, size_t offset, const float32x4_t * weights)
            {
                return vaddq_f32(Convolution2<align>(buffer.rows[0] + offset, weights),
                    Convolution2<align>(buffer.rows[1] + offset, weights + 2));
            }

            template <bool align> static SIMD_INLINE void Sum(const float * src, const float32x4_t & dst, float32x4_t * sums)
            {
                float32x4_t _src[2];
                _src[0] = Load<align>(src);
                _src[1] = vld1q_f32(src + 1);
                sums[0] = vmlaq_f32(sums[0], dst, _src[0]);
                sums[1] = vmlaq_f32(sums[1], dst, _src[1]);
            }

            template <bool align> static SIMD_INLINE void Sum(const float * src, size_t stride, const float32x4_t & dst, float32x4_t * sums)
            {
                Sum<align>(src + stride * 0, dst, sums + 0);
                Sum<align>(src + stride * 1, dst, sums + 2);
            }
        };

        template<> struct Convolution<3, 3>
        {
            template <bool align> static SIMD_INLINE float32x4_t Convolution3(const float * src, const float32x4_t * weights)
            {
                float32x4_t _src[3];
                _src[0] = Load<align>(src + 0);
                _src[1] = vld1q_f32(src + 1);
                _src[2] = vld1q_f32(src + 2);
                return vmlaq_f32(vmlaq_f32(vmulq_f32(_src[0], weights[0]), _src[1], weights[1]), _src[2], weights[2]);
            }

            template<bool align> static SIMD_INLINE  float32x4_t Forward(const float * src, size_t stride, const  float32x4_t * weights)
            {
                return vaddq_f32(Convolution3<align>(src, weights),
                    vaddq_f32(Convolution3<align>(src + stride, weights + 3),
                    Convolution3<align>(src + 2 * stride, weights + 6)));
            }

            template<bool align> static SIMD_INLINE float32x4_t Backward(const Buffer<3> & buffer, size_t offset, const float32x4_t * weights)
            {
                return vaddq_f32(Convolution3<align>(buffer.rows[0] + offset, weights),
                    vaddq_f32(Convolution3<align>(buffer.rows[1] + offset, weights + 3),
                    Convolution3<align>(buffer.rows[2] + offset, weights + 6)));
            }

            template <bool align> static SIMD_INLINE void Sum(const float * src, const float32x4_t & dst, float32x4_t * sums)
            {
                float32x4_t _src[3];
                _src[0] = Load<align>(src);
                _src[1] = vld1q_f32(src + 1);
                _src[2] = vld1q_f32(src + 2);
                sums[0] = vmlaq_f32(sums[0], dst, _src[0]);
                sums[1] = vmlaq_f32(sums[1], dst, _src[1]);
                sums[2] = vmlaq_f32(sums[2], dst, _src[2]);
            }

            template <bool align> static SIMD_INLINE void Sum(const float * src, size_t stride, const float32x4_t & dst, float32x4_t * sums)
            {
                Sum<align>(src + stride * 0, dst, sums + 0);
                Sum<align>(src + stride * 1, dst, sums + 3);
                Sum<align>(src + stride * 2, dst, sums + 6);
            }
        };

        template<> struct Convolution<4, 4>
        {
            template <bool align> static SIMD_INLINE float32x4_t Convolution4(const float * src, const float32x4_t * weights)
            {
                float32x4_t _src[4];
                _src[0] = Load<align>(src + 0);
                _src[1] = vld1q_f32(src + 1);
                _src[2] = vld1q_f32(src + 2);
                _src[3] = vld1q_f32(src + 3);
                return vmlaq_f32(vmlaq_f32(vmlaq_f32(vmulq_f32(_src[0], weights[0]), _src[1], weights[1]), _src[2], weights[2]), _src[3], weights[3]);
            }

            template<bool align> static SIMD_INLINE  float32x4_t Forward(const float * src, size_t stride, const  float32x4_t * weights)
            {
                return vaddq_f32(vaddq_f32(Convolution4<align>(src, weights), 
                    Convolution4<align>(src + stride, weights + 4)),
                    vaddq_f32(Convolution4<align>(src + 2 * stride, weights + 8), 
                    Convolution4<align>(src + 3 * stride, weights + 12)));
            }

            template<bool align> static SIMD_INLINE float32x4_t Backward(const Buffer<4> & buffer, size_t offset, const float32x4_t * weights)
            {
                return vaddq_f32(vaddq_f32(Convolution4<align>(buffer.rows[0] + offset, weights),
                    Convolution4<align>(buffer.rows[1] + offset, weights + 4)),
                    vaddq_f32(Convolution4<align>(buffer.rows[2] + offset, weights + 8),
                    Convolution4<align>(buffer.rows[3] + offset, weights + 12)));
            }

            template <bool align> static SIMD_INLINE void Sum(const float * src, const float32x4_t & dst, float32x4_t * sums)
            {
                float32x4_t _src[5];
                _src[0] = Load<align>(src);
                _src[1] = vld1q_f32(src + 1);
                _src[2] = vld1q_f32(src + 2);
                _src[3] = vld1q_f32(src + 3);
                sums[0] = vmlaq_f32(sums[0], dst, _src[0]);
                sums[1] = vmlaq_f32(sums[1], dst, _src[1]);
                sums[2] = vmlaq_f32(sums[2], dst, _src[2]);
                sums[3] = vmlaq_f32(sums[3], dst, _src[3]);
            }

            template <bool align> static SIMD_INLINE void Sum(const float * src, size_t stride, const float32x4_t & dst, float32x4_t * sums)
            {
                Sum<align>(src + stride * 0, dst, sums + 0);
                Sum<align>(src + stride * 1, dst, sums + 4);
                Sum<align>(src + stride * 2, dst, sums + 8);
                Sum<align>(src + stride * 3, dst, sums + 12);
            }
        };

        template<> struct Convolution<5, 5>
        {
            template <bool align> static SIMD_INLINE float32x4_t Convolution5(const float * src, const float32x4_t * weights)
            {
                float32x4_t _src[5];
                _src[0] = Load<align>(src + 0);
                _src[1] = vld1q_f32(src + 1);
                _src[2] = vld1q_f32(src + 2);
                _src[3] = vld1q_f32(src + 3);
                _src[4] = Load<align>(src + 4);
                return vmlaq_f32(vmlaq_f32(vmlaq_f32(vmlaq_f32(vmulq_f32(_src[0], weights[0]), _src[1], weights[1]), _src[2], weights[2]), _src[3], weights[3]), _src[4], weights[4]);
            }

            template<bool align> static SIMD_INLINE  float32x4_t Forward(const float * src, size_t stride, const  float32x4_t * weights)
            {
                return vaddq_f32(Convolution5<align>(src, weights), 
                    vaddq_f32(vaddq_f32(Convolution5<align>(src + stride, weights + 5), 
                    Convolution5<align>(src + 2 * stride, weights + 10)),
                    vaddq_f32(Convolution5<align>(src + 3 * stride, weights + 15), 
                    Convolution5<align>(src + 4 * stride, weights + 20))));
            }
            
            template<bool align> static SIMD_INLINE float32x4_t Backward(const Buffer<5> & buffer, size_t offset, const float32x4_t * weights)
            {
                return vaddq_f32(vaddq_f32(Convolution5<align>(buffer.rows[0] + offset, weights),
                    vaddq_f32(Convolution5<align>(buffer.rows[1] + offset, weights + 5),
                    Convolution5<align>(buffer.rows[2] + offset, weights + 10))),
                    vaddq_f32(Convolution5<align>(buffer.rows[3] + offset, weights + 15),
                    Convolution5<align>(buffer.rows[4] + offset, weights + 20)));
            }

            template <bool align> static SIMD_INLINE void Sum(const float * src, const float32x4_t & dst, float32x4_t * sums)
            {
                float32x4_t _src[5];
                _src[0] = Load<align>(src);
                _src[1] = vld1q_f32(src + 1);
                _src[2] = vld1q_f32(src + 2);
                _src[3] = vld1q_f32(src + 3);
                _src[4] = Load<align>(src + 4);
                sums[0] = vmlaq_f32(sums[0], dst, _src[0]);
                sums[1] = vmlaq_f32(sums[1], dst, _src[1]);
                sums[2] = vmlaq_f32(sums[2], dst, _src[2]);
                sums[3] = vmlaq_f32(sums[3], dst, _src[3]);
                sums[4] = vmlaq_f32(sums[4], dst, _src[4]);
            }

            template <bool align> static SIMD_INLINE void Sum(const float * src, size_t stride, const float32x4_t & dst, float32x4_t * sums)
            {
                Sum<align>(src + stride * 0, dst, sums + 0);
                Sum<align>(src + stride * 1, dst, sums + 5);
                Sum<align>(src + stride * 2, dst, sums + 10);
                Sum<align>(src + stride * 3, dst, sums + 15);
                Sum<align>(src + stride * 4, dst, sums + 20);
            }
        };

        template <bool align, size_t coreX, size_t coreY> void NeuralAddConvolutionForward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            size_t alignedWidth = AlignLo(width, F);
            float32x4_t tailMask = RightNotZero(width - alignedWidth);
            float32x4_t _weights[coreX*coreY];
            LoadWeightsForward<coreX*coreY>(weights, _weights);
            for (size_t row = 0; row < height; ++row)
            {
                size_t col = 0;
                for (; col < alignedWidth; col += F)
                {
                    float32x4_t _dst = Load<align>(dst + col);
                    _dst = vaddq_f32(_dst, Convolution<coreX, coreY>::template Forward<align>(src + col, srcStride, _weights));
                    Store<align>(dst + col, _dst);
                }
                if (width - alignedWidth)
                {
                    size_t col = width - F;
                    float32x4_t _dst = Load<false>(dst + col);
                    _dst = vaddq_f32(_dst, And(tailMask, Convolution<coreX, coreY>::template Forward<false>(src + col, srcStride, _weights)));
                    Store<false>(dst + col, _dst);
                }
                src += srcStride;
                dst += dstStride;
            }
        }

        void NeuralAddConvolution2x2Forward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionForward<true, 2, 2>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolutionForward<false, 2, 2>(src, srcStride, width, height, weights, dst, dstStride);
        }

        void NeuralAddConvolution3x3Forward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionForward<true, 3, 3>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolutionForward<false, 3, 3>(src, srcStride, width, height, weights, dst, dstStride);
        }

        void NeuralAddConvolution4x4Forward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionForward<true, 4, 4>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolutionForward<false, 4, 4>(src, srcStride, width, height, weights, dst, dstStride);
        }

        void NeuralAddConvolution5x5Forward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionForward<true, 5, 5>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolutionForward<false, 5, 5>(src, srcStride, width, height, weights, dst, dstStride);
        }

        template<bool condition> struct If
        {
            template<bool align> static SIMD_INLINE void AddMultiplied(const float * src, size_t aligned, size_t partial, size_t full, float value, float * dst)
            {
                Neon::AddMultiplied<align>(src, aligned, partial, full, value, dst);
            }
        };

        template<> struct If<false>
        {
            template<bool align> static SIMD_INLINE void AddMultiplied(const float * src, size_t aligned, size_t partial, size_t full, float value, float * dst)
            {
            }
        };

        template <bool align, int coreX, int coreY> void NeuralAddConvolutionBackwardSmall(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            size_t aligned = AlignLo(width, QF);
            size_t partial = AlignLo(width, F);
            for (size_t row = 0; row < height; ++row)
            {
                for (size_t dy = 0; dy < coreY; ++dy)
                {
                    const float * w = weights + dy * coreX;
                    float * d = dst + dy*dstStride;
                    If<0 < coreX>::template AddMultiplied<align>(src, aligned, partial, width, w[0], d + 0);
                    If<1 < coreX>::template AddMultiplied<false>(src, aligned, partial, width, w[1], d + 1);
                    If<2 < coreX>::template AddMultiplied<false>(src, aligned, partial, width, w[2], d + 2);
                    If<3 < coreX>::template AddMultiplied<false>(src, aligned, partial, width, w[3], d + 3);
                    If<4 < coreX>::template AddMultiplied<align>(src, aligned, partial, width, w[4], d + 4);
                }
                src += srcStride;
                dst += dstStride;
            }
        }

        template <bool align, size_t coreX, size_t coreY> void NeuralAddConvolutionBackwardLarge(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            Buffer<coreX> buffer(width);
            height += coreY - 1;
            width += coreX - 1;
            size_t alignedWidth = AlignLo(width, F);
            float32x4_t tailMask = RightNotZero(width - alignedWidth);
            float32x4_t _weights[coreX*coreY];
            LoadWeightsBackward<coreX*coreY>(weights, _weights);

            for (size_t row = 0; row < height; ++row)
            {
                buffer.Update(row <= height - coreY ? src : NULL);
                for (size_t col = 0; col < alignedWidth; col += F)
                {
                    float32x4_t _dst = Load<align>(dst + col);
                    _dst = vaddq_f32(_dst, Convolution<coreX, coreY>::template Backward<true>(buffer, col, _weights));
                    Store<align>(dst + col, _dst);
                }
                if (width - alignedWidth)
                {
                    size_t col = width - F;
                    float32x4_t _dst = Load<false>(dst + col);
                    _dst = vaddq_f32(_dst, And(tailMask, Convolution<coreX, coreY>::template Backward<false>(buffer, col, _weights)));
                    Store<false>(dst + col, _dst);
                }
                src += srcStride;
                dst += dstStride;
            }
        }

        template <bool align, size_t coreX, size_t coreY> void NeuralAddConvolutionBackward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (width*height < 1024)
                NeuralAddConvolutionBackwardSmall<align, coreX, coreY>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolutionBackwardLarge<align, coreX, coreY>(src, srcStride, width, height, weights, dst, dstStride);
        }

        void NeuralAddConvolution2x2Backward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionBackward<true, 2, 2>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolutionBackward<false, 2, 2>(src, srcStride, width, height, weights, dst, dstStride);
        }

        void NeuralAddConvolution3x3Backward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionBackward<true, 3, 3>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolutionBackward<false, 3, 3>(src, srcStride, width, height, weights, dst, dstStride);
        }

        void NeuralAddConvolution4x4Backward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionBackward<true, 4, 4>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolutionBackward<false, 4, 4>(src, srcStride, width, height, weights, dst, dstStride);
        }

        void NeuralAddConvolution5x5Backward(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionBackward<true, 5, 5>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolutionBackward<false, 5, 5>(src, srcStride, width, height, weights, dst, dstStride);
        }

        template <bool align, size_t coreX, size_t coreY> SIMD_INLINE void NeuralAddConvolutionSum(const float * src, size_t srcStride, const float * dst, size_t dstStride, size_t width, size_t height, float * sums)
        {
            size_t alignedWidth = Simd::AlignLo(width, F);
            float32x4_t tailMask = RightNotZero(width - alignedWidth);
            float32x4_t _sums[coreX*coreY];
            memset(_sums, 0, sizeof(_sums));
            for (size_t row = 0; row < height; ++row)
            {
                for (size_t col = 0; col < alignedWidth; col += F)
                {
                    float32x4_t _dst = Load<align>(dst + col);
                    Convolution<coreX, coreY>::template Sum<align>(src + col, srcStride, _dst, _sums);
                }
                if (alignedWidth < width)
                {
                    size_t col = width - F;
                    float32x4_t _dst = And(tailMask, Load<false>(dst + col));
                    Convolution<coreX, coreY>::template Sum<false>(src + col, srcStride, _dst, _sums);
                }
                src += srcStride;
                dst += dstStride;
            }
            for (size_t i = 0; i < coreX*coreY; ++i)
                sums[i] += ExtractSum32f(_sums[i]);
        }

        void NeuralAddConvolution2x2Sum(const float * src, size_t srcStride, const float * dst, size_t dstStride, size_t width, size_t height, float * sums)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionSum<true, 2, 2>(src, srcStride, dst, dstStride, width, height, sums);
            else
                NeuralAddConvolutionSum<false, 2, 2>(src, srcStride, dst, dstStride, width, height, sums);
        }

        void NeuralAddConvolution3x3Sum(const float * src, size_t srcStride, const float * dst, size_t dstStride, size_t width, size_t height, float * sums)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionSum<true, 3, 3>(src, srcStride, dst, dstStride, width, height, sums);
            else
                NeuralAddConvolutionSum<false, 3, 3>(src, srcStride, dst, dstStride, width, height, sums);
        }

        void NeuralAddConvolution4x4Sum(const float * src, size_t srcStride, const float * dst, size_t dstStride, size_t width, size_t height, float * sums)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionSum<true, 4, 4>(src, srcStride, dst, dstStride, width, height, sums);
            else
                NeuralAddConvolutionSum<false, 4, 4>(src, srcStride, dst, dstStride, width, height, sums);
        }

        void NeuralAddConvolution5x5Sum(const float * src, size_t srcStride, const float * dst, size_t dstStride, size_t width, size_t height, float * sums)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolutionSum<true, 5, 5>(src, srcStride, dst, dstStride, width, height, sums);
            else
                NeuralAddConvolutionSum<false, 5, 5>(src, srcStride, dst, dstStride, width, height, sums);
        }

        template <bool align> SIMD_INLINE float32x4_t Max2x2(const float * src, size_t stride)
        {
            float32x4_t s0 = vmaxq_f32(Load<align>(src + 0), Load<align>(src + stride + 0));
            float32x4_t s1 = vmaxq_f32(Load<align>(src + F), Load<align>(src + stride + F));
            return vcombine_f32(vpmax_f32(vget_low_f32(s0), vget_high_f32(s0)), vpmax_f32(vget_low_f32(s1), vget_high_f32(s1)));
        }

        template <bool align> void NeuralMax2x2(const float * src, size_t srcStride, size_t width, size_t height, float * dst, size_t dstStride)
        {
            assert((width & 1) == 0 && (height & 1) == 0);
            if (align)
                assert(Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F));

            size_t alignedWidth = AlignLo(width, DF);
            for (size_t row = 0; row < height; row += 2)
            {
                for (size_t col = 0; col < alignedWidth; col += DF)
                {
                    Store<align>(dst + (col >> 1), Max2x2<align>(src + col, srcStride));
                }
                if (width - alignedWidth)
                {
                    size_t col = width - DF;
                    Store<false>(dst + (col >> 1), Max2x2<false>(src + col, srcStride));
                }
                src += 2 * srcStride;
                dst += dstStride;
            }
        }

        void NeuralMax2x2(const float * src, size_t srcStride, size_t width, size_t height, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralMax2x2<true>(src, srcStride, width, height, dst, dstStride);
            else
                NeuralMax2x2<false>(src, srcStride, width, height, dst, dstStride);
        }
    }
#endif// SIMD_NEON_ENABLE
}
