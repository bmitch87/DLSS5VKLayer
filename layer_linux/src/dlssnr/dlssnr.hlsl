
#ifdef VK_MODE
[[vk::binding(0, 0)]]
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gColourStrength;
    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    float gMvScaleX;     // motion vector units -> pixels of this dispatch
    float gMvScaleY;
    uint  gGuideWidth;   // the motion texture's valid region
    uint  gGuideHeight;
    uint  gCompareMode;  // 0 off, 1 side by side, 2 wipe
    float gCompareSplit; // where the wipe cuts, 0..1
    float gCompareZoom;  // side by side: 1 fits the frame, 2 fills the half
    uint  gCompareSwap;  // put the edited frame on the other side
    uint  gTransfer;     // 0 classic, 1 matched residual, 2 native + edit -- how a below-size model comes back
    float gDebugScale;   // what the debug views are scaled by, held still while the meter moves
    uint  gReversibleMode; // 0 knee, 1 Neutwo+composed, 2 Neutwo+replace, 3 hybrid+composed, 4 hybrid+replace
    uint  gApplyModel;     // 0 output the clean frame (pass still runs), 1 apply the model's edit
    uint  gUseGameExposure;// D3D12 source-1 only: 1 = read the game's live exposure in-shader (t4)
    float gExposurePreMul; // preExposure * trim, so the live white point is gExposurePreMul / exposure
    uint  gHdrProxy;       // 1: the proxy surface is float16 carrying linear HDR normalised by the
                           //    white point -- no knee, no sRGB, no ceiling. Off is the SDR path.
    uint  gHdrTransfer;    // 1 with gHdrProxy: the swapchain carries PQ (ST 2084), so the frame is
                           //    PQ-decoded on the way in and PQ-encoded on the way out.
    float gColourTrust;    // maximum chroma displacement from the frame, in normalized units
    float gRatioSmooth;    // how much of the relighting ratio to take from the neighbourhood


};

// Bringing an impossible colour back into a possible one.
//
// A colour with a negative component is not a colour any display can show, and the composition can
// produce one: the model's answer is rescaled by a ratio and its chroma rebuilt, and either step can
// push a saturated pixel past the edge of the gamut.
//
// This used to be a hard clamp -- convert to AP1, max() every channel against zero, convert back --
// which is a per-channel operation on exactly the pixels most likely to breach, and per-channel
// operations on saturated pixels are the hue distorter this file warns about everywhere else. The
// channel that hits the wall first decides the colour of the rest.
//
// Instead the whole colour is scaled toward the neutral axis by one factor, so its hue survives and
// only its saturation gives way. And it is exactly nothing when nothing is out of gamut: with every
// component non-negative the scale is 1 and the colour comes back bit-for-bit.
//
// Taken from RenoDX's DLSS 5 addon by clshortfuse (https://github.com/clshortfuse/renodx), whose
// implementation this is -- the D65 adaptation state, the reversible scale and the LMS basis are
// theirs. See third_party/optiscaler/RenoDX_ATTRIBUTION.txt.

float SanitizeFinite(float v, float fallback) { return isfinite(v) ? v : fallback; }

float3 SanitizeFinite3(float3 v, float3 fallback)
{
    return float3(SanitizeFinite(v.x, fallback.x), SanitizeFinite(v.y, fallback.y),
                  SanitizeFinite(v.z, fallback.z));
}

float SafeDivide(float numerator, float denominator, float fallback)
{
    return abs(denominator) > 1e-8 ? numerator / denominator : fallback;
}

// Hunt-Pointer-Estevez LMS over linear BT.709, carrying the fixed D65 adaptation state the
// compression is defined against. The signal itself never leaves BT.709.
float3 LMSToBT709(float3 color)
{
    const float3x3 m = { 5.62059812, -4.57145756, 0.15577924,
                         -1.15555585, 2.25800438, -0.15415806,
                         0.03059913, -0.19018011, 1.06820532 };
    return mul(m, color);
}

float3 BT709ToLMS(float3 color)
{
    const float3x3 m = { 0.30569589, 0.62271286, 0.04528636,
                         0.15776262, 0.76968599, 0.08807030,
                         0.01933082, 0.11919478, 0.95053215 };
    return mul(m, color);
}

// The neutral colour of the same luminance as what is being compressed -- the point everything is
// pulled toward, so that pulling changes saturation and not hue.
float3 D65NeutralBT709(float3 adaptiveStateLms, float luminance)
{
    float3 d65 = LMSToBT709(max(adaptiveStateLms, 1e-8));
    float d65Y = max(dot(d65, float3(0.2126, 0.7152, 0.0722)), 1e-8);
    return d65 * (luminance / d65Y);
}

// The largest scale toward the neutral axis that leaves no channel negative. One for a colour that
// was already representable, which is why this is safe to run on every pixel.
float GamutCompressionScale(float3 color, float3 adaptiveStateLms)
{
    color = SanitizeFinite3(color, float3(0.0, 0.0, 0.0));

    const float y = dot(color, float3(0.2126, 0.7152, 0.0722));

    if (!(y > 1e-8))
        return 1.0;

    const float3 neutral = D65NeutralBT709(adaptiveStateLms, y);
    float scale = 1.0;

    if (color.r < 0.0 && neutral.r > color.r)
        scale = min(scale, SafeDivide(neutral.r, neutral.r - color.r, 1.0));

    if (color.g < 0.0 && neutral.g > color.g)
        scale = min(scale, SafeDivide(neutral.g, neutral.g - color.g, 1.0));

    if (color.b < 0.0 && neutral.b > color.b)
        scale = min(scale, SafeDivide(neutral.b, neutral.b - color.b, 1.0));

    return saturate(SanitizeFinite(scale, 1.0));
}

float3 ClampAp1(float3 color)
{
    const float3 adaptiveStateLms = BT709ToLMS(float3(0.18, 0.18, 0.18));
    const float scale = GamutCompressionScale(color, adaptiveStateLms);

    // Nothing was out of gamut. Leave the colour exactly as it arrived.
    if (scale >= 1.0)
        return color;

    const float y = dot(color, float3(0.2126, 0.7152, 0.0722));
    const float3 neutral = D65NeutralBT709(adaptiveStateLms, y);

    return SanitizeFinite3(neutral + (color - neutral) * scale, max(neutral, 0.0));
}

// ---------------------------------------------------------------------------------------------
// The composition below (UpgradeToneMap's two-branch ratio, the OkLab hue correction, and the blend
// between a luminance-only result and the model's own colour) is taken from RenoDX's DLSS 5 addon by
// clshortfuse -- https://github.com/clshortfuse/renodx. It is their design, not ours; see
// third_party/optiscaler/RenoDX_ATTRIBUTION.txt. The OkLab matrices are Bjorn Ottosson's published constants and the
// AP1, sRGB and PQ transforms are standard colour science.
// ---------------------------------------------------------------------------------------------

// OkLab, so the model's colour can be reached without its hue being invented on the way. A ratio
// applied to an RGB triple does not move hue, but a difference added to one does -- which is what the
// old composition did, and why a warm subject could come back green. Here the result's chroma is
// rebuilt in the model's own hue direction and only its magnitude is taken from the scaled colour.
float3 CbrtSigned(float3 v) { return sign(v) * pow(abs(v), 1.0 / 3.0); }

float3 ToOkLab(float3 color)
{
    const float3x3 rgb_to_lms = { 0.4122214708, 0.5363325363, 0.0514459929,
                                  0.2119034982, 0.6806995451, 0.1073969566,
                                  0.0883024619, 0.2817188376, 0.6299787005 };
    const float3x3 lms_to_lab = { 0.2104542553, 0.7936177850, -0.0040720468,
                                  1.9779984951, -2.4285922050, 0.4505937099,
                                  0.0259040371, 0.7827717662, -0.8086757660 };
    return mul(lms_to_lab, CbrtSigned(mul(rgb_to_lms, color)));
}

float3 FromOkLab(float3 lab)
{
    const float3x3 lab_to_lms = { 1.0, 0.3963377774, 0.2158037573,
                                  1.0, -0.1055613458, -0.0638541728,
                                  1.0, -0.0894841775, -1.2914855480 };
    const float3x3 lms_to_rgb = { 4.0767416621, -3.3077115913, 0.2309699292,
                                  -1.2684380046, 2.6097574011, -0.3413193965,
                                  -0.0041960863, -0.7034186147, 1.7076147010 };
    float3 lms = mul(lab_to_lms, lab);
    return mul(lms_to_rgb, lms * lms * lms);
}

