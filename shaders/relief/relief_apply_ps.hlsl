// Smart parallax, Apply (once per displayed frame, output resolution): relief shading and position-based parallax.
// Research note 2.1 and 2.5. Reads the game-resolution height H and the half-size protection gates (bilinear, so they are upsampled
// smoothly; both are already edge-aware: H comes from the guided-filter detail layer and P is zero across geometry
// edges, HUD and flat regions) and modulates luminance only, in linear light:
//   N      = normalize(-bump * dH/dx, -bump * dH/dy, 1)        (dH from 4 diagonal bilinear taps, game px)
//   shade  = tanh(-bump * grad(H) . L.xy)                       (0 on flat areas; slopes facing L brighten, others darken)
//   cavity = max(0, mean4(H) - H)                              (pixels lower than their neighbourhood darken)
//   delta  = maxDelta * tanh(strength * (shading*Ks*shade - cavity*Kc*cavity) / maxDelta) * P * fade
//   out    = srgb(linear(src) * (1 + delta))                   (all three channels scaled: chroma kept)
// maxDelta is 15 % at any strength (soft clip), so the change never exceeds about +-15 % of the pixel's
// linear luminance. Where delta is 0 (protected, letterbox, Split left half) the source pixel is returned unchanged,
// bit for bit. A generated frame blends heights from both real frames at their motion-warped positions.
// The stricter HUD and edge gates from either source protect the interpolated output.
#define RELIEF_NO_ANALYSE_CB
#include "relief/relief_common.hlsli"

cbuffer ReliefApplyCB : register(b0)
{
    float4 aContent;    // content rect in dst px: x, y, width, height
    float2 aGameSize;   // analysed size (game px)
    float2 aGameTexel;  // 1 / aGameSize
    float4 aLight;      // xyz: unit light vector in screen space (x right, y DOWN, z towards the viewer)
    float  aShadeGain;  // strength * shading * Ks
    float  aCavityGain; // strength * cavity * Kc
    float  aMaxDelta;
    float  aBump;
    uint   aDebug;      // ReliefDebugView value
    float  aSplitX;     // dst x of the Split divider (Split view only)
    float  aPhase;      // 1 on the newest real frame
    uint   aSrcLinear;  // 1: the SRV returns linear values (sRGB view or float format): no conversion
    float2 aCellInv;    // 1 / cell size (game px)
    uint2  aCells;
    float  aTapPx;      // height-tap distance in game px (scales with the detail size: borders bevel, grain does not)
    float  aParallaxPx; // strength times the maximum output-pixel offset, zero for Shading
    float2 aOutTexel;   // 1 / output texture size
    uint   aProtectCharacters; // Motion depth independent-motion mask
    uint   aHasMotion;
    uint   aHasHistory;
    uint   aHasDepthHistory;
    uint   aFilteredParallax;
    uint   aHasSelection;
};

Texture2D<float4>      srcTex   : register(t0);
Texture2D<float>       hTex     : register(t1);
Texture2D<float4>      pTex     : register(t2); // half-size gates (hud, edge * tex, changed, SigmaEnc(sigma))
ByteAddressBuffer      state    : register(t3);
Texture2DArray<float4> flowTex  : register(t4);
#ifdef RELIEF_MOTION_DEPTH
Texture2D<float4> motionDepth  : register(t5);
Texture2D<float4> prevDepth    : register(t8);
#endif
Texture2D<float>       prevH   : register(t6);
Texture2D<float4>      prevP   : register(t7);
#if defined(RELIEF_PAIR) && defined(RELIEF_PARALLAX)
Texture2D<float4>      selectionTex : register(t9); // phase motion (xy), frame share and confidence from FG
#endif
SamplerState           linClamp : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

#define DBG_NONE 0
#define DBG_HEIGHT 1
#define DBG_NORMALS 2
#define DBG_PROTECTION 3
#define DBG_OFFSET 4
#define DBG_DEPTH 5
#define DBG_SPLIT 6

float4 ReadProtection(Texture2D<float4> gates, float2 uv)
{
    float4 p = gates.SampleLevel(linClamp, uv, 0);
    const float4 hud = gates.GatherRed(linClamp, uv);
    p.x = min(min(hud.x, hud.y), min(hud.z, hud.w));
    return p;
}

