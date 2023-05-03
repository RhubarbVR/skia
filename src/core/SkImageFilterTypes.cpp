/*
 * Copyright 2019 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkImageFilterTypes.h"

#include "include/core/SkShader.h"
#include "include/core/SkTileMode.h"
#include "src/core/SkColorFilterBase.h"
#include "src/core/SkImageFilter_Base.h"
#include "src/core/SkMatrixPriv.h"
#include "src/core/SkRectPriv.h"
#include "src/core/SkSpecialSurface.h"

// This exists to cover up issues where infinite precision would produce integers but float
// math produces values just larger/smaller than an int and roundOut/In on bounds would produce
// nearly a full pixel error. One such case is crbug.com/1313579 where the caller has produced
// near integer CTM and uses integer crop rects that would grab an extra row/column of the
// input image when using a strict roundOut.
static constexpr float kRoundEpsilon = 1e-3f;

// Both [I]Vectors and Sk[I]Sizes are transformed as non-positioned values, i.e. go through
// mapVectors() not mapPoints().
static SkIVector map_as_vector(int32_t x, int32_t y, const SkMatrix& matrix) {
    SkVector v = SkVector::Make(SkIntToScalar(x), SkIntToScalar(y));
    matrix.mapVectors(&v, 1);
    return SkIVector::Make(SkScalarRoundToInt(v.fX), SkScalarRoundToInt(v.fY));
}

static SkVector map_as_vector(SkScalar x, SkScalar y, const SkMatrix& matrix) {
    SkVector v = SkVector::Make(x, y);
    matrix.mapVectors(&v, 1);
    return v;
}

static bool fills_layer_bounds(const SkColorFilter* colorFilter) {
    return colorFilter && as_CFB(colorFilter)->affectsTransparentBlack();
}

// If m is epsilon within the form [1 0 tx], this returns true and sets out to [tx, ty]
//                                 [0 1 ty]
//                                 [0 0 1 ]
// TODO: Use this in decomposeCTM() (and possibly extend it to support is_nearly_scale_translate)
// to be a little more forgiving on matrix types during layer configuration.
static bool is_nearly_integer_translation(const skif::LayerSpace<SkMatrix>& m,
                                          skif::LayerSpace<SkIPoint>* out=nullptr) {
    float tx = SkScalarRoundToScalar(sk_ieee_float_divide(m.rc(0,2), m.rc(2,2)));
    float ty = SkScalarRoundToScalar(sk_ieee_float_divide(m.rc(1,2), m.rc(2,2)));
    SkMatrix expected = SkMatrix::MakeAll(1.f, 0.f, tx,
                                          0.f, 1.f, ty,
                                          0.f, 0.f, 1.f);
    for (int i = 0; i < 9; ++i) {
        if (!SkScalarNearlyEqual(expected.get(i), m.get(i), kRoundEpsilon)) {
            return false;
        }
    }

    if (out) {
        *out = skif::LayerSpace<SkIPoint>({(int) tx, (int) ty});
    }
    return true;
}

// Assumes 'image' is decal-tiled, so everything outside the image bounds but inside dstBounds is
// transparent black, in which case the returned special image may be smaller than dstBounds.
static std::pair<sk_sp<SkSpecialImage>, skif::LayerSpace<SkIPoint>> extract_subset(
        const SkSpecialImage* image,
        skif::LayerSpace<SkIPoint> origin,
        skif::LayerSpace<SkIRect> dstBounds) {
    skif::LayerSpace<SkIRect> imageBounds(SkIRect::MakeXYWH(origin.x(), origin.y(),
                                          image->width(), image->height()));
    if (!imageBounds.intersect(dstBounds)) {
        return {nullptr, {}};
    }

    // Offset the image subset directly to avoid issues negating (origin). With the prior
    // intersection (bounds - origin) will be >= 0, but (bounds + (-origin)) may not, (e.g.
    // origin is INT_MIN).
    SkIRect subset = { imageBounds.left() - origin.x(),
                       imageBounds.top() - origin.y(),
                       imageBounds.right() - origin.x(),
                       imageBounds.bottom() - origin.y() };
    SkASSERT(subset.fLeft >= 0 && subset.fTop >= 0 &&
             subset.fRight <= image->width() && subset.fBottom <= image->height());

    return {image->makeSubset(subset), imageBounds.topLeft()};
}

static SkRect map_rect(const SkMatrix& matrix, const SkRect& rect) {
    if (rect.isEmpty()) {
        return SkRect::MakeEmpty();
    }
    return matrix.mapRect(rect);
}

static SkIRect map_rect(const SkMatrix& matrix, const SkIRect& rect) {
    if (rect.isEmpty()) {
        return SkIRect::MakeEmpty();
    }
    // Unfortunately, there is a range of integer values such that we have 1px precision as an int,
    // but less precision as a float. This can lead to non-empty SkIRects becoming empty simply
    // because of float casting. If we're already dealing with a float rect or having a float
    // output, that's what we're stuck with; but if we are starting form an irect and desiring an
    // SkIRect output, we go through efforts to preserve the 1px precision for simple transforms.
    if (matrix.isScaleTranslate()) {
        double l = (double)matrix.getScaleX()*rect.fLeft   + (double)matrix.getTranslateX();
        double r = (double)matrix.getScaleX()*rect.fRight  + (double)matrix.getTranslateX();
        double t = (double)matrix.getScaleY()*rect.fTop    + (double)matrix.getTranslateY();
        double b = (double)matrix.getScaleY()*rect.fBottom + (double)matrix.getTranslateY();

        return {sk_double_saturate2int(sk_double_floor(std::min(l, r) + kRoundEpsilon)),
                sk_double_saturate2int(sk_double_floor(std::min(t, b) + kRoundEpsilon)),
                sk_double_saturate2int(sk_double_ceil(std::max(l, r)  - kRoundEpsilon)),
                sk_double_saturate2int(sk_double_ceil(std::max(t, b)  - kRoundEpsilon))};
    } else {
        return skif::RoundOut(matrix.mapRect(SkRect::Make(rect)));
    }
}

static bool inverse_map_rect(const SkMatrix& matrix, const SkRect& rect, SkRect* out) {
    if (rect.isEmpty()) {
        *out = SkRect::MakeEmpty();
        return true;
    }
    return SkMatrixPriv::InverseMapRect(matrix, out, rect);
}

static bool inverse_map_rect(const SkMatrix& matrix, const SkIRect& rect, SkIRect* out) {
    if (rect.isEmpty()) {
        *out = SkIRect::MakeEmpty();
        return true;
    }
    // This is a specialized inverse equivalent to the 1px precision preserving map_rect above.
    if (matrix.isScaleTranslate()) {
        double l = (rect.fLeft   - (double)matrix.getTranslateX()) / (double)matrix.getScaleX();
        double r = (rect.fRight  - (double)matrix.getTranslateX()) / (double)matrix.getScaleX();
        double t = (rect.fTop    - (double)matrix.getTranslateY()) / (double)matrix.getScaleY();
        double b = (rect.fBottom - (double)matrix.getTranslateY()) / (double)matrix.getScaleY();

        *out = {sk_double_saturate2int(sk_double_floor(std::min(l, r) + kRoundEpsilon)),
                sk_double_saturate2int(sk_double_floor(std::min(t, b) + kRoundEpsilon)),
                sk_double_saturate2int(sk_double_ceil(std::max(l, r)  - kRoundEpsilon)),
                sk_double_saturate2int(sk_double_ceil(std::max(t, b)  - kRoundEpsilon))};
        return true;
    } else {
        SkRect mapped;
        if (inverse_map_rect(matrix, SkRect::Make(rect), &mapped)) {
            *out = skif::RoundOut(mapped);
            return true;
        } else {
            return false;
        }
    }
}

namespace skif {

SkIRect RoundOut(SkRect r) { return r.makeInset(kRoundEpsilon, kRoundEpsilon).roundOut(); }

SkIRect RoundIn(SkRect r) { return r.makeOutset(kRoundEpsilon, kRoundEpsilon).roundIn(); }

sk_sp<SkSpecialSurface> Context::makeSurface(const SkISize& size,
                                             const SkSurfaceProps* props) const {
    if (!props) {
        props = &fInfo.fSurfaceProps;
    }

    SkImageInfo imageInfo = SkImageInfo::Make(size,
                                              fInfo.fColorType,
                                              kPremul_SkAlphaType,
                                              sk_ref_sp(fInfo.fColorSpace));

#if defined(SK_GANESH)
    if (fGaneshContext) {
        // FIXME: Context should also store a surface origin that matches the source origin
        return SkSpecialSurface::MakeRenderTarget(fGaneshContext,
                                                  imageInfo,
                                                  *props,
                                                  fGaneshOrigin);
    } else
#endif
#if defined(SK_GRAPHITE)
    if (fGraphiteRecorder) {
        return SkSpecialSurface::MakeGraphite(fGraphiteRecorder, imageInfo, *props);
    } else
#endif
    {
        return SkSpecialSurface::MakeRaster(imageInfo, *props);
    }
}

bool Mapping::decomposeCTM(const SkMatrix& ctm, const SkImageFilter* filter,
                           const skif::ParameterSpace<SkPoint>& representativePt) {
    SkMatrix remainder, layer;
    SkSize decomposed;
    using MatrixCapability = SkImageFilter_Base::MatrixCapability;
    MatrixCapability capability =
            filter ? as_IFB(filter)->getCTMCapability() : MatrixCapability::kComplex;
    if (capability == MatrixCapability::kTranslate) {
        // Apply the entire CTM post-filtering
        remainder = ctm;
        layer = SkMatrix::I();
    } else if (ctm.isScaleTranslate() || capability == MatrixCapability::kComplex) {
        // Either layer space can be anything (kComplex) - or - it can be scale+translate, and the
        // ctm is. In both cases, the layer space can be equivalent to device space.
        remainder = SkMatrix::I();
        layer = ctm;
    } else if (ctm.decomposeScale(&decomposed, &remainder)) {
        // This case implies some amount of sampling post-filtering, either due to skew or rotation
        // in the original matrix. As such, keep the layer matrix as simple as possible.
        layer = SkMatrix::Scale(decomposed.fWidth, decomposed.fHeight);
    } else {
        // Perspective, which has a non-uniform scaling effect on the filter. Pick a single scale
        // factor that best matches where the filter will be evaluated.
        SkScalar scale = SkMatrixPriv::DifferentialAreaScale(ctm, SkPoint(representativePt));
        if (SkScalarIsFinite(scale) && !SkScalarNearlyZero(scale)) {
            // Now take the sqrt to go from an area scale factor to a scaling per X and Y
            // FIXME: It would be nice to be able to choose a non-uniform scale.
            scale = SkScalarSqrt(scale);
        } else {
            // The representative point was behind the W = 0 plane, so don't factor out any scale.
            // NOTE: This makes remainder and layer the same as the MatrixCapability::Translate case
            scale = 1.f;
        }

        remainder = ctm;
        remainder.preScale(SkScalarInvert(scale), SkScalarInvert(scale));
        layer = SkMatrix::Scale(scale, scale);
    }

    SkMatrix invRemainder;
    if (!remainder.invert(&invRemainder)) {
        // Under floating point arithmetic, it's possible to decompose an invertible matrix into
        // a scaling matrix and a remainder and have the remainder be non-invertible. Generally
        // when this happens the scale factors are so large and the matrix so ill-conditioned that
        // it's unlikely that any drawing would be reasonable, so failing to make a layer is okay.
        return false;
    } else {
        fParamToLayerMatrix = layer;
        fLayerToDevMatrix = remainder;
        fDevToLayerMatrix = invRemainder;
        return true;
    }
}

bool Mapping::adjustLayerSpace(const SkMatrix& layer) {
    SkMatrix invLayer;
    if (!layer.invert(&invLayer)) {
        return false;
    }
    fParamToLayerMatrix.postConcat(layer);
    fDevToLayerMatrix.postConcat(layer);
    fLayerToDevMatrix.preConcat(invLayer);
    return true;
}

// Instantiate map specializations for the 6 geometric types used during filtering
template<>
SkRect Mapping::map<SkRect>(const SkRect& geom, const SkMatrix& matrix) {
    return map_rect(matrix, geom);
}

template<>
SkIRect Mapping::map<SkIRect>(const SkIRect& geom, const SkMatrix& matrix) {
    return map_rect(matrix, geom);
}

template<>
SkIPoint Mapping::map<SkIPoint>(const SkIPoint& geom, const SkMatrix& matrix) {
    SkPoint p = SkPoint::Make(SkIntToScalar(geom.fX), SkIntToScalar(geom.fY));
    matrix.mapPoints(&p, 1);
    return SkIPoint::Make(SkScalarRoundToInt(p.fX), SkScalarRoundToInt(p.fY));
}

template<>
SkPoint Mapping::map<SkPoint>(const SkPoint& geom, const SkMatrix& matrix) {
    SkPoint p;
    matrix.mapPoints(&p, &geom, 1);
    return p;
}

template<>
IVector Mapping::map<IVector>(const IVector& geom, const SkMatrix& matrix) {
    return IVector(map_as_vector(geom.fX, geom.fY, matrix));
}

template<>
Vector Mapping::map<Vector>(const Vector& geom, const SkMatrix& matrix) {
    return Vector(map_as_vector(geom.fX, geom.fY, matrix));
}

template<>
SkISize Mapping::map<SkISize>(const SkISize& geom, const SkMatrix& matrix) {
    SkIVector v = map_as_vector(geom.fWidth, geom.fHeight, matrix);
    return SkISize::Make(v.fX, v.fY);
}

template<>
SkSize Mapping::map<SkSize>(const SkSize& geom, const SkMatrix& matrix) {
    SkVector v = map_as_vector(geom.fWidth, geom.fHeight, matrix);
    return SkSize::Make(v.fX, v.fY);
}

template<>
SkMatrix Mapping::map<SkMatrix>(const SkMatrix& m, const SkMatrix& matrix) {
    // If 'matrix' maps from the C1 coord space to the C2 coord space, and 'm' is a transform that
    // operates on, and outputs to, the C1 coord space, we want to return a new matrix that is
    // equivalent to 'm' that operates on and outputs to C2. This is the same as mapping the input
    // from C2 to C1 (matrix^-1), then transforming by 'm', and then mapping from C1 to C2 (matrix).
    SkMatrix inv;
    SkAssertResult(matrix.invert(&inv));
    inv.postConcat(m);
    inv.postConcat(matrix);
    return inv;
}

LayerSpace<SkRect> LayerSpace<SkMatrix>::mapRect(const LayerSpace<SkRect>& r) const {
    return LayerSpace<SkRect>(map_rect(fData, SkRect(r)));
}

LayerSpace<SkIRect> LayerSpace<SkMatrix>::mapRect(const LayerSpace<SkIRect>& r) const {
    return LayerSpace<SkIRect>(map_rect(fData, SkIRect(r)));
}

bool LayerSpace<SkMatrix>::inverseMapRect(const LayerSpace<SkRect>& r,
                                          LayerSpace<SkRect>* out) const {
    SkRect mapped;
    if (inverse_map_rect(fData, SkRect(r), &mapped)) {
        *out = LayerSpace<SkRect>(mapped);
        return true;
    } else {
        return false;
    }
}

bool LayerSpace<SkMatrix>::inverseMapRect(const LayerSpace<SkIRect>& r,
                                          LayerSpace<SkIRect>* out) const {
    SkIRect mapped;
    if (inverse_map_rect(fData, SkIRect(r), &mapped)) {
        *out = LayerSpace<SkIRect>(mapped);
        return true;
    } else {
        return false;
    }
}

sk_sp<SkSpecialImage> FilterResult::imageAndOffset(const Context& ctx, SkIPoint* offset) const {
    auto [image, origin] = this->resolve(ctx, fLayerBounds);
    *offset = SkIPoint(origin);
    return image;
}

bool FilterResult::isCropped(const LayerSpace<SkMatrix>& xtraTransform,
                             const LayerSpace<SkIRect>& dstBounds) const {
    // Tiling and color-filtering can completely fill 'fLayerBounds' in which case its edge is
    // a transition from possibly non-transparent to definitely transparent color.
    bool fillsLayerBounds = fills_layer_bounds(fColorFilter.get());
    if (!fillsLayerBounds) {
        // When that's not the case, 'fLayerBounds' may still be important if it crops the
        // edges of the original transformed image itself.
        LayerSpace<SkIRect> imageBounds = fTransform.mapRect(
                    LayerSpace<SkIRect>{SkIRect::MakeWH(fImage->width(), fImage->height())});
        fillsLayerBounds = !fLayerBounds.contains(imageBounds);
    }

    if (fillsLayerBounds) {
        // Some content (either the image itself, or tiling/color-filtering) can produce
        // non-transparent output beyond 'fLayerBounds'. 'fLayerBounds' can only be ignored if the
        // desired output is completely contained within it (i.e. the edges of 'fLayerBounds' are
        // not visible).
        // NOTE: For the identity transform, this is equal to !fLayerBounds.contains(dstBounds)
        return !SkRectPriv::QuadContainsRect(SkMatrix(xtraTransform),
                                             SkIRect(fLayerBounds),
                                             SkIRect(dstBounds));
    } else {
        // No part of the sampled and color-filtered image would produce non-transparent pixels
        // outside of 'fLayerBounds' so 'fLayerBounds' can be ignored.
        return false;
    }
}

FilterResult FilterResult::applyCrop(const Context& ctx,
                                     const LayerSpace<SkIRect>& crop) const {
    LayerSpace<SkIRect> tightBounds = crop;
    // TODO(michaelludwig): Intersecting to the target output is only valid when the crop has
    // decal tiling (the only current option).
    if (!fImage ||
        !tightBounds.intersect(ctx.desiredOutput()) ||
        !tightBounds.intersect(fLayerBounds)) {
        // The desired output would be filled with transparent black. There should never be a
        // color filter acting on an empty image that could change that assumption.
        SkASSERT(fImage || !fColorFilter);
        return {};
    }

    LayerSpace<SkIPoint> origin;
    if (!fills_layer_bounds(fColorFilter.get()) &&
         is_nearly_integer_translation(fTransform, &origin)) {
        // We can lift the crop to earlier in the order of operations and apply it to the image
        // subset directly. This does not rely on resolve() to call extract_subset() because it
        // will still render a new image if there's a color filter. As such, we have to preserve
        // the current color filter on the new FilterResult.
        // NOTE: Even though applying a crop never renders a new image, moving the crop into the
        // image dimensions allows future operations like applying a transform or color filter to
        // be composed without rendering a new image since there is no longer an intervening crop.
        FilterResult restrictedOutput = extract_subset(fImage.get(), origin, tightBounds);
        restrictedOutput.fColorFilter = fColorFilter;
        return restrictedOutput;
    } else {
        // Otherwise cropping is the final operation to the FilterResult's image and can always be
        // applied by adjusting the layer bounds.
        FilterResult restrictedOutput = *this;
        restrictedOutput.fLayerBounds = tightBounds;
        return restrictedOutput;
    }
}

FilterResult FilterResult::applyColorFilter(const Context& ctx,
                                            sk_sp<SkColorFilter> colorFilter) const {
    static const LayerSpace<SkMatrix> kIdentity{SkMatrix::I()};

    // A null filter is the identity, so it should have been caught during image filter DAG creation
    SkASSERT(colorFilter);

    // Color filters are applied after the transform and image sampling, but before the fLayerBounds
    // crop. We can compose 'colorFilter' with any previously applied color filter regardless
    // of the transform/sample state, so long as it respects the effect of the current crop.
    LayerSpace<SkIRect> newLayerBounds = fLayerBounds;
    if (as_CFB(colorFilter)->affectsTransparentBlack()) {
        if (!fImage || !newLayerBounds.intersect(ctx.desiredOutput())) {
            // The current image's intersection with the desired output is fully transparent, but
            // the new color filter converts that into a non-transparent color. The desired output
            // is filled with this color.
            // TODO: When kClamp is supported, we can allocate a smaller surface
            sk_sp<SkSpecialSurface> surface = ctx.makeSurface(SkISize(ctx.desiredOutput().size()));
            if (!surface) {
                return {};
            }

            SkPaint paint;
            paint.setColor4f(SkColors::kTransparent, /*colorSpace=*/nullptr);
            paint.setColorFilter(std::move(colorFilter));
            surface->getCanvas()->drawPaint(paint);
            return {surface->makeImageSnapshot(), ctx.desiredOutput().topLeft()};
        }

        if (this->isCropped(kIdentity, ctx.desiredOutput())) {
            // Since 'colorFilter' modifies transparent black, the new result's layer bounds must
            // be the desired output. But if the current image is cropped we need to resolve the
            // image to avoid losing the effect of the current 'fLayerBounds'.
            FilterResult filtered = this->resolve(ctx, ctx.desiredOutput());
            return filtered.applyColorFilter(ctx, std::move(colorFilter));
        }

        // otherwise we can fill out to the desired output without worrying about losing the crop.
        newLayerBounds = ctx.desiredOutput();
    } else {
        if (!fImage || !newLayerBounds.intersect(ctx.desiredOutput())) {
            // The color filter does not modify transparent black, so it remains transparent
            return {};
        }
        // otherwise a non-transparent affecting color filter can always be lifted before any crop
        // because it does not change the "shape" of the prior FilterResult.
    }

    // If we got here we can compose the new color filter with the previous filter and the prior
    // layer bounds are either soft-cropped to the desired output, or we fill out the desired output
    // when the new color filter affects transparent black. We don't check if the entire composed
    // filter affects transparent black because earlier floods are restricted by the layer bounds.
    FilterResult filtered = *this;
    filtered.fLayerBounds = newLayerBounds;
    filtered.fColorFilter = SkColorFilters::Compose(std::move(colorFilter), fColorFilter);
    return filtered;
}