// Takes the hue and the chroma direction from `correct`, and only the chroma magnitude from
// `incorrect`. Scaling a colour by a luminance ratio changes how saturated it reads; this puts the
// saturation back where the model meant it without letting the hue drift.
// Takes the hue and chroma direction from `correct` and only the chroma magnitude from
// `incorrect`, so a rescaled colour keeps the model's own hue rather than drifting toward whatever
// the scaling did to its channels.
float3 HueOkLab(float3 incorrect, float3 correct)
{
    float3 incorrectLab = ToOkLab(incorrect);
    const float3 correctLab = ToOkLab(correct);
    const float incorrectChroma = length(incorrectLab.yz);
    const float correctChroma = length(correctLab.yz);

    // Normalise the direction before scaling it, rather than scaling by a ratio of magnitudes.
    //
    // The two are the same algebra -- correctLab.yz * (incorrectChroma / correctChroma) is
    // (correctLab.yz / correctChroma) * incorrectChroma -- but only this order is bounded. The
    // other divides by correctChroma while guarding it with `== 0.0`, which is an exact float
    // comparison and so catches only a chroma that is precisely zero. A model pixel that is merely
    // very close to grey has a chroma of about 1e-7, sails past that guard, and turns a hue
    // direction with no meaningful magnitude into a multiplier of ten thousand. The result is a
    // saturated colour pulled out of numerical noise.
    //
    // Written this way the direction is unit length by construction and the result cannot exceed
    // incorrectChroma, whatever the model returned. Nioh 3 is where this showed: a night scene
    // leaves most of the frame near-achromatic, so near-zero chroma is the common case rather than
    // the edge, and the speckle it produced was reported as green noise.
    const float2 hueDirection = correctChroma > 1e-5 ? correctLab.yz / correctChroma : float2(0.0, 0.0);

    incorrectLab.yz = hueDirection * incorrectChroma;

    return ClampAp1(FromOkLab(incorrectLab));
}

// Bindings are stated for SPIR-V rather than inferred. D3D keeps b, t, u and s in separate register
// files, so b0 and t0 do not collide; Vulkan has one number line per descriptor set, and dxc's default
// mapping would put both at binding 0. The numbers below are the order the pass binds them in, and
// DlssNr_Vk's descriptor set layout has to agree with them entry for entry.
#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4>   gSource   : register(t0);  // encode: the frame. resolve: the proxy.
#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float4>   gModel    : register(t1);  // resolve: what the model returned.
#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
#ifdef VK_MODE
[[vk::binding(4, 0)]]
#endif
Texture2D<float4>   gMotion   : register(t3);  // resolve, accumulating: the game's motion vectors.

// The game's 1x1 exposure texture, bound at t4 (DispatchPass's "prev edit" SRV slot). D3D12 only:
// Vulkan has no eighth descriptor for it and keeps computing the white point on the CPU, so the whole
// live path is compiled out under VK_MODE and gUseGameExposure is never set on that backend.
#ifndef VK_MODE
Texture2D<float4>   gExposure : register(t4);
#endif
#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
RWTexture2D<float4> gTarget   : register(u0);  // encode: the proxy. resolve: the frame.
#ifdef VK_MODE
[[vk::binding(6, 0)]]
#endif
RWTexture2D<float4> gKeep     : register(u1);  // encode: the untouched copy. unused by the resolve.
#ifdef VK_MODE
[[vk::binding(7, 0)]]
#endif
SamplerState        gLinear   : register(s0);  // so the edit can be read at a different size

// The picture white point. Sources 0 (paper white) and 2 (scan) resolve it on the CPU and pass it in
// gWhitePoint; source 1 (the game's own exposure) also passes a CPU value in gWhitePoint as a fallback,
// but when the exposure texture is bound (D3D12) it is recomputed HERE from the live exposure --
// gExposurePreMul (= preExposure * trim) / exposure -- which removes the 3-4 frame CPU-readback lag the
// meter path has. The clamp matches the CPU path's [0.01, 4096]. Vulkan compiles the live path out and
// always returns the CPU value, so its behaviour is unchanged.
float WhitePoint()
{
#ifndef VK_MODE
    if (gUseGameExposure != 0)
    {
        float e = gExposure.Load(int3(0, 0, 0)).r;
        if (e > 1e-6 && e < 1e6)
            return clamp(gExposurePreMul / e, 0.01, 4096.0);
        // A missing or absurd sample falls through to the CPU value the meter path still maintains.
    }
#endif
    return max(gWhitePoint, 1e-4);
}


static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// sRGB rather than a plain 2.2 power: it is what an SDR game buffer actually carries, and the model was
// trained on those.
float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

// ST 2084 (PQ), in the normalised form both ends of this pipeline use: 1.0 means 10000 nits at the
// boundary and 0.0 means black. A PQ swapchain hands over exactly this [0,1] code, and the float16
// proxy carries the decoded linear value in the same units, so the white point divides them alike.
// The constants are the standard ones (2610/4096*256/16 ... written as decimals for the same reason
// every other colour constant here is a decimal).
static const float kPqM1 = 0.1593017578125;   // 2610 / 16384
static const float kPqM2 = 78.84375;          // 2523 / 4096 * 128
static const float kPqC1 = 0.8359375;         // 3424 / 4096
static const float kPqC2 = 18.8515625;        // 2413 / 4096 * 32
static const float kPqC3 = 18.6875;           // 2392 / 4096 * 32

// Paper white on a PQ display: 203 nits of the 10000-nit range. The composition works in units where
// 1.0 is paper white, and a PQ frame has no exposure to divide out -- its scale is absolute -- so
// this is the divisor that puts a UI white at 1.0. The user's white point scales it from there.
static const float kPqPaperWhite = 0.0203;

float3 PqToLinear(float3 pq)
{
    float3 q = pow(max(pq, 0.0), 1.0 / kPqM2);
    return pow(max(q - kPqC1, 0.0) / (kPqC2 - kPqC3 * q), 1.0 / kPqM1);
}

float3 LinearToPq(float3 lin)
{
    float3 q = pow(max(lin, 0.0), kPqM1);
    return pow((kPqC1 + kPqC2 * q) / (1.0 + kPqC3 * q), kPqM2);
}

// The white point in the composition's normalised units -- 1.0 is paper white whatever the frame's
// transfer function. The SDR and float paths divide by the white point directly; a PQ frame carries
// absolute nits, so its paper white sits at a fixed fraction of the range and the white point acts
// as a multiplier on that.
float NormScale()
{
    if (gHdrProxy != 0)
        return max(gWhitePoint, 1e-4) * (gHdrTransfer != 0 ? kPqPaperWhite : 1.0);
    return gPassthrough != 0 ? 1.0 : max(gWhitePoint, 1e-4);
}

// The edit at an arbitrary position, exactly as the resolve computes its own.
float3 EditAt(float2 uvq)
{
    float3 p = gSource.SampleLevel(gLinear, uvq, 0).rgb;
    float3 m = gModel.SampleLevel(gLinear, uvq, 0).rgb;

    if (gPassthrough == 0)
    {
        p = SrgbToLinear(p);
        m = SrgbToLinear(m);
    }

    return m - p;
}


// The soft knee, shared by the encode and the resolve.
//
// The encode applies it on the way in; the resolve has to be able to reproduce it, because the
// matched-residual path needs the frame's own proxy at full resolution and the encode only ever
// wrote a reduced one. It is a pure function of the pixel, so recomputing costs less than the
// texture read it replaces.
float3 SoftKnee(float3 display)
{
    if (gPassthrough != 0)
        return display;

    float displayLuma = dot(display, kLuma);

    if (displayLuma > 0.75)
    {
        float rolled = 0.75 + 0.25 * (1.0 - exp(-(displayLuma - 0.75) / 0.25));
        display *= rolled / displayLuma;
    }

    // Per-channel headroom, with the hue kept.
    //
    // The roll-off above is on luminance, and luminance is a weighted sum in which blue counts for
    // seven percent. A saturated blue can therefore sit at B = 2 with a luminance of 0.14, pass the
    // knee untouched, and be clipped per channel by the saturate in LinearToSrgb -- and clipping one
    // channel of a triple is a hue rotation, so blue arrives as cyan. That was the green cast over
    // every blue thing in GTA V at colour strength 1: the sky, the denim, the minimap. The model was
    // shown a cyan proxy, answered in cyan, and at colour strength 1 its hue is the frame's hue.
    //
    // One scalar on the whole triple cannot move hue, so the peak channel is brought to 1 that way.
    // Only pixels that were already being clipped are touched, so everything else is bit-identical
    // to before, and the resolve's reconstruction of this proxy stays exact because it goes through
    // this same function.
    float peak = max(display.r, max(display.g, display.b));

    if (peak > 1.0)
        display /= peak;

    return display;
}

