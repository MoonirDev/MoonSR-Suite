// Smart parallax analysis, gate pass (HALF resolution): local detail energy, the protection gates, the static-pixel
// (HUD) counter and all frame statistics. Owner: upscaling-engineer. Research note 1.2 steps 5-6, 3.1, 3.3, 3.4, 3.6.
// The gates are smooth fields, so they are computed on 2x2 blocks (a quarter of the pixels) and sampled bilinearly by
// the height pass and by Apply:
//   sigma     = sqrt(box5x5(D^2)) over half texels (about 10x10 game px)    D = L2x2 - (mean_a * L2x2 + mean_b)
//   tex gate  = smoothstep(e0, e1, sigma) * (1 - smoothstep(e2, e3, sigma))  mid-energy texture only, times a flat-paint
//               gate (detail between perfectly flat areas is painted albedo, not relief)
//   edge gate = 1 - smoothstep(a, b, max3x3 |grad B|)                       geometry edges, dilated about 2-3 game px
//   HUD gate  = 1 - smoothstep(6, 16, max5x5 static count)
//   static count (per half texel): + while the frame changes and the texel keeps the value it had when its static run
//               began; reset when it drifts (slow sub-pixel motion breaks the run within a few frames; an opaque overlay
//               never does). Smooth TEXTURED texels hold their count (a slow pan barely changes them); flat texels and
//               texels near strong edges may count (a moving edge changes them, a static overlay edge does not).
//               The frame-change share and the scene-cut flag it uses are the previous frame's (one frame late).
// Output G = (hud, edge * tex, changed since the previous frame, SigmaEnc(sigma)) RGBA8. Frame statistics (pixels that receive relief, textured pixels under the
// HUD gate, changed pixels, sum |dL|, sum Y; 4 pixels per half texel) are reduced per group, spread over slots, and the
// last group to finish sums them and updates the frame state (relief_frame.hlsli), which the height pass and Apply read.
#include "relief/relief_common.hlsli"

Texture2D<float>   curL        : register(t0);
Texture2D<float2>  abTex       : register(t1); // half-size (mean_a, mean_b)
Texture2D<float2>  prevCount   : register(t2); // half-size (count / 64, LEnc(reference L)) of the previous frame
Texture2D<float>   prevL       : register(t3); // full-size encoded L of the previous frame
Buffer<uint>       motionStats : register(t4); // MotionFieldView::stats (optional)
SamplerState       linClamp    : register(s0);

RWTexture2D<unorm float4> outG : register(u0); // (hud, edge * tex, changed, SigmaEnc(sigma))
globallycoherent RWByteAddressBuffer stats : register(u1); // coherent: the last group reads the other groups' sums
RWByteAddressBuffer state   : register(u2);  // persistent frame state (relief_frame.hlsli)
RWTexture2D<unorm float2> outCount : register(u3); // half-size (count / 64, LEnc(reference L))
#include "relief/relief_frame.hlsli"

#define TILE 16
#define HALO 2
#define SIDE (TILE + 2 * HALO)
#define GS (TILE + 2) // gradient tile: 1 half texel around the tile for the 3x3 dilations
groupshared float2 gsBD[SIDE * SIDE]; // (B, D)
groupshared float gsCnt[SIDE * SIDE];
groupshared float gsG[GS * GS];
groupshared float3 gsHorizontal[TILE * SIDE]; // horizontal sums of D² and flat samples, and maximum HUD count
groupshared uint4 gsRed[TILE * TILE];  // (textured, hud, changed, |dL|)
groupshared uint gsRedY[TILE * TILE];  // sum Y
groupshared uint gsLast;