static bool compatible_sampling(const SkSamplingOptions& currentSampling,
                                bool currentXformWontAffectNearest,
                                SkSamplingOptions* nextSampling,
                                bool nextXformWontAffectNearest) {
    // Both transforms could perform non-trivial sampling, but if they are similar enough we
    // assume performing one non-trivial sampling operation with the concatenated transform will
    // not be visually distinguishable from sampling twice.
    // TODO(michaelludwig): For now ignore mipmap policy, SkSpecialImages are not supposed to be
    // drawn with mipmapping, and the majority of filter steps produce images that are at the
    // proper scale and do not define mip levels. The main exception is the ::Image() filter
    // leaf but that doesn't use this system yet.
    if (currentSampling.isAniso() && nextSampling->isAniso()) {
        // Assume we can get away with one sampling at the highest anisotropy level
        *nextSampling =  SkSamplingOptions::Aniso(std::max(currentSampling.maxAniso,
                                                           nextSampling->maxAniso));
        return true;
    } else if (currentSampling.isAniso() && nextSampling->filter == SkFilterMode::kLinear) {
        // Assume we can get away with the current anisotropic filter since the next is linear
        *nextSampling = currentSampling;
        return true;
    } else if (nextSampling->isAniso() && currentSampling.filter == SkFilterMode::kLinear) {
        // Mirror of the above, assume we can just get away with next's anisotropic filter
        return true;
    } else if (currentSampling.useCubic && (nextSampling->filter == SkFilterMode::kLinear ||
                                            (nextSampling->useCubic &&
                                             currentSampling.cubic.B == nextSampling->cubic.B &&
                                             currentSampling.cubic.C == nextSampling->cubic.C))) {
        // Assume we can get away with the current bicubic filter, since the next is the same
        // or a bilerp that can be upgraded.
        *nextSampling = currentSampling;
        return true;
    } else if (nextSampling->useCubic && currentSampling.filter == SkFilterMode::kLinear) {
        // Mirror of the above, assume we can just get away with next's cubic resampler
        return true;
    } else if (currentSampling.filter == SkFilterMode::kLinear &&
               nextSampling->filter == SkFilterMode::kLinear) {
        // Assume we can get away with a single bilerp vs. the two
        return true;
    } else if (nextSampling->filter == SkFilterMode::kNearest && currentXformWontAffectNearest) {
        // The next transform and nearest-neighbor filtering isn't impacted by the current transform
        SkASSERT(currentSampling.filter == SkFilterMode::kLinear);
        return true;
    } else if (currentSampling.filter == SkFilterMode::kNearest && nextXformWontAffectNearest) {
        // The next transform doesn't change the nearest-neighbor filtering of the current transform
        SkASSERT(nextSampling->filter == SkFilterMode::kLinear);
        *nextSampling = currentSampling;
        return true;
    } else {
        // The current or next sampling is nearest neighbor, and will produce visible texels
        // oriented with the current transform; assume this is a desired effect and preserve it.
        return false;
    }
}