float HeightAt(float2 current, float2 previous, float2 offset, bool generated)
{
    float h = hTex.SampleLevel(linClamp, current + offset, 0);
#ifdef RELIEF_PAIR
    if (generated) h = lerp(prevH.SampleLevel(linClamp, previous + offset, 0), h, aPhase);
#endif
    return h;
}

#ifdef RELIEF_PAIR
float FlowProtection(int2 cell, float4 backward)
{
    const int2 hi = int2(aCells) - 1;
    const float2 left  = flowTex.Load(int4(max(cell - int2(1, 0), 0), 1, 0)).xy;
    const float2 right = flowTex.Load(int4(min(cell + int2(1, 0), hi), 1, 0)).xy;
    const float2 up    = flowTex.Load(int4(max(cell - int2(0, 1), 0), 1, 0)).xy;
    const float2 down  = flowTex.Load(int4(min(cell + int2(0, 1), hi), 1, 0)).xy;
    const float discontinuity = max(max(length(backward.xy - left), length(backward.xy - right)),
                                    max(length(backward.xy - up), length(backward.xy - down)));
    return smoothstep(0.1, 0.65, backward.z) *
           (1.0 - smoothstep(0.22, 0.65, backward.w)) *
           (1.0 - smoothstep(1.5, 5.0, discontinuity));
}
#endif

#ifdef RELIEF_PARALLAX
static const float kWarpSurfaceMin = 0.9;

// Fetch towards the point from which a raised texel would have projected. For an assumed facing wall
// that is towards the centre, and for an assumed floor it is down, away from the estimated horizon.
bool WarpStopped(Texture2D<float4> gates, float2 uv)
{
    return any(gates.GatherRed(linClamp, uv) < 0.75) || any(gates.GatherGreen(linClamp, uv) < kWarpSurfaceMin);
}

float2 ParallaxOffset(float2 local, float2 uv, float2 prevUv, bool generated, float h, float2 gradient,
                      float protection, float depthGain, float groundHint)
{
    const float2 halfSize = 0.5 * aContent.zw;
    const float2 fromCentre = local - halfSize;
    const float2 wall = -fromCentre / length(halfSize);
    const float groundY = saturate(fromCentre.y / halfSize.y);
    const float groundBlend = saturate(smoothstep(0.0, 0.35, groundY) + groundHint);
    const float2 ray = lerp(wall, float2(0.0, groundY), groundBlend);
    // Give coherent surfaces more displacement at the default cap. Taper the gain towards
    // the maximum setting, where a larger position-driven offset can make textures swim.
    const float gain = lerp(1.2, 1.14, saturate(aParallaxPx - 2.0));
    // Limit the rate of change along the ray so the extra displacement does not fold fine detail.
    const float slope = abs(dot(gradient, ray));
    const float amplitude = min(gain * aParallaxPx * depthGain, 0.25 / max(slope, 1e-4));
    // Keep strong or poorly resolved height gradients from dragging adjacent detail.
    const float fineDetail = 1.0 - 0.55 * smoothstep(0.04, 0.25, length(gradient));
    const float displacement = clamp(amplitude * h * protection * fineDetail, -aParallaxPx, aParallaxPx);
    const float2 offset = ray * displacement;
    float2 accepted = 0.0;
    [unroll] for (int step = 1; step <= 3; ++step) {
        const float2 candidate = offset * (float(step) / 3.0);
        const float2 sampleLocal = local + candidate;
        if (any(sampleLocal < 0.5) || any(sampleLocal > aContent.zw - 0.5)) break;
        const float2 sampleUv = uv + candidate / aContent.zw;
        // Check every tap in the gate's bilinear footprint, including the HUD footprint, before
        // accepting the step. At the three-pixel cap each step is at most one output pixel.
        if (WarpStopped(pTex, sampleUv)) break;
#ifdef RELIEF_PAIR
        if (generated) {
            if (WarpStopped(prevP, prevUv + candidate / aContent.zw)) break;
            // HUD stays in screen space even if the motion estimate at its boundary follows the game.
            const float2 screenUv = sampleLocal / aContent.zw;
            if (any(pTex.GatherRed(linClamp, screenUv) < 0.75) || any(prevP.GatherRed(linClamp, screenUv) < 0.75))
                break;
        }
#endif
        accepted = candidate;
    }
    return accepted;
}