[numthreads(TILE, TILE, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const int2 hi = int2(cHalf) - 1;
    const int2 tile0 = int2(gid.xy) * TILE;
    const bool reset = (cFlags & RELIEF_F_RESET) != 0;
    for (uint i = gi; i < SIDE * SIDE; i += TILE * TILE) {
        const int2 q = clamp(tile0 - HALO + int2(i % SIDE, i / SIDE), int2(0, 0), hi);
        const float2 ab = abTex.Load(int3(q, 0));
        const float l = LDec(curL.SampleLevel(linClamp, (float2(q) * 2.0 + 1.0) / float2(cSize), 0)); // 2x2 mean
        const float b = ab.x * l + ab.y;
        gsBD[i] = float2(b, l - b);
        gsCnt[i] = reset ? 0.0 : 64.0 * prevCount.Load(int3(q, 0)).x;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint g = gi; g < GS * GS; g += TILE * TILE) {
        const int2 c = int2(g % GS, g / GS) + (HALO - 1);
        const float gx = gsBD[c.y * SIDE + c.x + 1].x - gsBD[c.y * SIDE + c.x - 1].x;
        const float gy = gsBD[(c.y + 1) * SIDE + c.x].x - gsBD[(c.y - 1) * SIDE + c.x].x;
        gsG[g] = 0.25 * sqrt(gx * gx + gy * gy); // central difference over 2 half texels = 4 game px
    }
    for (uint j = gi; j < TILE * SIDE; j += TILE * TILE) {
        const uint first = (j / TILE) * SIDE + j % TILE;
        float3 row = 0.0;
        [unroll] for (uint dx = 0; dx < SIDE - TILE + 1; ++dx) {
            const float v = gsBD[first + dx].y;
            row.x += v * v;
            row.y += abs(v) < 0.02 ? 1.0 : 0.0; // about one to two 8-bit code values
            row.z = max(row.z, gsCnt[first + dx]);
        }
        gsHorizontal[j] = row;
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 p = tile0 + int2(gtid.xy);
    uint4 counts = 0;
    uint countY = 0;
    if (all(p < int2(cHalf))) {
        const int2 t = int2(gtid.xy) + HALO;
        float e = 0.0, flat = 0.0, cmax = 0.0;
        [unroll] for (uint dy = 0; dy < SIDE - TILE + 1; ++dy) {
            const float3 row = gsHorizontal[(gtid.y + dy) * TILE + gtid.x];
            e += row.x;
            flat += row.y;
            cmax = max(cmax, row.z);
        }
        const float sigma = sqrt(e / 25.0);
        // Flat-paint gate: detail that sits between perfectly flat areas (painted patterns, decals, signs, UI art,
        // cel shading) is albedo, not relief. Real surface texture has some detail almost everywhere.
        const float flatGate = 1.0 - smoothstep(0.2, 0.4, flat / 25.0);
        const float texGate = smoothstep(cTexGate.x, cTexGate.y, sigma) * (1.0 - smoothstep(cTexGate.z, cTexGate.w, sigma)) * flatGate;
        float gmax = 0.0;
        [unroll] for (int ey = 0; ey < 3; ++ey)
            [unroll] for (int ex = 0; ex < 3; ++ex) {
                gmax = max(gmax, gsG[(gtid.y + ey) * GS + gtid.x + ex]);
            }
        const float edgeGate = (cFlags & RELIEF_F_PROTECT_EDGES) ? 1.0 - smoothstep(cEdgeGate.x, cEdgeGate.y, gmax) : 1.0;

        // Change against the previous frame (frame statistics) and the static counter of this texel.
        const uint k0 = t.y * SIDE + t.x;
        const float l = gsBD[k0].x + gsBD[k0].y;
        const float lPrev = LDec(prevL.SampleLevel(linClamp, (float2(p) * 2.0 + 1.0) / float2(cSize), 0));
        const float dl = abs(l - lPrev);
        // Log luminance amplifies small code-value changes in shadows. Give prefiltered shadow noise
        // more room without losing the small changes that identify a slow pan in brighter areas.
        const float noiseFloor = lerp(0.01, 0.045, saturate((-min(l, lPrev) - 3.0) / 4.0));
        const bool texelChanged = dl > noiseFloor;
        bool cut = reset || StateGet(RST_CUT) > 0.5; // previous frame's flag (this frame's is written by the last group)
        if (cFlags & RELIEF_F_MOTION_CUT) cut = cut || motionStats.Load(cSceneCutIndex) != 0;
        const float changedSharePrev = reset ? 0.0 : StateGet(RST_CHANGED);
        float2 cs = float2(0.0, l);
        if (!reset) { const float2 e2 = prevCount.Load(int3(p, 0)); cs = float2(64.0 * e2.x, LDec(e2.y)); }
        const float lgx = (gsBD[k0 + 1].x + gsBD[k0 + 1].y) - (gsBD[k0 - 1].x + gsBD[k0 - 1].y);
        const float lgy = (gsBD[k0 + SIDE].x + gsBD[k0 + SIDE].y) - (gsBD[k0 - SIDE].x + gsBD[k0 - SIDE].y);
        const bool structured = 0.25 * max(abs(lgx), abs(lgy)) > 0.05 || sigma < cTexGate.x || gmax > cEdgeGate.x;
        // Keep the anchored HUD comparison strict so a slow drift still clears the counter.
        if (cut || abs(l - cs.y) > 0.02) cs = float2(0.0, l);
        else if (changedSharePrev > cFrameChangedMin && structured) cs.x = min(cs.x + 1.0, 64.0);
        outCount[p] = float2(cs.x / 64.0, LEnc(cs.y));
        cmax = cut ? 0.0 : max(cmax, cs.x);
        const float hudGate = (cFlags & RELIEF_F_PROTECT_HUD) ? 1.0 - smoothstep(6.0, 16.0, cmax) : 1.0;
        outG[p] = float4(hudGate, edgeGate * texGate, texelChanged ? 1.0 : 0.0, SigmaEnc(sigma));

        const uint area = (uint)(min(2, int(cSize.x) - 2 * p.x) * min(2, int(cSize.y) - 2 * p.y)); // pixels covered
        counts.x = edgeGate * texGate > 0.25 ? area : 0u; // textured share, HUD-protected pixels included
        counts.y = (hudGate < 0.5 && texGate > 0.1) ? area : 0u; // static TEXTURED pixels (menu rule)
        counts.z = texelChanged ? area : 0u;
        counts.w = (uint)(min(dl, 8.0) * 32.0) * area;
        countY = (uint)(saturate(exp2(l) - kLogEps) * 64.0) * area;
    }
    gsRed[gi] = counts;
    gsRedY[gi] = countY;
    GroupMemoryBarrierWithGroupSync();
    // The first row sums the columns before the group leader combines them. This needs one
    // barrier between the two sums, independent of wave size and without SM6 wave operations.
    if (gi < TILE) {
        uint4 column = gsRed[gi];
        uint sumY = gsRedY[gi];
        [unroll] for (uint y = 1; y < TILE; ++y) {
            column += gsRed[y * TILE + gi];
            sumY += gsRedY[y * TILE + gi];
        }
        gsRed[gi] = column;
        gsRedY[gi] = sumY;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        uint4 groupTotals = 0;
        uint groupY = 0;
        [unroll] for (uint x = 0; x < TILE; ++x) {
            groupTotals += gsRed[x];
            groupY += gsRedY[x];
        }
        uint dummy;
        const uint slot = RsSlot(gid.xy);
        stats.InterlockedAdd(RS_ADDR(RS_TEXTURED, slot), groupTotals.x, dummy);
        stats.InterlockedAdd(RS_ADDR(RS_HUD, slot), groupTotals.y, dummy);
        stats.InterlockedAdd(RS_ADDR(RS_CHANGED, slot), groupTotals.z, dummy);
        stats.InterlockedAdd(RS_ADDR(RS_ABSDL, slot), groupTotals.w, dummy);
        stats.InterlockedAdd(RS_ADDR(RS_SUMY, slot), groupY, dummy);
        DeviceMemoryBarrier();
        uint done;
        stats.InterlockedAdd(RS_DONE, 1u, done);
        const uint2 groups = (cHalf + TILE - 1) / TILE;
        gsLast = done == groups.x * groups.y - 1 ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gsLast != 0) { // the last group to finish sums the frame statistics and updates the frame state
        gsRed[gi] = gi < RS_SLOTS ? uint4(stats.Load(RS_ADDR(RS_TEXTURED, gi)), stats.Load(RS_ADDR(RS_HUD, gi)),
                                          stats.Load(RS_ADDR(RS_CHANGED, gi)), stats.Load(RS_ADDR(RS_ABSDL, gi))) : 0u;
        gsRedY[gi] = gi < RS_SLOTS ? stats.Load(RS_ADDR(RS_SUMY, gi)) : 0u;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint s = RS_SLOTS / 2; s > 0; s >>= 1) {
            if (gi < s) { gsRed[gi] += gsRed[gi + s]; gsRedY[gi] += gsRedY[gi + s]; }
            GroupMemoryBarrierWithGroupSync();
        }
        if (gi == 0) {
            const bool motionCut = (cFlags & RELIEF_F_MOTION_CUT) != 0 && motionStats.Load(cSceneCutIndex) != 0;
            ReliefFrameUpdate(gsRed[0].x, gsRed[0].y, gsRed[0].z, gsRed[0].w, gsRedY[0], motionCut);
        }
    }
}