FilterResult FilterResult::applyTransform(const Context& ctx,
                                          const LayerSpace<SkMatrix> &transform,
                                          const SkSamplingOptions &sampling) const {
    if (!fImage) {
        // Transformed transparent black remains transparent black.
        SkASSERT(!fColorFilter);
        return {};
    }

    // Extract the sampling options that matter based on the current and next transforms.
    // We make sure the new sampling is bilerp (default) if the new transform doesn't matter
    // (and assert that the current is bilerp if its transform didn't matter). Bilerp can be
    // maximally combined, so simplifies the logic in compatible_sampling().
    const bool currentXformIsInteger = is_nearly_integer_translation(fTransform);
    const bool nextXformIsInteger = is_nearly_integer_translation(transform);

    SkASSERT(!currentXformIsInteger || fSamplingOptions == kDefaultSampling);
    SkSamplingOptions nextSampling = nextXformIsInteger ? kDefaultSampling : sampling;

    // Determine if the image is being visibly cropped by the layer bounds, in which case we can't
    // merge this transform with any previous transform (unless the new transform is an integer
    // translation in which case any visible edge is aligned with the desired output and can be
    // resolved by intersecting the transformed layer bounds and the output bounds).
    bool isCropped = !nextXformIsInteger && this->isCropped(transform, ctx.desiredOutput());

    FilterResult transformed;
    if (!isCropped && compatible_sampling(fSamplingOptions, currentXformIsInteger,
                                          &nextSampling, nextXformIsInteger)) {
        // We can concat transforms and 'nextSampling' will be either fSamplingOptions,
        // sampling, or a merged combination depending on the two transforms in play.
        transformed = *this;
    } else {
        // We'll have to resolve this FilterResult first before 'transform' and 'sampling' can be
        // correctly evaluated. 'nextSampling' will always be 'sampling'.
        LayerSpace<SkIRect> tightBounds;
        if (transform.inverseMapRect(ctx.desiredOutput(), &tightBounds)) {
            transformed = this->resolve(ctx, tightBounds);
        }

        if (!transformed.fImage) {
            // Transform not invertible or resolve failed to create an image
            return {};
        }
    }

    transformed.fSamplingOptions = nextSampling;
    transformed.fTransform.postConcat(transform);
    // Rebuild the layer bounds and then restrict to the current desired output. The original value
    // of fLayerBounds includes the image mapped by the original fTransform as well as any
    // accumulated soft crops from desired outputs of prior stages. To prevent discarding that info,
    // we map fLayerBounds by the additional transform, instead of re-mapping the image bounds.
    transformed.fLayerBounds = transform.mapRect(transformed.fLayerBounds);
    if (!transformed.fLayerBounds.intersect(ctx.desiredOutput())) {
        // The transformed output doesn't touch the desired, so it would just be transparent black.
        // TODO: This intersection only applies when the tile mode is kDecal.
        return {};
    }

    return transformed;
}

