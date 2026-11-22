// MoonSR frame generation: shared constants and helpers. Owner: frame-generation-engineer.
//
// Every compute pass and the interpolation pixel shader use the same 96-byte constant buffer layout. The meaning of
// the generic fields is documented per pass at the top of each shader. C++ mirror: `FgConstants` in
// src/framegen/frame_generator.cpp. Never change one without the other.
//
// Units: luma pyramid values are 0..255 (gamma-encoded BT.709 luma). Vectors inside the pyramid passes are in pixels
// of the level being processed; the final per-cell fields are in FRAME pixels.
#ifndef MOONSR_FG_COMMON_HLSLI
#define MOONSR_FG_COMMON_HLSLI

cbuffer FgConstants : register(b0)
{
    int2 gSrcSize;   // size of the image the pass reads (pyramid level or frame)
    int2 gDstSize;   // size of the grid the pass writes
    int2 gFieldSize; // size of a flow field the pass reads
    int2 gAuxSize;   // pass specific second grid size
    float4 gP0;      // pass specific
    float4 gP1;      // pass specific
    uint4 gU0;       // pass specific
    uint4 gU1;       // pass specific
};

static const int kBlock = 8; // block size of the hierarchical search, in level pixels
static const int kCell = 4;  // cell size of the final (refined) field, in level-0 (flow) pixels

// Stats buffer layout (RWBuffer<uint>, see fg_consistency_cs / fg_scenecut_cs).
static const uint kStatSumConf = 0;    // sum of confidence * 1024 (direction 0 cells)
static const uint kStatSumMotion = 1;  // sum of |v| * 16, frame pixels (direction 0 cells)
static const uint kStatCount = 2;      // number of direction 0 cells
static const uint kStatTextured = 3;   // textured cells, both directions
static const uint kStatBad = 4;        // textured cells whose best match is poor, both directions
static const uint kStatCut = 5;        // 1 = scene cut decided for this pair
static const uint kStatHistDist = 6;   // mean regional histogram distance * 1000
static const uint kStatBadFrac = 7;    // bad / textured * 1000
static const uint kStatMeanConf = 8;   // asuint(float)
static const uint kStatMeanMotion = 9; // asuint(float), frame pixels
static const uint kStatSlots = 16;

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

int2 ClampI(int2 p, int2 size) { return clamp(p, int2(0, 0), size - 1); }

// Sub-pixel offset from the rms costs at the best position (c0) and its two neighbours at -h and +h (cm, cp), in
// units of h. Under bilinear resampling the shifted block is linear in the shift between two samples, so the rms of
// the difference is V-shaped, |x - x*| * slope, on each side. Using only the side of the lower neighbour,
// x* = c0 / (c0 + c_side) is exact for that model; in particular an exact match (c0 = 0) stays at 0, which a single
// parabola through three costs does not guarantee.
float SubPixel(float c0, float cm, float cp)
{
    if (cm < cp) return cm + c0 > 1e-6 ? -c0 / (c0 + cm) : 0.0;
    return cp + c0 > 1e-6 ? c0 / (c0 + cp) : 0.0;
}

// Linear index -> 2D position for a row width w (unsigned arithmetic; i, w >= 0).
int2 Unflatten(int i, int w) { return int2(uint(i) % uint(w), uint(i) / uint(w)); }

#endif