float3 SampleParallax(float3 source, float2 pixel, float2 offset)
{
    const float3 shifted = srcTex.SampleLevel(linClamp, (pixel + offset) * aOutTexel, 0).rgb;
    if (aFilteredParallax == 0) return shifted;

    // Shift the lower-frequency colour variation along the accepted segment and retain fine detail from
    // the frame generator. Every tap stays between the original and protected destination samples.
    const float3 nearSource = srcTex.SampleLevel(linClamp, (pixel + 0.45 * offset) * aOutTexel, 0).rgb;
    const float3 nearShifted = srcTex.SampleLevel(linClamp, (pixel + 0.55 * offset) * aOutTexel, 0).rgb;
    const float3 difference = 0.5 * ((shifted - nearSource) + (nearShifted - source));
    return clamp(source + difference, min(source, shifted), max(source, shifted));
}
#ifdef RELIEF_PAIR
// Use the generator's smooth-region motion for the height lookup where its neighbouring hypotheses agree.
// The original flow and the most protective masks still control where lighting and displacement can apply.
void AlignHeightToSelection(float2 screen, inout float2 currentUv, inout float2 previousUv)
{
    const int2 last = int2(ceil(0.5 * aGameSize)) - 1;
    const float2 grid = 0.5 * (screen - 1.0);
    const int2 base = int2(floor(grid));
    const float2 f = frac(grid);
    const float4 topLeft = selectionTex.Load(int3(clamp(base, 0, last), 0));
    const float4 topRight = selectionTex.Load(int3(clamp(base + int2(1, 0), 0, last), 0));
    const float4 bottomLeft = selectionTex.Load(int3(clamp(base + int2(0, 1), 0, last), 0));
    const float4 bottomRight = selectionTex.Load(int3(clamp(base + 1, 0, last), 0));
    const float4 selected = lerp(lerp(topLeft, topRight, f.x), lerp(bottomLeft, bottomRight, f.x), f.y);
    const float spread = max(max(length(topLeft.xy - selected.xy), length(topRight.xy - selected.xy)),
                             max(length(bottomLeft.xy - selected.xy), length(bottomRight.xy - selected.xy)));
    const float weight = smoothstep(0.3, 0.7, selected.w) * (1.0 - smoothstep(0.5, 1.0, spread));
    const float2 current = screen + (1.0 - aPhase) * selected.xy;
    const float2 previous = screen - aPhase * selected.xy;
    if (all(current >= 0.5) && all(current <= aGameSize - 0.5) &&
        all(previous >= 0.5) && all(previous <= aGameSize - 0.5)) {
        currentUv = lerp(currentUv, current * aGameTexel, weight);
        previousUv = lerp(previousUv, previous * aGameTexel, weight);
    }
}
#endif
#endif

float3 ScaleSrgb(float3 colour, float gain)
{
    // In the power-law part of sRGB, scaling in linear light is equivalent to multiplying
    // the encoded channel plus its bias by gain^(1/2.4). Only a knee crossing needs decoding.
    const float3 low = colour * gain;
    const float3 high = (colour + 0.055) * pow(abs(gain), 1.0 / 2.4) - 0.055;
    const bool3 inHigh = colour > 0.04045;
    float3 scaled = inHigh ? high : low;
    [branch] if (any(inHigh != (scaled > 0.04045)))
        scaled = LinearToSrgb3(saturate(SrgbToLinear3(colour) * gain));
    return saturate(scaled);
}