// The reversible proxy, from RenoDX's Sep-2 DLSS 5 addon (clshortfuse) -- an unclipped, hue-preserving
// encode meant to be reproduced exactly, so the model is shown the highlight gradation the soft knee
// compresses into a razor-thin band near white. Neutwo maps [0, inf) -> [0, 1) with no clip point,
// applied as ONE scalar on the peak channel so the three channels keep their ratios and hue cannot
// bend. The knee reaches its asymptote within a stop of white -- scene 2 and scene 4 arrive ~0.001
// apart, nothing the model can resolve between; Neutwo puts them ~0.076 apart.
//
// This changes only WHAT the proxy is. The resolve already works in a hybrid space -- proxy and model
// in display [0,1], original in linear -- and its ratio bridges the two, so nothing downstream has to
// change: the proxy is decoded by the same SrgbToLinear, compared the same way, and the ratio carries
// the model's answer back to the original's linear luminance exactly as before. Only the matched-
// residual proxy rebuild, which reproduces the encode, switches curve with it.
//
// Gamut nuance (RenoDX also compresses toward the D65 neutral axis first) is deferred: out-of-BT.709
// negative channels are clamped to zero here, enough for the highlight question this measures. See
// third_party/optiscaler/RenoDX_ATTRIBUTION.txt.
float Neutwo(float x) { return x * rsqrt(x * x + 1.0); } // [0, inf) -> [0, 1), no clip point

float3 NeutwoEncode(float3 v)
{
    v = max(v, 0.0);
    float m = max(v.r, max(v.g, v.b));

    if (m <= 1e-6)
        return v;

    // One scalar taken from the peak channel keeps the hue; the peak lands at Neutwo(m) < 1, so no
    // channel clips and LinearToSrgb's saturate never fires -- the proxy is fully invertible.
    return v * (Neutwo(m) / m);
}

// The exact inverse of NeutwoEncode, for the "replace" decode: y/sqrt(1 - y^2) on the peak channel,
// same one-scalar-preserves-hue trick. The inverse diverges at 1, so the peak is clamped just below
// it -- this is the steep-highlight-slope the toggle's help warns about: a highlight at the ceiling
// decodes to a very large but finite value. Only the replace path uses this; the composed path never
// decodes (its ratio bridges display->linear instead), so mode 0/1 are untouched by it.
float3 NeutwoDecode(float3 y)
{
    y = max(y, 0.0);
    float m = max(y.r, max(y.g, y.b));
    m = min(m, 0.999999);

    if (m <= 1e-6)
        return y;

    float x = m * rsqrt(max(1.0 - m * m, 1e-8)); // Neutwo^-1 of the peak
    return y * (x / m);
}

// The hybrid proxy (mode 3): the fix for the two curves each only winning in some scenes. The soft
// knee is fine in the midtones but crushes highlights; Neutwo fixes the highlights but compresses the
// midtones too, so it only helps where the knee was hurting (bright content) and is a downgrade in
// soft-lit content. The hybrid is IDENTITY below the knee -- so midtones are exactly what the soft
// knee already gave (as good as Off) -- and an unclipped, gentle Neutwo-of-the-excess ABOVE it, so
// highlights get the gradation the model needs. C1-continuous at the knee. One proxy that is >= Off
// everywhere: no midtone loss, plus the highlight win. (Composed only -- mode 3 does not replace.)
float HybridCurve(float m)
{
    const float k = 0.75; // knee point: identity below, gentle unclipped roll above

    if (m <= k)
        return m;

    const float e = (m - k) / (1.0 - k);         // excess above the knee, [0, inf)
    return k + (1.0 - k) * (e * rsqrt(e * e + 1.0)); // Neutwo(e) scaled into [k, 1); -> 1, never clips
}

float3 HybridEncode(float3 v)
{
    v = max(v, 0.0);
    float m = max(v.r, max(v.g, v.b));

    if (m <= 1e-6)
        return v;

    // One scalar on the peak channel, hue preserved. Below the knee the scalar is 1 (identity); above
    // it the peak lands at HybridCurve(m) < 1, so no channel clips.
    return v * (HybridCurve(m) / m);
}

// The exact inverse of the hybrid curve, for the hybrid REPLACE decode (mode 4). Because it is IDENTITY
// below the knee, the steep expansion is confined to genuine highlights: midtone model wobble is not
// amplified, so hybrid-replace flashes far less than Neutwo-replace while keeping the raw model detail.
float HybridCurveInv(float y)
{
    const float k = 0.75;

    if (y <= k)
        return y;

    float u = (y - k) / (1.0 - k);                  // Neutwo(e), in [0,1)
    u = min(u, 0.999999);                           // the inverse diverges at 1
    const float e = u * rsqrt(max(1.0 - u * u, 1e-8)); // Neutwo^-1 of the excess
    return k + (1.0 - k) * e;
}

float3 HybridDecode(float3 y)
{
    y = max(y, 0.0);
    float m = max(y.r, max(y.g, y.b));

    if (m <= 1e-6)
        return y;

    return y * (HybridCurveInv(m) / m);
}

