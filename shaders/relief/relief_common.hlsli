// Smart parallax (screen-space relief), shared definitions. Owner: upscaling-engineer.
// Design: documentation/research/screen_space_relief.md, Parts 1.2, 2.1, 3 and 4.2. Clean-room implementation.
//
// Analysis runs at game resolution as compute passes (8x8 or 16x16 groups):
//   luma   : capture -> prefiltered log2 luminance L (R32F) + frame-change statistics
//   box 1-4: self-guided filter of L (He, Sun, Tang 2010): base B and detail D = L - B (RG32F)
//   height : contrast normalisation, gates, HUD counter, temporal history -> H (R16F), protection P (RGBA8)
//   frame  : one thread; frame-level state (fade, auto-disable, coverage) in a small raw buffer
// Apply (relief_apply_ps.hlsl) runs per displayed frame at output resolution.
#ifndef RELIEF_COMMON_HLSLI
#define RELIEF_COMMON_HLSLI

#ifndef RELIEF_NO_ANALYSE_CB
cbuffer ReliefAnalyseCB : register(b0)
{
    int2   cSrcOffset;      // top-left of the game rect inside the source copy (always 0,0 today)
    uint2  cSize;           // game size in pixels
    int    cRadius;         // guided filter box radius (half-resolution texels)
    float  cEps;            // guided filter regulariser (log2-luma units squared)
    float  cSigmaK;         // contrast normalisation: H = tanh(D / (k*sigma + floor))
    float  cSigmaFloor;
    float4 cTexGate;        // e0, e1 (lower energy ramp), e2, e3 (upper energy ramp), on sigma_local
    float2 cEdgeGate;       // a, b: edge protection ramp on the dilated |grad B|
    uint   cFlags;          // RELIEF_F_*
    uint   cSceneCutIndex;  // element of the motion stats buffer holding the scene-cut flag
    float2 cCellInv;        // 1 / motion cell size (frame px), 0 when no motion
    uint2  cCells;          // motion field size in cells
    float  cHistoryAlpha;   // weight of the new estimate where history is valid
    float  cFrameChangedMin;// share of changed pixels above which the HUD counter advances
    uint2  cHalf;           // half-resolution size of the guided-filter statistics ((size + 1) / 2)
};
#endif

#define RELIEF_F_PROTECT_HUD   1u
#define RELIEF_F_PROTECT_EDGES 2u
#define RELIEF_F_AUTO_DISABLE  4u
#define RELIEF_F_MOTION        8u
#define RELIEF_F_MOTION_CUT    16u
#define RELIEF_F_RESET         32u  // first frame after a reset: no history, HUD counter cleared

// Frame statistics buffer (written by the half-size gate pass), byte offsets. Each counter is spread over RS_SLOTS
// addresses (chosen by thread group) so that thousands of groups do not serialise on one global atomic. The LAST group
// of the pass to finish (finished-group counter) sums the slots in parallel and updates the frame state.
#define RS_SLOTS       64
#define RS_COUNTERS    5
#define RS_ADDR(c, slot) ((((c) * RS_SLOTS) + (slot)) * 4)
#define RS_TEXTURED    0   // pixels with P > 0.25 (received relief)
#define RS_HUD         1   // textured pixels (texture gate > 0.1) with HUD protection > 0.5
#define RS_CHANGED     2   // pixels whose L (2x2 mean) changed by more than 0.01 stop since the previous frame
#define RS_ABSDL       3   // sum of truncated |dL| * 32 per pixel (|dL| saturates at 8 stops; no overflow up to 4K)
#define RS_SUMY        4   // sum of truncated linear Y * 64 per pixel
#define RS_DONE        ((RS_COUNTERS * RS_SLOTS) * 4) // finished-group counter
// Persistent state buffer (frame pass), float slots, byte offsets.
#define RST_FADE       0
#define RST_COV_EMA    4
#define RST_HUD_EMA    8
#define RST_MEAN_Y     12
#define RST_REASON     16  // 0 none, 1 stylised/flat, 2 menu/static screen, 3 dark frame (as float)
#define RST_FRAMES     20
#define RST_COV        24
#define RST_HUD        28
#define RST_CUT        32
#define RST_CHANGED    36
#define RST_SLOTS      16  // buffer size in 32-bit slots

float SrgbToLinear(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 SrgbToLinear3(float3 c) { return float3(SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b)); }
float LinearToSrgb(float c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 LinearToSrgb3(float3 c) { return float3(LinearToSrgb(c.r), LinearToSrgb(c.g), LinearToSrgb(c.b)); }

uint RsSlot(uint2 gid) { return (gid.x * 7u + gid.y * 13u) & (RS_SLOTS - 1u); }

static const float3 kRec709 = float3(0.2126, 0.7152, 0.0722);
static const float kLogEps = 1.0 / 256.0; // log2(Y + eps): keeps near-black from exploding into noise

// L = log2(Y + 1/256) lies in [-8, 0]. It is stored as R16_UNORM over [-9, 1] (step 0.00015 stop; 2 bytes instead of 4:
// the analysis is memory-bound). The static counter is R16G16_UNORM: (count / 64, encoded reference L).
float LEnc(float l) { return saturate((l + 9.0) * 0.1); }
float LDec(float v) { return v * 10.0 - 9.0; }
// The gate texture is RGBA8 (hud, edge x tex, changed, encoded sigma): sigma is stored as sqrt(sigma / 2), which keeps about
// 0.0001-0.01 stop resolution where the texture gate ramps (0.012-0.045) and covers sigma up to 2 stops.
float SigmaEnc(float s) { return sqrt(saturate(s * 0.5)); }
float SigmaDec(float v) { return 2.0 * v * v; }

#endif