std::pair<sk_sp<SkSpecialImage>, LayerSpace<SkIPoint>> FilterResult::resolve(
        const Context& ctx,
        LayerSpace<SkIRect> dstBounds) const {
    // The layer bounds is the final clip, so it can always be used to restrict 'dstBounds'. Even
    // if there's a non-decal tile mode or transparent-black affecting color filter, those floods
    // are restricted to fLayerBounds.
    if (!fImage || !dstBounds.intersect(fLayerBounds)) {
        return {nullptr, {}};
    }

    // If we have any extra effect to apply, there's no point in trying to extract a subset.
    // TODO: Also factor in a non-decal tile mode
    const bool subsetCompatible = !fColorFilter;

    // TODO(michaelludwig): If we get to the point where all filter results track bounds in
    // floating point, then we can extend this case to any S+T transform.
    LayerSpace<SkIPoint> origin;
    if (subsetCompatible && is_nearly_integer_translation(fTransform, &origin)) {
        return extract_subset(fImage.get(), origin, dstBounds);
    } // else fall through and attempt a draw

    // Don't use context properties to avoid DMSAA on internal stages of filter evaluation.
    SkSurfaceProps props = {};
    sk_sp<SkSpecialSurface> surface = ctx.makeSurface(SkISize(dstBounds.size()), &props);
    if (!surface) {
        return {nullptr, {}};
    }

    // Since dstBounds has been intersected with fLayerBounds already, there is no need to
    // explicitly clip the surface's canvas.
    SkCanvas* canvas = surface->getCanvas();
    // skbug.com/5075: GPU-backed special surfaces don't reset their contents.
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->translate(-dstBounds.left(), -dstBounds.top()); // dst's origin adjustment

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setColorFilter(fColorFilter);

    canvas->concat(SkMatrix(fTransform)); // src's origin is embedded in fTransform

    if (fills_layer_bounds(fColorFilter.get())) {
        paint.setShader(fImage->asShader(SkTileMode::kDecal, fSamplingOptions, SkMatrix::I()));
        canvas->drawPaint(paint);
    } else {
        fImage->draw(canvas, 0.f, 0.f, fSamplingOptions, &paint);
    }
    return {surface->makeImageSnapshot(), dstBounds.topLeft()};
}

} // end namespace skif
