// Scene-cut decision and pair statistics. A single thread; reads 2 x 144 histogram counters and the stats counters.
//
//   histDist = mean over the 3x3 regions of the L1 distance between the normalised 16-bin luma histograms (0..2)
//   badFrac  = textured cells whose best match is poor (rel > threshold) or whose vector fails the forward-backward
//              check (consistency < 0.25) / textured cells, both directions
// Rule (gP0 = (badHard, histSoft, badSoft, histFlat), gP1.x = min textured fraction):
//   enough texture (textured >= minTexturedFrac * cells):  cut = badFrac > badHard || (histDist > histSoft && badFrac > badSoft)
//   almost no texture (flat images): cut = histDist > histFlat
// badFrac uses a zero-mean, contrast-normalised cost, so a brightness change does not count as "bad"; histDist alone
// would (histograms shift), which is why it is only a supporting cue.
#include "framegen/fg_common.hlsli"

Buffer<uint> gHistPrev : register(t0);
Buffer<uint> gHistCurr : register(t1);
RWBuffer<uint> gStats : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    float dist = 0.0;
    for (uint r = 0; r < 9; ++r) {
        float na = 0.0, nb = 0.0;
        for (uint b = 0; b < 16; ++b) {
            na += float(gHistPrev[r * 16 + b]);
            nb += float(gHistCurr[r * 16 + b]);
        }
        na = max(na, 1.0);
        nb = max(nb, 1.0);
        float d = 0.0;
        for (uint b2 = 0; b2 < 16; ++b2) d += abs(float(gHistPrev[r * 16 + b2]) / na - float(gHistCurr[r * 16 + b2]) / nb);
        dist += d;
    }
    dist /= 9.0;

    const float count = float(gStats[kStatCount]);
    const float textured = float(gStats[kStatTextured]);
    const float bad = float(gStats[kStatBad]);
    const float badFrac = bad / max(textured, 1.0);
    bool cut;
    if (textured >= gP1.x * 2.0 * max(count, 1.0)) cut = badFrac > gP0.x || (dist > gP0.y && badFrac > gP0.z);
    else cut = dist > gP0.w;

    gStats[kStatCut] = cut ? 1u : 0u;
    gStats[kStatHistDist] = uint(dist * 1000.0 + 0.5);
    gStats[kStatBadFrac] = uint(badFrac * 1000.0 + 0.5);
    gStats[kStatMeanConf] = asuint(float(gStats[kStatSumConf]) / 1024.0 / max(count, 1.0));
    gStats[kStatMeanMotion] = asuint(float(gStats[kStatSumMotion]) / 16.0 / max(count, 1.0));
}