// Scale a residual so the result cannot leave the unit cube, without changing its direction.
//
// The model's edit is carried up from a smaller raster and laid on the frame's own proxy, so nothing
// guarantees the sum is still a colour. Clamping per channel would bend the hue -- the channel that
// hits the wall first decides the colour of the rest -- so the whole residual is scaled by the
// largest factor that keeps every channel inside, and the direction survives.
//
// hhkbble's, from the multi-pass PR against this fork.
float3 CubeScaleResidual(float3 P, float3 T)
{
    // The unit cube is the SDR proxy's bound. A float16 proxy is unbounded by design -- the edit may
    // take a pixel past 1.0 into real HDR and that is the picture, not an overflow.
    if (gPassthrough != 0 || gHdrProxy != 0)
        return T;

    float3 d = T - P;
    float alpha = 1.0;

    [unroll] for (int c = 0; c < 3; ++c)
    {
        if (d[c] > 1e-6)
            alpha = min(alpha, (1.0 - P[c]) / d[c]);
        else if (d[c] < -1e-6)
            alpha = min(alpha, (0.0 - P[c]) / d[c]);
    }

    return P + saturate(alpha) * d;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    // Normalised, so the source may be any size relative to this dispatch.
    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    // The meter. One thread per tile of a 64x64 grid over the frame, writing that tile's mean
    // luminance. The frame is raw linear here -- this runs before the encode, on purpose, because the
    // number being looked for is what the encode's divisor should be.
    //
    // A mean per tile, then a percentile across tiles on the CPU. Not the frame's mean, which is what
    // the meter this replaces measured: that reads scene brightness, and a dark scene then asks for a
    // small divisor and hands the model a blown picture anyway. Not the frame's maximum either, which
    // one specular hit decides.
    // What scale is this game's buffer on?
    //
    // Not a taste question. The composition divides the frame by paper white to work in a normalised
    // space, and the right divisor is the one that lands the picture in [0,1]. Nioh 3 needs about 240
    // because its linear buffer holds values around two hundred; GTA V's exposure yields 2.7. Below
    // the correct value the frame is never normalised, the headroom branch computes ratios in the
    // hundreds, and ToOkLab is handed values far outside the range its cube root was built for -- the
    // green tint.
    //
    // Measured from the UNTOUCHED copy the encode kept, never from the frame this pass writes. That
    // distinction is the whole reason this is safe where the old white point meter was not: that one
    // read its own output and chased it, walking one Enshrouded session from 0.010 to 97.910. There
    // is no path from what this pass writes back into what this reads.
    //
    // Per tile, the peak luminance rather than the mean. The mean is scene brightness and says
    // nothing about scale; the peak says where the top of the range is, which is exactly what the
    // divisor has to match. One specular hit cannot decide the answer because the host takes a
    // percentile across tiles afterwards.
    if (gMode == 4)
    {
        uint fullW, fullH;
        gSource.GetDimensions(fullW, fullH);

        const uint tx0 = (uint) (((float) id.x * (float) fullW) / (float) gWidth);
        const uint tx1 = (uint) (((float) (id.x + 1) * (float) fullW) / (float) gWidth);
        const uint ty0 = (uint) (((float) id.y * (float) fullH) / (float) gHeight);
        const uint ty1 = (uint) (((float) (id.y + 1) * (float) fullH) / (float) gHeight);

        // Sixteen samples a side rather than eight, and offset half a step in so the lattice does not
        // sit on the tile's own corner.
        //
        // A fixed sample count over a growing tile means a shrinking fraction of it is read: eight per
        // side covers about 17% of a tile at 1080p but only 4% at 4K, so the same scene reported a
        // lower peak -- and therefore a smaller suggested divisor -- the higher the resolution. That is
        // a measurement that changes with the setting rather than with the game.
        const uint stepX = max((tx1 - tx0) / 16u, 1u);
        const uint stepY = max((ty1 - ty0) / 16u, 1u);

        float peak = 0.0;

        for (uint ty = ty0; ty < max(ty1, ty0 + 1u); ty += stepY)
        {
            for (uint tx = tx0; tx < max(tx1, tx0 + 1u); tx += stepX)
            {
                float3 c = max(gSource.Load(int3(min(tx, fullW - 1u), min(ty, fullH - 1u), 0)).rgb, 0.0);
                if (gHdrProxy != 0 && gHdrTransfer != 0)
                    c = PqToLinear(c);  // a PQ peak is a code, not a luminance; the divisor is in nits
                peak = max(peak, dot(c, kLuma));
            }
        }

        gTarget[id.xy] = float4(peak, 0.0, 0.0, 1.0);
        return;
    }

    if (gMode == 3)
    {
        // Tile (0,0) carries the game's own exposure rather than a tile mean.
        //
        // The exposure is a 1x1 texture the game owns, in a resource state this pass did not set and
        // must not assume. Copying it would mean transitioning someone else's resource on a guess,
        // which is how a device is lost. Reading it as an SRV in a pass that is already running costs
        // nothing and touches no state -- and it rides back on the readback that already exists.
        //
        // The motion slot is free here: the meter has no use for motion vectors.
        if (id.x == 0 && id.y == 0)
        {
            gTarget[id.xy] = float4(gMotion.Load(int3(0, 0, 0)).r, 0.0, 0.0, 1.0);
            return;
        }

        uint fullW, fullH;
        gSource.GetDimensions(fullW, fullH);

        const uint tx0 = (uint) (((float) id.x * (float) fullW) / (float) gWidth);
        const uint tx1 = (uint) (((float) (id.x + 1) * (float) fullW) / (float) gWidth);
        const uint ty0 = (uint) (((float) id.y * (float) fullH) / (float) gHeight);
        const uint ty1 = (uint) (((float) (id.y + 1) * (float) fullH) / (float) gHeight);

        // A tile of a 4K frame is 60x34 pixels. Sampling a bounded number of them is within a percent
        // of the true mean and keeps the pass flat regardless of resolution.
        const uint stepX = max((tx1 - tx0) / 8u, 1u);
        const uint stepY = max((ty1 - ty0) / 8u, 1u);

        float sum = 0.0;
        uint taken = 0;

        for (uint ty = ty0; ty < max(ty1, ty0 + 1u); ty += stepY)
        {
            for (uint tx = tx0; tx < max(tx1, tx0 + 1u); tx += stepX)
            {
                float3 c = max(gSource.Load(int3(min(tx, fullW - 1u), min(ty, fullH - 1u), 0)).rgb, 0.0);
                if (gHdrProxy != 0 && gHdrTransfer != 0)
                    c = PqToLinear(c);  // see the calibrate mode: the meter must measure light, not code
                sum += dot(c, kLuma);
                taken++;
            }
        }

        gTarget[id.xy] = float4(taken > 0u ? sum / (float) taken : 0.0, 0.0, 0.0, 1.0);
        return;
    }

    if (gMode == 2)
    {
        uint srcW, srcH;
        gSource.GetDimensions(srcW, srcH);

        // Nothing to do when the sizes already agree.
        if (srcW == gWidth && srcH == gHeight)
        {
            gTarget[id.xy] = gSource.Load(int3(id.xy, 0));
            return;
        }

        // An exact area average rather than a bilinear tap.
        //
        // A bilinear sample of a shrinking image reads four texels and ignores the rest, so most of
        // the picture never reaches the model and what does is weighted by where the sample landed
        // rather than by how much of the pixel it covers. That is aliasing on the way in: the model
        // is shown a picture with detail that was never there and misses detail that was, and its
        // answer changes with sub-pixel motion for no reason in the scene.
        //
        // This integrates the source over the exact footprint of the destination pixel, which is the
        // correct box resample and costs a handful of loads at these ratios.
        //
        // hhkbble's, from the multi-pass PR against this fork.
        const float x0 = ((float) id.x * (float) srcW) / (float) gWidth;
        const float x1 = ((float) (id.x + 1) * (float) srcW) / (float) gWidth;
        const float y0 = ((float) id.y * (float) srcH) / (float) gHeight;
        const float y1 = ((float) (id.y + 1) * (float) srcH) / (float) gHeight;
        const float area = (x1 - x0) * (y1 - y0);

        const int i0 = (int) floor(x0);
        const int i1 = (int) ceil(x1) - 1;
        const int j0 = (int) floor(y0);
        const int j1 = (int) ceil(y1) - 1;

        float3 acc = 0.0;

        for (int j = j0; j <= j1; ++j)
        {
            const int jj = clamp(j, 0, (int) srcH - 1);
            const float aY = max(y0, (float) j);
            const float bY = min(y1, (float) j + 1.0);
            const float wy = max(bY - aY, 0.0);

            for (int i = i0; i <= i1; ++i)
            {
                const int ii = clamp(i, 0, (int) srcW - 1);
                const float aX = max(x0, (float) i);
                const float bX = min(x1, (float) i + 1.0);
                acc += gSource.Load(int3(ii, jj, 0)).rgb * (max(bX - aX, 0.0) * wy);
            }
        }

        const int acx = clamp((int) floor(((float) id.x + 0.5) * (float) srcW / (float) gWidth), 0, (int) srcW - 1);
        const int acy = clamp((int) floor(((float) id.y + 0.5) * (float) srcH / (float) gHeight), 0, (int) srcH - 1);

        gTarget[id.xy] = float4(acc / area, gSource.Load(int3(acx, acy, 0)).a);
        return;
    }

    if (gMode == 0)
    {
        float4 source = gSource.Load(int3(id.xy, 0));
        float3 frame = max(source.rgb, float3(0.0, 0.0, 0.0));

        // Kept so the resolve has the frame as it was, rather than having to reconstruct it.
        gKeep[id.xy] = float4(frame, source.a);

        // The float16 proxy. The frame's light -- PQ-decoded first if the swapchain carries PQ --
        // divided by the white point, and that is all: no knee, no sRGB, no ceiling. The whole point
        // of the float proxy is that the model is shown the highlights the SDR encode throws away,
        // so anything that compresses the range here would undo it. The resolve undoes the divide.
        if (gHdrProxy != 0)
        {
            float3 lin = gHdrTransfer != 0 ? PqToLinear(frame) : frame;
            gTarget[id.xy] = float4(lin / NormScale(), source.a);
            return;
        }

        // Some games hand DLSS a frame that has already been through their tonemapper. The game says
        // which in its own DLSS creation flags, and converting one that needs no conversion is pure
        // damage, so it goes through untouched.
        if (gPassthrough != 0)
        {
            gTarget[id.xy] = float4(frame, source.a);
            return;
        }

        // What the model is shown. Mode 2 -- the default -- scales the frame and encodes it, and that
        // is all: the game is going to tone map this picture later, so tone mapping it here as well
        // shows the model a doubly compressed image. Measured against Cyberpunk's own numbers, the
        // Reinhard proxy handed the model a scene value of 1.0 as 0.55 and 1.5 as 0.64 -- flat, dark,
        // and nothing like the finished frame it was trained on. The model then synthesised weakly,
        // judged tone on a picture that does not exist, and its answer had to be un-crushed on the way
        // back. Mode 0 keeps that old curve, mode 1 the fitted one.
        // A soft knee instead of a hard ceiling. Anything above 0.75 is rolled off rather than
        // clipped, so the model is never shown a field of flat white whose blown pixels flip between
        // frames -- unstable input is unstable output, and this is where a bright scene would produce
        // it. The resolve reproduces this exactly, so the two agree on what the frame's own proxy is.
        // The classic soft knee, or -- when the reversible proxy is on -- the unclipped Neutwo encode
        // that shows the model highlight gradation the knee throws away. Reached only when the frame
        // is not passthrough (handled and returned above), so NeutwoEncode never sees a tone-mapped
        // frame. Both are undone by the resolve: the knee approximately, Neutwo exactly.
        float3 normalized = frame / WhitePoint();
        float3 display;
        if (gReversibleMode == 0)
            display = SoftKnee(normalized);        // soft knee
        else if (gReversibleMode >= 3)
            display = HybridEncode(normalized);    // 3 hybrid composed, 4 hybrid replace -- same curve
        else
            display = NeutwoEncode(normalized);    // 1 composed, 2 replace -- both the full Neutwo proxy

        // The reversible proxy forces opaque alpha -- feature 18 expects an opaque colour input, and
        // the frame's own alpha is not part of what the model reads. The knee path keeps the frame's
        // alpha, so the default stays byte-identical.
        float alpha = gReversibleMode != 0 ? 1.0 : source.a;

        gTarget[id.xy] = float4(LinearToSrgb(display), alpha);
        return;
    }

    // Comparison, decided before anything is read, because side by side changes which part of the
    // frame this pixel is showing rather than just which version of it.
    //
    //   1  side by side  each half carries the whole frame, so both are squeezed horizontally
    //   2  wipe          one frame cut at the split, nothing resampled
    //
    // Neither needs the menu open to stay up. The wipe's split is a setting like any other; the menu
    // is only how you drag it.
    float2 cmpUv = uv;
    bool showOriginal = false;
    bool onDivider = false;
    bool outsideFrame = false;

    if (gCompareMode == 1)
    {
        showOriginal = (uv.x < 0.5) != (gCompareSwap != 0);

        // Each half is half as wide as the frame and just as tall, so the frame cannot fill it and
        // keep its shape. Stretching it to fit is what made both sides look squashed. Fitting it
        // properly leaves the halves letterboxed, which is the honest way round: a comparison that
        // changes the shape of what it is comparing is not showing you the picture.
        //
        // Zoom decides which is given up. At 1 the whole frame is there at its right proportions
        // with bars above and below; at 2 the half is filled and the sides are cropped away.
        float2 halfUv = float2(uv.x < 0.5 ? uv.x * 2.0 : (uv.x - 0.5) * 2.0, uv.y) - 0.5;
        cmpUv = float2(0.5 + halfUv.x / gCompareZoom, 0.5 + halfUv.y * 2.0 / gCompareZoom);

        outsideFrame = cmpUv.x < 0.0 || cmpUv.x > 1.0 || cmpUv.y < 0.0 || cmpUv.y > 1.0;
        onDivider = abs(uv.x - 0.5) < (1.0 / max(gWidth, 1u));
    }
    else if (gCompareMode == 2)
    {
        showOriginal = (uv.x < gCompareSplit) != (gCompareSwap != 0);
        onDivider = abs(uv.x - gCompareSplit) < (1.0 / max(gWidth, 1u));
    }

    // Sampled rather than loaded: when the model ran at a reduced resolution these are smaller than the
    // frame, and its edit is enlarged here while the frame underneath stays untouched.
    float4 proxySample = gSource.SampleLevel(gLinear, cmpUv, 0);
    float4 modelSample = gModel.SampleLevel(gLinear, cmpUv, 0);

    // Nothing was encoded on the way in, so nothing is decoded here either. The float16 proxy is
    // linear light already -- the sRGB decode would fold the highlights flat.
    float3 proxy, model;
    if (gHdrProxy != 0 || gPassthrough != 0)
    {
        proxy = proxySample.rgb;
        model = modelSample.rgb;
    }
    else
    {
        proxy = SrgbToLinear(proxySample.rgb);
        model = SrgbToLinear(modelSample.rgb);
    }

    // The model's own answer, kept before the matched-residual block below can rewrite `model`, so the
    // replace decode uses what the model returned rather than the residual reconstruction.
    float3 modelDirect = model;
    // The proxy as it was sampled, kept for the same reason: the residual branch below rewrites
    // `proxy`, and the relighting ratio's smoothing has to compare like with like.
    float3 proxyDirect = proxy;

    // What the model says about the light over this pixel's neighbourhood, and over the pixel alone.
    //
    // Computed once, here, because two different bounds below both need it and both were previously
    // making do with the pixel alone. Five taps of each picture, no encode: the expensive thing in
    // this pass has always been encoding neighbours, never fetching them.
    float gainSharp = 1.0;
    float gainSmooth = 1.0;
    {
        const float kGainFloor = 1.0 / 512.0;
        const float2 texel = 1.0 / float2(gWidth, gHeight);
        float mAcc = dot(modelDirect, kLuma);
        float pAcc = dot(proxyDirect, kLuma);
        [unroll]
        for (int nb = 0; nb < 4; ++nb)
        {
            const float2 off = float2(nb == 0 ? -1.0 : nb == 1 ? 1.0 : 0.0,
                                      nb == 2 ? -1.0 : nb == 3 ? 1.0 : 0.0) * texel;
            const float2 uvn = saturate(cmpUv + off);
            float3 pn = gSource.SampleLevel(gLinear, uvn, 0).rgb;
            float3 mn = gModel.SampleLevel(gLinear, uvn, 0).rgb;
            if (gHdrProxy == 0 && gPassthrough == 0)
            {
                pn = SrgbToLinear(pn);
                mn = SrgbToLinear(mn);
            }
            mAcc += dot(mn, kLuma);
            pAcc += dot(pn, kLuma);
        }
        gainSharp  = (dot(modelDirect, kLuma) + kGainFloor) / (dot(proxyDirect, kLuma) + kGainFloor);
        gainSmooth = (mAcc / 5.0 + kGainFloor) / (pAcc / 5.0 + kGainFloor);
    }
    float4 originalSample = gCompareMode == 1 ? gOriginal.SampleLevel(gLinear, cmpUv, 0)
                                              : gOriginal.Load(int3(id.xy, 0));

    // All three pictures have to share a scale before their luminances can be compared. The proxy and
    // the model come back from an sRGB decode, so they sit in 0..1 where 1 is the white point; the
    // frame is raw linear and runs well past that. Comparing them unnormalised is a real bug and it
    // reads exactly like the model has stopped adding detail: with the frame several times larger,
    // the shadow branch never fires, every pixel takes the highlight branch, and the clamp flattens
    // the result to a near-constant scale. Colour still moves, because that comes from the model's
    // own hue, which is what makes the failure so confusing to look at.
    const float normScale = NormScale();
    float3 originalRaw = originalSample.rgb;
    if (gHdrProxy != 0 && gHdrTransfer != 0)
        originalRaw = PqToLinear(max(originalRaw, 0.0));  // the frame's own light, in nits
    float3 original = originalRaw / normScale;

    float originalLuma = dot(original, kLuma);
    float proxyLuma = dot(proxy, kLuma);

    // Apply the model. Off outputs the frame as the upscaler produced it (clean) while the pass keeps
    // running -- so with Hold frame you can freeze a frame and toggle this to A/B the same frozen frame
    // with and without Neural Rendering. In passthrough the frame is already display-referred.
    if (gApplyModel == 0)
    {
        gTarget[id.xy] = float4(max(originalSample.rgb, 0.0), originalSample.a);
        return;
    }

    if (gDebugView == 1)
    {
        float3 dbg = proxy * gDebugScale * (gHdrProxy != 0 ? normScale : 1.0);
        if (gHdrProxy != 0 && gHdrTransfer != 0) dbg = LinearToPq(dbg);
        gTarget[id.xy] = float4(dbg, originalSample.a);
        return;
    }

    if (gDebugView == 2)
    {
        float3 dbg = model * gDebugScale * (gHdrProxy != 0 ? normScale : 1.0);
        if (gHdrProxy != 0 && gHdrTransfer != 0) dbg = LinearToPq(dbg);
        gTarget[id.xy] = float4(dbg, originalSample.a);
        return;
    }

    float3 edit = model - proxy;

    // Coring was tried here and removed: the per-frame churn's amplitude overlaps the real detail's,
    // so an amplitude threshold cannot separate them -- it only relocated the noise to the threshold.

    if (gDebugView == 3)
    {
        // Amplified and centred on grey, so both directions of the edit are visible at once.
        float3 shown = saturate(0.5 + edit * 20.0);
        float3 dbg = SrgbToLinear(shown) * gDebugScale * (gHdrProxy != 0 ? normScale : 1.0);
        if (gHdrProxy != 0 && gHdrTransfer != 0) dbg = LinearToPq(dbg);
        gTarget[id.xy] = float4(dbg, originalSample.a);
        return;
    }

    // There is no accumulator here, and this is where one used to be.
    //
    // The edit was averaged over time -- blended with its own reprojected history to keep the part
    // that stays and cancel the part that re-randomises. It was measured as a dead end twice, once
    // with a trained DLAA pass, for the same reason both times: the model re-decides its detail with
    // the framing, so an old answer does not belong to a new frame and reprojecting it only moves
    // where the disagreement lands. The composition is re-anchored to the model every frame instead,
    // which is what makes it steady.
    //
    // Said plainly because the comment that survived the removal did not say it, and a later reader
    // took it for a description of live code and planned on top of machinery that is not here.

    // Matched residual: put the two pictures being compared at the same resolution first.
    //
    // Classic hands the composition below a low-resolution `proxy` and a low-resolution `model`
    // against a full-resolution `original`. Those disagree by the downsample's blur as well as by the
    // model's edit, and the composition cannot tell the two apart -- it reads the blur as headroom
    // the frame has and the model never saw, which is a term that grows as the model's raster
    // shrinks. That is the resolution-dependent colour shift measured at 50%.
    //
    // Here the frame's own proxy is rebuilt at full resolution -- the encode is a pure function, so
    // SoftKnee reproduces it exactly -- and only the model's *difference* is carried up from small.
    // Both pictures handed to the composition are then full resolution and the only thing that came
    // from the reduced raster is the edit itself, which is what was wanted from it.
    //
    // The residual and its cube scaling are hhkbble's, from the multi-pass PR against this fork.
    //
    // Taken only when the model actually worked below the frame. At the same rate the arithmetic
    // collapses -- fullProxy + (model - proxy) is model, because proxy already is the frame's own
    // full-resolution proxy -- but only in exact arithmetic. The one this pass reads has been through
    // an sRGB encode, a texture, and a decode, while the one SoftKnee rebuilds has not, so the two
    // agree to within the proxy surface's precision rather than exactly. Skipping the path when there
    // is no residual to carry makes 100% bit-identical to Classic instead of nearly identical, which
    // is what lets this default to on: the shipped configuration cannot be changed by it at all.
    uint proxyW, proxyH;
    gSource.GetDimensions(proxyW, proxyH);
    const bool modelRanSmall = proxyW != gWidth || proxyH != gHeight;

    if (gTransfer == 1 && modelRanSmall)
    {
        // Saturated, because that is what the encode does and this has to reproduce it exactly.
        //
        // The encode writes LinearToSrgb(SoftKnee(frame / paperwhite)), and LinearToSrgb saturates
        // before it does anything else -- so the proxy the Classic path reads back is always inside
        // the unit cube. SoftKnee alone is not: it rolls luminance off above 0.75 but leaves a
        // channel free to sit above 1, and with a measured white point of 0.1 in a dark red interior
        // the red channel of anything lit is far above 1.
        //
        // CubeScaleResidual then computes (1 - P) / d to find how far the residual may travel before
        // leaving the cube. With P above 1 that numerator is negative, alpha comes out negative,
        // saturate(alpha) is zero, and the entire edit is discarded -- leaving the knee'd proxy as
        // the answer, which is darker than the frame everywhere the knee fired. That is the darker,
        // redder 50% picture: not the working scale, and not the residual idea, just a proxy that was
        // never clamped the way the one it stands in for is.
        // Rebuilt with the same curve the encode used, so the two agree on the frame's own proxy.
        // Passthrough must reproduce the encode's passthrough branch, which writes the frame raw --
        // SoftKnee returns it unchanged there, so both non-passthrough branches are gated behind the
        // same passthrough check the encode has. Without this, a reversible + matched-residual +
        // already-tone-mapped frame would Neutwo-compress a frame the encode left raw. Neutwo already
        // lands in [0,1), so it needs no saturate.
        float3 fullProxy = gHdrProxy != 0
                               ? original  // the float encode is a divide; the rebuild is the same divide
                               : gPassthrough != 0
                               ? saturate(original)
                               : (gReversibleMode == 0   ? saturate(SoftKnee(original))
                                  : gReversibleMode >= 3 ? HybridEncode(original)
                                                         : NeutwoEncode(original));
        proxy = fullProxy;
        proxyLuma = dot(proxy, kLuma);

        // At the same rate there is no residual to carry: the model's own picture is already at the
        // frame's resolution, and P + (m - p) collapses to m exactly.
        model = CubeScaleResidual(fullProxy, fullProxy + edit);
    }

    // The composition. The model's answer is not treated as a difference to add onto the frame -- it
    // is a complete picture in its own right, and it is brought back by rescaling it to sit where the
    // original's luminance says it should. Adding a difference is what let colour run away: nothing
    // bounded where the sum landed, so a warm subject could arrive green. Here both ends of every
    // blend are well-formed pictures, so everything between them is one too.
    //
    // Transfer 2 is the exception: it adds a difference, and carries the bound that path lacked.
    float modelLuma = dot(model, kLuma);
    float3 upgraded;

    if (modelLuma <= 1e-5)
    {
        // The model can return an empty frame for an input it cannot read. Rescaling that collapses
        // the picture to black, so the frame is handed back untouched instead. Ahead of the additive
        // branch as well: an empty model makes the edit the negated proxy, which subtracts the frame.
        upgraded = original;
    }
    else if (gTransfer == 2 && modelRanSmall)
    {
        // Native + edit. The frame's own pixels are the result and only the model's difference is
        // laid on top of them.
        //
        // The other two modes build every output pixel out of the model's raster, so below frame size
        // the whole picture arrives through the enlargement and geometry, text and edges the model
        // never touched come back softened. Enlarging the difference alone leaves everything the
        // model had no opinion about at native sharpness.
        //
        // Taken only where there is an enlargement, as the residual path is. At the same rate the
        // model's picture is already the frame's size, nothing is resampled on the way back, and
        // there is no softening for this to avoid.
        //
        // Technique from xenmods' DLSSNR-Cost-Scaler, Copyright (c) 2026 xen, MIT --
        // https://github.com/xenmods/DLSSNR-Cost-Scaler. Its CS_Resolve is the source of the additive
        // rule and of the guard's shape below; no code is copied.
        //
        // Saturated like the blend in the branch below. Detail strength above 1 is carried further
        // down as a power on the luminance ratio, so scaling the edit by it here as well spends it
        // twice.
        upgraded = max(original + edit * saturate(gTransferStrength), float3(0.0, 0.0, 0.0));

        // What bounds the sum. An addition says nothing about where the result lands, so the model's
        // verdict is read as a ratio on the pair it came from and the sum is held near it.
        //
        // 1/512 in the normalised space, the same value and the same reason as the ratio floor
        // further down: two dark pixels divide into an arbitrarily large number and the ratio is then
        // built from rounding noise, which crawls frame to frame. The term is in both halves, so a
        // bright pair is unaffected and the ratio falls to one as the pair goes black.
        const float kEditFloor = 1.0 / 512.0;
        const float editRatio = (modelLuma + kEditFloor) / (proxyLuma + kEditFloor);
        const float sumLuma = dot(upgraded, kLuma);

        if (sumLuma > 1e-5 && proxyLuma > 1e-5)
        {
            // The higher of two ceilings: 2.5x the pixel's own luminance, so a pixel the model
            // genuinely brightened is not pulled back, and the model's ratio with half again plus an
            // absolute 0.1, so a near-black pixel keeps an allowance rather than one that scales away
            // with it. One scalar over the whole triple -- a per-channel bound moves hue.
            const float targetLuma = originalLuma * editRatio;

            // Bounded by the guard the user set, and in both directions.
            //
            // This path adds a difference, so nothing about it says where the result lands -- and it
            // was bounded only upwards, only at a fixed 2.5x, and never by the control whose whole
            // job is to say how far the pass may move a pixel. On a photograph that is generous
            // enough not to show; on flat high-contrast panels it is not, and an interface put
            // through it comes back scorched: highlights driven to 2.5x, dark text driven below zero
            // and clamped flat, which together is the deep-fried look.
            //
            // The ratio path a few lines down has been two-sided since a measured collapse -- red
            // fell 57% while an upward-only bound sat watching it -- and there is no reason this path
            // should be the exception. The absolute term stays, so a near-black pixel keeps an
            // allowance rather than one that scales away with it. One scalar over the whole triple:
            // a per-channel bound moves hue.
            // The guard was doing two jobs with one number, and they pull in opposite directions.
            //
            // Raising it is how you ask for stronger relighting -- more room for the model's verdict
            // about how much light belongs somewhere. But the same number was also the only thing
            // bounding how far a *single* pixel may depart from its neighbours, and that one must
            // stay tight whatever the first is set to. At a guard of 8 this band is
            // [originalLuma/8, originalLuma*8], which is no bound at all: the raw sum passes through
            // with every per-pixel excursion the addition produced, and the ones that drive a channel
            // to nothing arrive as blown or black pixels wearing the texture's own colour. That is
            // down could not reach them -- the damage is already in `upgraded` before that ratio is
            // applied to it.
            //
            // So the two are separated. The guard sets how far the *neighbourhood's* light may move,
            // which is the relighting it was always meant to control. A single pixel may then depart
            // from that level by a fixed factor and no more, however high the guard goes. Detail is a
            // pixel differing from its neighbours by tens of percent; a blowout is one differing by
            // multiples, and only the second is refused.
            const float addGuard = max(gMaxRatio, 1.0);
            const float broadLuma = originalLuma * clamp(gainSmooth, 1.0 / addGuard, addGuard);
            const float kPixelBand = 1.6;
            const float maxLuma = max(broadLuma * kPixelBand, targetLuma * 0.25 + 0.02);
            const float minLuma = broadLuma / kPixelBand;


            if (sumLuma > maxLuma)
                upgraded *= maxLuma / sumLuma;
        }
    }
    else
    {
        float ratio;

        if (originalLuma < proxyLuma)
        {
            // Below what the proxy showed: the frame's own luminance is the target.
            ratio = originalLuma / max(proxyLuma, 1e-6);
        }
        else
        {
            // Above it, the difference is headroom the proxy could not represent -- brightness the
            // frame really has and the model never saw. It is handed back on top of the model's own
            // answer rather than scaled away, which is what kept highlights from being muted.
            ratio = (modelLuma + max(0.0, originalLuma - proxyLuma)) / modelLuma;
        }

        // Saturated deliberately. lerp past 1 extrapolates -- it walks beyond the target instead
        // of towards it -- and the target is the only well-formed picture in the pair, so the
        // guarantee stated above holds on [0,1] and nowhere else. Past it the channels spread apart
        // faster than luminance does, and the guard below cannot pull them back: it scales the whole
        // triple by one scalar, which corrects luminance while preserving the spread. A lit face at
        // strength 2 clips to white, and it starts to show just past 1.
        //
        // Strength above 1 is carried below instead, as an amplification of the luminance ratio,
        // which the guard does bound.
        upgraded = lerp(original, HueOkLab(model * ratio, model), saturate(gTransferStrength));
    }

    // Detail strength decides how much of the model's picture is reached at all; colour strength
    // decides whether its colour comes with it. At 0 the frame keeps the game's own hue exactly and
    // only its light carries the model's verdict; at 1 the model's colour arrives as well.
    float upgradedLuma = dot(upgraded, kLuma);

    // A ratio against a dark pixel is unbounded, and clamping it is not the same as taming it.
    //
    // In linear light divided by paper white a shadowed pixel sits around a thousandth, so a tiny
    // absolute edit from the model becomes an enormous ratio, hits the clamp, and doubles that
    // pixel's brightness. The next frame it lands slightly differently and the pixel drops back.
    // That is the boiling: patches of lighter colour crawling over otherwise still geometry, worst
    // where the picture is darkest.
    //
    // Adding the same floor above and below leaves bright pixels alone -- where luminance is far
    // larger than the floor the term vanishes -- while making the ratio fall smoothly to one as
    // luminance approaches zero. No edit at all is the right answer for a pixel with no light in it.
    const float kRatioFloor = 1.0 / 512.0;
    float lumaRatio = (upgradedLuma + kRatioFloor) / (originalLuma + kRatioFloor);

    // Take the model's broad relighting and leave its per-pixel disagreement behind.
    //
    // Everything above rebuilds the frame as its own pixel times this one number. Where the model and
    // the frame agree the number is 1 and nothing happens, which is why flat surfaces are always
    // clean. On detailed content the model's answer differs sharply from one pixel to the next --
    // that difference is the enhancement -- so the number is large and varies fast, and the highlight
    // guard is the only thing holding it. Raising the guard therefore lets more of the variation
    // through, and it lands as blown and black pixels carrying whatever colour the texture had. That
    // is why the artifacts scale with the guard instead of being clipped by it, why colour strength 0
    // does not touch them -- a scalar cannot move hue -- and why they sit only on detail.
    //
    // A ratio is the wrong thing to carry at full spatial frequency. What the model has a real
    // opinion about at this scale is how much light belongs here, not which individual pixel is
    // brighter than its neighbour; the frame already knows that and is about to be multiplied by
    // this. So the ratio's high-frequency component is replaced with the neighbourhood's, leaving the
    // broad verdict intact. Simulated against a surface carrying both: the pixel-to-pixel speckle
    // falls about fourfold while the range the relighting spans is untouched, and the range still
    // grows with the guard, which is the point -- a high guard becomes strong smooth relighting
    // rather than speckle.
    //
    // Off by default, so the shipped configuration is unchanged and this is something to turn up
    // when a raised guard is wanted.
    if (gRatioSmooth > 0.0)
    {
        // The pixel's own gain against the neighbourhood's. Their quotient is exactly the
        // high-frequency part being removed, so where the gain is already smooth this is the
        // identity.
        const float corrected = lumaRatio * (gainSmooth / max(gainSharp, 1e-6));
        lumaRatio = lerp(lumaRatio, corrected, saturate(gRatioSmooth));
    }

    // Where detail strength above 1 goes.
    //
    // Raising the ratio to a power rather than extending the blend keeps every property that
    // matters: it cannot go negative, it leaves a pixel the model did not change alone -- one to any
    // power is one -- and it moves brightening and darkening by the same factor in opposite
    // directions, so it does not favour either. Most importantly the result is still a ratio, so the
    // guard below binds it, which is exactly what the extrapolated blend escaped.
    //
    // The correction further down divides by the unamplified ratio, so the composed picture ends up
    // at the original's luminance times the bounded ratio either way. Strength 1 leaves this the
    // identity and the pass bit-identical to before.
    const float amplified = pow(max(lumaRatio, 1e-6), 1.0 + max(gTransferStrength - 1.0, 0.0));

    // The guard binds the composed picture, not only the luminance-only end of the blend below.
    //
    // It used to bind `original * lumaRatio` and nothing else -- the colour-strength-zero end. At
    // colour strength 1, which is the default, that end is never reached, so the guard did nothing
    // at all and whatever the model returned was handed back unbounded. Where the soft knee fires
    // that stays hidden, because the headroom term above makes the frame's own brightness dominate
    // the result. Where the knee does not fire -- any dark scene -- the ratio degenerates to one,
    // the composition reduces to the model's own picture, and every frame the model re-decided
    // arrived whole. That is the flicker reported in Nioh 3, and it worsened with paper white
    // because the model's answer is multiplied by it on the way out.
    //
    // Two-sided, because the failure measured there was a collapse and not a runaway: red fell 57%
    // while an upward-only bound sat watching it. The control's own help text said darkening was
    // deliberately uncapped; that was decided before there was a case against it.
    //
    // One scalar, taken from luminance, applied to the whole triple. A per-channel bound is a hue
    // distorter -- on a saturated pixel the smallest channel reaches the bound first, so an
    // achromatic edit lands as a colour shift.
    const float guard = max(gMaxRatio, 1.0);

    // Relighting cannot invent light, so the room to brighten shrinks toward none as a pixel
    // approaches black.
    //
    // The composed pixel is the frame's own pixel times this number. A scalar cannot move hue, so
    // whatever tint the texture already had is multiplied along with everything else -- and a dark
    // pixel's tint is the most saturated thing about it. The platform ring is RGB (2, 4, 20): almost
    // invisible, and a chroma of 0.9. At a guard of 8 that becomes (15, 25, 67), which is a glaring
    // blue block, and it is the game's own colour every step of the way. That is the black-to-blue
    // fault, and it is why the colour bound could not touch it -- the debug view shows that bound
    // fully engaged, red, on exactly these pixels. There was never a wrong colour to hold back.
    //
    // Below the floor there is also nothing to relight *from*: at a couple of counts in 8-bit the
    // pixel's own value is mostly quantisation, so an eightfold lift amplifies the transport rather
    // than the model's verdict. Measured on the values above, this leaves a shadowed pixel of
    // (20, 26, 44) with 6.4x of its 8x and anything at mid shadow or brighter completely untouched,
    // while the ring keeps 1.2x and stays where it belongs.
    //
    // Only upward. Darkening a near-black pixel further is harmless -- it stays black -- and
    // clamping that side would be a second bound nobody asked for.
    const float lift = lerp(1.0, guard, smoothstep(0.0, 8.0 * kRatioFloor, originalLuma));

    // And the room to darken shrinks toward none as a pixel approaches white, for the same reason
    // read the other way round.
    //
    // The guard is symmetric, so raising it to allow stronger relighting allows equally strong
    // *darkening* -- and a light source is exactly where that shows. At a guard of 1 the clamp is
    // [1,1] and a lamp comes out white; at 3 the same lamp is allowed down to a third of itself and
    // visibly dims, which reads as the value inverting. Detail strength makes it worse rather than
    // better, because it raises the ratio to a power: at 2.0 a ratio of 0.85 becomes 0.72.
    //
    // The help text for this control has always said that a detail pass has no business restyling a
    // light source. Nothing enforced it. Now the floor rises to 1 as the pixel reaches paper white,
    // so a highlight cannot be pulled down however high the guard goes, while a bright wall at 0.75
    // and everything below it is bounded exactly as before.
    const float drop = lerp(1.0 / guard, 1.0, smoothstep(0.6, 1.1, originalLuma));
    float boundedRatio = clamp(amplified, drop, lift);

    // Exactly one while the ratio is already inside the guard, so a frame that never needed bounding
    // is untouched rather than rounded, and strength zero stays bit-identical.
    upgraded *= boundedRatio / max(lumaRatio, 1e-6);

    // Both ends of the blend now sit inside the same guard, so neither needs a second clamp.
    //
    // Colour strength 0..1 blends toward the model's colour, as before. Above 1 it OVER-SATURATES: the
    // blend caps at the model's colour (min), then the excess scales CHROMA in OkLab -- L and hue kept,
    // only a and b grow -- and ClampAp1 below pulls anything past the gamut back by desaturating toward
    // neutral, NOT by clipping channels. So an over-driven colour rolls off at the gamut boundary
    // (maximally vivid but still a real colour with detail) instead of flattening into a blown peak.
    // At strength 1 the boost is the identity, so <=1 is bit-identical to before.
    const float3 lumaOnly = original * boundedRatio;

    // How far taking the model's colour would move this pixel's balance, and how much of it to take.
    //
    // Both ends of the blend carry the same luminance -- the guard above bound them together -- so
    // what separates them is chroma and nothing else. On a flat surface the model agrees with the
    // frame about hue and that separation is small, which is where the colour transfer earns its
    // keep. On an edge the model's answer differs most, because an edge is precisely what it was
    // asked to re-decide, and taking its hue whole puts one colour on one side of the edge and its
    // complement on the other. That is the blue and orange fringing, and at strength 1 -- the
    // default -- there was nothing between the disagreement and the screen.
    //
    // Measured on a game frame: the pass moved colour balance three to five times more at edges than
    // on flat pixels, and every bit of it came from this blend. Under it the ringing is gone with the
    // luminance detail untouched, because that lives in boundedRatio and not here.
    // How far the composed colour sits from the frame's own, at matched luminance -- the guard above
    // bound the two together -- so this is a pure colour difference and nothing to do with brightness.
    const float3 colourDev = upgraded - lumaOnly;
    const float chromaSwing = length(colourDev) / max(dot(lumaOnly, kLuma), 1e-4);

    // Bound that difference rather than switching it off. The switch was backwards.
    //
    // It faded the model's colour to nothing as disagreement grew: all of it below a swing of 0.05,
    // none at all above 0.25. The reasoning was sound as far as it went -- the model disagrees most
    // at edges, and taking its hue whole there put one colour on one side of an edge and its
    // complement on the other. What it missed is that a large disagreement is also exactly what a
    // real colour correction looks like. So the rule discarded the model's verdict precisely where it
    // had one, and the bigger the correction the more completely it went.
    //
    // Measured on a ceiling strip light: the game's own glow is blue, the model corrects it to white,
    // the swing is 1.82, and the gate handed back the game's blue untouched. Composing made that
    // light bluer than not composing at all, which is the opposite of what the pass is for.
    //
    // A bound keeps what the gate was protecting and drops what it was breaking. A correction of
    // ordinary size passes whole, so the light comes back white. Fringing, which is larger still, is
    // capped -- and capped without inverting, because the direction is kept and only the length is
    // limited. More disagreement can no longer mean less colour, only the same amount of it.
    const float colourBand = max(gColourTrust, 0.0);
    const float colourAllow = colourBand <= 0.0
                                  ? 0.0
                                  : min(1.0, colourBand / max(chromaSwing, 1e-6));

    // Below a few code values the model's hue is quantisation, not information.
    //
    // The answer crosses as 8-bit. In a near-black region that leaves two or three levels, so the
    // *hue* of such a pixel is decided by which channel happened to round up. The composition then
    // reads that hue as the model's verdict and amplifies it: the ratio is (modelLuma + headroom) /
    // modelLuma, which on a near-black pixel runs to a hundred and more, and the guard that follows
    // bounds luminance only -- deliberately, one scalar over the whole triple, so that a bound cannot
    // shift hue. The result is that the meaningless hue is preserved exactly and lifted to the
    // frame's own brightness.
    //
    // Simulated on this path with real 8-bit inputs: a model pixel of (0,0,1) composes to
    // (0, 0, 0.694) -- one least significant bit of blue becomes a saturated blue pixel -- and
    // (1,0,1) composes to (0.176, 0, 0.176), which is magenta. That is the dead blue, red and
    // magenta speckle, it is why it sits only on dark detailed content, and it is why it disappears
    // when the composition is bypassed: presented directly, (0,0,1) is simply a black pixel. It reads
    // as blocks rather than speckle because above a working scale of 1 the model's raster is filtered
    // down, so neighbouring output pixels share the same few codes.
    //
    // So the model's colour is trusted in proportion to how much light it actually reported, and
    // below the noise floor the frame's own hue is used instead. Measured against the same
    // simulation: codes 0 to 5 collapse to the frame's colour and codes of 12 and above come out bit
    // identical, so this cannot touch content the model had a real opinion about.
    //
    // Only where the transport quantises. A float16 proxy has no such floor and needs no guard.
    // Raised from 6/255 to 25/255, from the range the fault actually occupies.
    //
    // Six was chosen as "a couple of code values", which is where hue is purely quantisation. But the
    // pixels still arriving blue after the pass chain was widened to sixteen bits sit at a luminance
    // of 6 to 18 in 255 -- median 13 -- and only a seventh of them are anywhere the colour bound
    // engages, so on the rest the model's hue was passing through untouched. Below about 25 there are
    // too few levels for the model to have a colour opinion worth more than the frame's own, and the
    // frame's is the game's actual render rather than something reconstructed from three or four
    // codes.
    //
    // It tapers rather than switching, so a pixel at the median is damped to about 40% and one at 32
    // and above is untouched entirely.
    const float kQuantFloor = 0.0097;  // SrgbToLinear(25/255), in the same normalised units as model
    const float hueTrust = gHdrProxy != 0 ? 1.0
                                          : smoothstep(0.0, kQuantFloor, dot(modelDirect, kLuma));

    // What the colour bound is doing, seen directly.
    //
    // Green is the model's colour passing whole, red is it being held back, so a fault can be put on
    // one side or the other of this line without guessing: if a wrong colour shows green here the
    // bound is not engaging on it and the fault is upstream in `upgraded`; if it shows red then the
    // bound is engaging and the colour is coming from `lumaOnly`, which is the frame's own hue times
    // one scalar and therefore a luminance problem rather than a colour one.
    if (gDebugView == 4)
    {
        const float a = saturate(colourAllow);
        gTarget[id.xy] = float4(float3(1.0 - a, a, 0.0) * WhitePoint(), originalSample.a);
        return;
    }

    // The composed colour before the bound is applied, so the two can be compared frame by frame.
    if (gDebugView == 5)
    {
        float3 dbg = upgraded * gDebugScale * (gHdrProxy != 0 ? normScale : 1.0);
        if (gHdrProxy != 0 && gHdrTransfer != 0) dbg = LinearToPq(dbg);
        gTarget[id.xy] = float4(max(dbg, 0.0), originalSample.a);
        return;
    }

    float3 result = lerp(lumaOnly, lumaOnly + colourDev * colourAllow,
                         min(gColourStrength, 1.0) * hueTrust);


    if (gColourStrength > 1.0)
        result = ClampAp1(FromOkLab(float3(1.0, gColourStrength, gColourStrength) * ToOkLab(max(result, 0.0))));

    // Replace mode: the model's answer IS the picture, decoded through Neutwo's exact inverse, with
    // NONE of the composition above -- no ratio, no highlight guard, no palette blend. This is the
    // RenoDX reversible-bridge behaviour and the second half of the A/B: composed vs pure model. On a
    // passthrough frame the model already worked in the frame's own space, so it is taken directly.
    if (gReversibleMode == 2)
        result = (gPassthrough != 0 || gHdrProxy != 0) ? modelDirect : NeutwoDecode(modelDirect);
    else if (gReversibleMode == 4)
        result = (gPassthrough != 0 || gHdrProxy != 0) ? modelDirect : HybridDecode(modelDirect);

    // Back out of the normalised space the composition worked in, and back into the swapchain's own
    // transfer. A PQ frame's code is not linear light; writing the composition's linear answer
    // straight to it would darken the whole picture into the bottom of the curve.
    result *= normScale;
    if (gHdrProxy != 0 && gHdrTransfer != 0)
        result = LinearToPq(max(result, 0.0));

    // The side being shown untouched takes the frame as it arrived, past every step above -- code
    // for code, transfer included.
    if (showOriginal)
        result = originalSample.rgb;

    // The letterbox. The sampler clamps rather than wrapping, so without this the bars would be the
    // frame's edge row smeared down the screen.
    if (outsideFrame)
        result = float3(0.0, 0.0, 0.0);

    // A hairline so the two sides are never mistaken for one picture.
    if (onDivider)
    {
        float3 white = float3(WhitePoint(), WhitePoint(), WhitePoint());
        if (gHdrProxy != 0 && gHdrTransfer != 0)
            white = LinearToPq(float3(WhitePoint() * kPqPaperWhite, WhitePoint() * kPqPaperWhite,
                                      WhitePoint() * kPqPaperWhite));
        result = white;
    }

    gTarget[id.xy] = float4(max(result, float3(0.0, 0.0, 0.0)), originalSample.a);
}