float4 main(VsOut i) : SV_Target
{
    const int2 px = int2(i.pos.xy);
    const float4 src = srcTex.Load(int3(px, 0));
    const float2 local = i.pos.xy - aContent.xy;
    if (any(local < 0.0) || any(local >= aContent.zw)) return src; // letterbox
    if (aDebug == DBG_SPLIT && abs(i.pos.x - aSplitX) < 1.0) return float4(0.5, 0.5, 0.5, src.a); // divider
    if (aDebug == DBG_SPLIT && i.pos.x < aSplitX) return src;

    float2 g = local / aContent.zw * aGameSize; // game px
    float2 previous = g;
    float motionProtection = 1.0;
#ifdef RELIEF_PAIR
    const bool generated = aHasHistory != 0 && aPhase < 1.0;
    if (aHasMotion != 0) {
        const int2 hi = int2(aCells) - 1;
        const int2 cell = min(int2(g * aCellInv), hi);
        const float4 backward = flowTex.Load(int4(cell, 1, 0));
        const float backwardProtection = FlowProtection(cell, backward);
        motionProtection = backwardProtection;
        if (generated) {
            const float4 forward = flowTex.Load(int4(cell, 0, 0));
            previous -= aPhase * forward.xy;
            g -= (1.0 - aPhase) * backward.xy;
            const float forwardProtection = smoothstep(0.1, 0.65, forward.z) * (1.0 - smoothstep(0.22, 0.65, forward.w));
            // The older frame's confidence matters less as the generated frame approaches the newest real one.
            motionProtection = lerp(min(forwardProtection, backwardProtection), backwardProtection, aPhase);
        } else {
            previous += backward.xy;
        }
        if (any(g < 0.0) || any(g >= aGameSize) || any(previous < 0.0) || any(previous >= aGameSize))
            motionProtection = 0.0;
    }
#else
    const bool generated = false;
#endif
    const float2 uv = g * aGameTexel;
    const float2 prevUv = previous * aGameTexel;
    const float2 t = aGameTexel * aTapPx;
    float4 prot = ReadProtection(pTex, uv);
#ifdef RELIEF_PAIR
    if (generated || (aHasHistory != 0 && aHasMotion != 0 && motionProtection > 0.0)) {
        const float2 old = ReadProtection(prevP, prevUv).xy;
        const float2 recovering = min(prot.xy, lerp(prot.xy, old, 0.35 * motionProtection));
        if (generated) {
            // Keep the stricter HUD mask. Let the surface strength approach the newest frame's strength.
            prot.x = min(old.x, prot.x);
            prot.y = lerp(min(old.y, prot.y), recovering.y, aPhase);
        } else {
            prot.xy = recovering;
        }
    }
    if (generated && aHasMotion != 0) {
        const float2 screenUv = local / aContent.zw;
        prot.x = min(prot.x, min(ReadProtection(pTex, screenUv).x, ReadProtection(prevP, screenUv).x));
    }
#endif
    prot.w = prot.x * prot.y * motionProtection;
    const float fade = asfloat(state.Load(RST_FADE));
    float depthGain = 1.0, groundHint = 0.0, depthProtection = 1.0;
#ifdef RELIEF_MOTION_DEPTH
    // Small sampling shifts are particularly visible on highlights. Roll off the depth boost and
    // displacement across bright tones so changing flow does not make shiny detail shimmer.
    const float luminance = dot(src.rgb, kRec709);
    const float bright = aSrcLinear ? smoothstep(0.45, 0.85, luminance) : smoothstep(0.70, 0.93, luminance);
    const float2 cellUv = g * aCellInv / float2(aCells);
    float4 depth = motionDepth.SampleLevel(linClamp, cellUv, 0);
    if (generated && aHasDepthHistory != 0) {
        const float4 old = prevDepth.SampleLevel(linClamp, previous * aCellInv / float2(aCells), 0);
        depth.xy = lerp(old.xy, depth.xy, aPhase);
        depth.z = max(old.z, depth.z);
    }
    const float4 nearby = motionDepth.GatherRed(linClamp, cellUv);
    const float boundary = max(max(nearby.x, nearby.y), max(nearby.z, nearby.w)) -
                           min(min(nearby.x, nearby.y), min(nearby.z, nearby.w));
    depthProtection = 1.0 - depth.y * smoothstep(0.15, 0.65, boundary);
    if (aProtectCharacters != 0) {
        const float4 moving = motionDepth.GatherBlue(linClamp, cellUv);
        depthProtection *= 1.0 - max(depth.z, max(max(moving.x, moving.y), max(moving.z, moving.w)));
    }
    const float gx = 0.5 * (nearby.y + nearby.z - nearby.x - nearby.w);
    const float gy = 0.5 * (nearby.x + nearby.y - nearby.z - nearby.w);
    groundHint = 0.3 * (1.0 - bright) * depth.y * smoothstep(0.025, 0.2, abs(gy) - abs(gx)) *
                 smoothstep(0.0, 0.2, local.y / aContent.w - 0.5);
    depthGain = (1.0 + 0.35 * depth.x * depth.y * (1.0 - bright)) * (1.0 - 0.5 * bright);
    if (aDebug == DBG_DEPTH)
        return float4(lerp((0.5 + 0.5 * depth.x * depth.y).xxx, float3(1.0, 0.0, 0.0), 1.0 - depthProtection), src.a);
#else
    if (aDebug == DBG_DEPTH) return float4(0.5, 0.5, 0.5, src.a);
#endif
    prot.w *= depthProtection * smoothstep(0.10, 0.55, prot.y);
    // Protected, faded-out or unprocessed pixels skip the height taps (normal output only; debug views need them).
    if ((aDebug == DBG_NONE || aDebug == DBG_SPLIT) && prot.w * fade <= 0.0) return src;
    float2 heightUv = uv, prevHeightUv = prevUv;
#if defined(RELIEF_PAIR) && defined(RELIEF_PARALLAX)
    if (generated && aHasSelection != 0 && asfloat(state.Load(RST_CUT)) < 0.5)
        AlignHeightToSelection(local / aContent.zw * aGameSize, heightUv, prevHeightUv);
#endif
    const float h0 = HeightAt(heightUv, prevHeightUv, 0.0, generated);
    const float4 neighbourhood = smoothstep(0.15, 0.65, pTex.GatherGreen(linClamp, uv));
    const float hA = lerp(h0, HeightAt(heightUv, prevHeightUv, float2(t.x, t.y), generated), neighbourhood.y);
    const float hB = lerp(h0, HeightAt(heightUv, prevHeightUv, float2(-t.x, t.y), generated), neighbourhood.x);
    const float hC = lerp(h0, HeightAt(heightUv, prevHeightUv, float2(t.x, -t.y), generated), neighbourhood.z);
    const float hD = lerp(h0, HeightAt(heightUv, prevHeightUv, float2(-t.x, -t.y), generated), neighbourhood.w);

    const float dhx = 0.25 * ((hA + hC) - (hB + hD));
    const float dhy = 0.25 * ((hA + hB) - (hC + hD));
    const float3 n = normalize(float3(-aBump * dhx, -aBump * dhy, 1.0));
    // Directional term, symmetric for slopes facing towards and away from the light (N.L - L.z is not: a slope can
    // darken by up to L.z but brighten only by 1 - L.z), so light-facing bevel edges get a highlight as strong as the
    // shadow on the opposite edge.
    const float shade = ReliefTanh(-aBump * dot(float2(dhx, dhy), aLight.xy));
    const float neighbourHeight = 0.25 * (hA + hB + hC + hD);
    const float cavity = max(0.0, neighbourHeight - h0);

    if (aDebug == DBG_HEIGHT) return float4((0.5 + 0.5 * h0).xxx, src.a);
    if (aDebug == DBG_NORMALS) return float4(0.5 + 0.5 * n, src.a);
    if (aDebug == DBG_PROTECTION) // red = HUD-protected, blue = edge-protected or no texture, green = receives relief
        return float4(0.25 * src.rgb + 0.75 * float3(1.0 - prot.x, prot.w, 1.0 - prot.y), src.a);

    float4 colour = src;
    float2 offset = 0.0;
#ifdef RELIEF_PARALLAX
    if (aParallaxPx > 0.0 && prot.x >= 0.75 && prot.y >= kWarpSurfaceMin && fade > 0.0) {
        const float2 gradient = float2(dhx, dhy) * aGameSize / (aTapPx * aContent.zw);
        const float warpProtection = prot.x * smoothstep(kWarpSurfaceMin, 1.0, prot.y) * depthProtection * motionProtection * fade;
        // The generator already interpolated fine detail. Use the local height average for most of the
        // displacement after generation, while retaining the centre estimate for lighting and cavity.
        const float warpHeight = aFilteredParallax != 0 ? lerp(h0, neighbourHeight, 0.65) : h0;
        offset = ParallaxOffset(local, uv, prevUv, generated, warpHeight, gradient, warpProtection, depthGain, groundHint);
        if (any(offset != 0.0) && aDebug != DBG_OFFSET)
            colour.rgb = SampleParallax(src.rgb, i.pos.xy, offset);
    }
#endif
    if (aDebug == DBG_OFFSET) return float4(0.5 + offset / 6.0, length(offset) / 3.0, src.a);

    const float d = aShadeGain * shade - aCavityGain * cavity;
    const float delta = aMaxDelta * ReliefTanh(d / aMaxDelta) * prot.w * fade;
    if (abs(delta) < (1.0 / 2048.0)) return colour;
    if (aSrcLinear) return float4(colour.rgb * (1.0 + delta), colour.a);
    return float4(ScaleSrgb(saturate(colour.rgb), 1.0 + delta), colour.a);
}
