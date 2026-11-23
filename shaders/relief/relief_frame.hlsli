// Smart parallax: frame-level state update (fade, auto-disable, coverage averages). Owner: upscaling-engineer.
// Research note 3.4 and 3.6. Run by ONE thread of the gate pass's last group, after all statistics of the frame exist.
// Keeps exponential averages of the textured share and the HUD share, decides auto-disable (stylised / flat frame, menu
// or static screen, dark frame) and drives a global fade: 0 right after a scene cut or reset, then +0.2 per real frame
// (about 5 frames, no one-frame flash of unstabilised relief), and towards 0 while an auto-disable rule holds. Apply
// multiplies its effect by the fade. The CPU reads a copy of the state buffer a few frames late for ReliefStats.
// Needs `state` (RWByteAddressBuffer) declared by the includer. Totals are in pixels (see relief_common.hlsli RS_*).
#ifndef RELIEF_FRAME_HLSLI
#define RELIEF_FRAME_HLSLI

float StateGet(uint o) { return asfloat(state.Load(o)); }
void StatePut(uint o, float v) { state.Store(o, asuint(v)); }

void ReliefFrameUpdate(uint texturedPx, uint hudPx, uint changedPx, uint absDl32, uint sumY64, bool motionCut)
{
    const float n = float(cSize.x * cSize.y);
    const float cov = float(texturedPx) / n;
    const float hud = float(hudPx) / n;
    const float meanY = float(sumY64) / (64.0 * n);
    const float changed = float(changedPx) / n;
    const float meanAbsDl = float(absDl32) / (32.0 * n);
    const bool reset = (cFlags & RELIEF_F_RESET) != 0;
    const bool cut = reset || motionCut || (changed > 0.85 && meanAbsDl > 0.5);

    float frames = reset ? 0.0 : StateGet(RST_FRAMES);
    float covEma = frames < 1.0 ? cov : lerp(StateGet(RST_COV_EMA), cov, 0.08);
    float hudEma = frames < 1.0 ? hud : lerp(StateGet(RST_HUD_EMA), hud, 0.08);
    float fade = reset ? 0.0 : StateGet(RST_FADE);

    float reason = 0.0;
    if (cFlags & RELIEF_F_AUTO_DISABLE) {
        if (meanY < 0.004) reason = 3.0;            // very dark frame (mean linear Y; about sRGB 0.06 for a flat frame)
        else if (hudEma > 0.6) reason = 2.0;        // mostly static overlay: a menu or a paused screen
        else if (covEma < 0.2) reason = 1.0;        // little texture: flat-shaded or stylised content, or mostly sky
    }
    if (cut) fade = 0.0;
    else fade = reason != 0.0 ? max(fade - 0.2, 0.0) : min(fade + 0.2, 1.0);

    StatePut(RST_FADE, fade);
    StatePut(RST_COV_EMA, covEma);
    StatePut(RST_HUD_EMA, hudEma);
    StatePut(RST_MEAN_Y, meanY);
    StatePut(RST_REASON, reason);
    StatePut(RST_FRAMES, frames + 1.0);
    StatePut(RST_COV, cov);
    StatePut(RST_HUD, hud);
    StatePut(RST_CUT, cut ? 1.0 : 0.0);
    StatePut(RST_CHANGED, changed);
}

#endif
