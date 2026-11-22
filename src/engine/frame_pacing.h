#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace moonsr {

// Highest Fixed-mode multiplier (displayed frames per real frame). See FrameGenConfig in engine.h.
constexpr uint32_t kMaxFrameGenMultiplier = 20;

// Start higher requests at a modest rate and let the measured refresh-cap logic raise them when they fit.
inline uint32_t StartingFrameMultiplier(uint32_t requested, uint32_t previous) {
    return requested <= 1 ? 1 : std::min(requested, std::max(previous, 2u));
}

// A real frame or a frame that is still near its slot is never skipped.
inline bool ShouldSkipStaleGenerated(bool vsync, bool real, double slotMs, double latenessMs, double catchupSlots) {
    return catchupSlots > 0.0 && vsync && !real && slotMs > 0.0 && latenessMs > catchupSlots * slotMs;
}

// One present owed by the Auto scheduler.
struct AutoSlot {
    double displayMs; // when to present (same clock as the arrival times passed in)
    float t;          // interpolation phase between the previous (0) and the newest (1) real frame
    bool real;        // present the newest real frame itself
};

// Auto frame generation ("present at a target rate"). Pure timing logic, no D3D; unit-tested by fgbench --selftest.
//
// Presents lie on a free-running display grid one slot (1 / target) apart. Each grid time P maps to a content time
// c = P - D on a smoothed timeline of the real frames, where D is the hold-back. When real frame B arrives (A is the
// one before), every grid slot whose content time lies between A and B is owed: the slot nearest B (within a quarter
// slot) shows B itself, every other one is synthesised at its own phase t = (c - A) / (B - A) from the ORIGINAL pair.
// So the number of generated frames per real frame follows the game's frame rate and averages a fraction when the
// rates are not multiples of each other. Generated frames are never inputs.
//
// The smoothed timeline (a first-order loop on the capture times) removes compositor quantisation of the arrival
// times, which would otherwise turn into phase errors. The hold-back is D = real frame time - lead + arrival-jitter
// margin + processing allowance, the smallest that lets every slot needing B be shown after B has arrived. In
// general the first such slot can lie just a quarter slot after A, so lead = quarter slot. When the newest real frame
// lands near a slot, the grid is nudged by at most 2% of a slot towards it, so integer ratios lock onto the real
// frames (no judder) while fractional ratios keep a steady grid. Once locked (real frames within 10% of a slot for
// kLockFrames frames; the lock holds until one is more than 20% off, with small absolute floors), the first slot
// needing B is at least 0.8 slot after A, so lead = slot - 1.25 x unlock tolerance (0.75 slot): half a slot less
// latency. The lead changes gradually (2% of a slot per real frame towards the lock, 10% away from it). Whenever the
// hold-back changes, the grid moves by the same amount so the real frames stay on their slots.
//
// Threading: single thread (the engine pipeline thread). No allocation.
class AutoPhaseScheduler {
public:
    static constexpr double kSnapFraction = 0.25;  // real frame shown when within this many slots of a grid time
    static constexpr double kTimelineGain = 0.02;  // smoothed timeline correction per real frame (after start-up,
                                                   // which averages: gain 1 / frames since the restart)
    static constexpr double kNudgeGain = 0.1;      // grid phase correction per snapped real frame ...
    static constexpr double kNudgeMaxFraction = 0.02; // ... at most this fraction of a slot
    static constexpr double kLockFraction = 0.1;   // real frame within this many slots of a grid time counts as locked
    static constexpr double kUnlockFraction = 0.2; // ... and the lock is lost beyond this many
    static constexpr double kMarginQuantile = 0.75; // arrival-lateness quantile covered by the hold-back
    static constexpr double kUnlockedLead = 0.5;    // lead (slots) when not locked; slots up to this far past a real
                                                    // frame may be due before the next real frame arrives (shown late)
    static constexpr double kLockFloorMs = 0.3, kUnlockFloorMs = 0.6; // absolute floors for high target rates
    static constexpr uint32_t kLockFrames = 30;    // consecutive locked real frames before the shorter hold-back

    // Drops the timeline and the grid; the next real frame starts again (shown at once).
    void Reset() {
        haveContent_ = anchored_ = false;
        lockCount_ = 0;
        marginMs_ = 0.0;
        holdBackMs_ = 0.0;
        leadMs_ = -1.0;
    }

    // Called once per real frame. `stable`: the frame fits the frame-time estimate and a valid pair exists.
    // Returns the number of slots written to `out` (in display order); 0 means "show this real frame at once"
    // (start, hitch, pause, resync). `skipped` receives grid slots dropped because presentation fell behind `nowMs`.
    uint32_t OnRealFrame(double arrivalMs, double nowMs, bool stable, double periodMs, double slotMs, double extraDelayMs,
                         AutoSlot* out, uint32_t capacity, uint32_t& skipped) {
        skipped = 0;
        if (!stable || !(periodMs > 0.0) || !(slotMs > 0.0) || capacity == 0 || !haveContent_) {
            Restart(arrivalMs);
            return 0;
        }
        const double predicted = contentMs_ + periodMs;
        const double err = arrivalMs - predicted;
        if (std::abs(err) > periodMs * 0.5) { // the timeline lost the game's rhythm: start again
            Restart(arrivalMs);
            return 0;
        }
        const double prev = contentMs_;
        timelineFrames_ = std::min(timelineFrames_ + 1, 1000u);
        const double gain = std::max(kTimelineGain, 1.0 / timelineFrames_);
        const double curr = predicted + err * gain;
        contentMs_ = curr;
        // Arrival jitter margin: a running estimate of the kMarginQuantile of recent lateness (owner's choice: lower
        // latency over always-even pacing). Arrivals later than this make their first slots late; those are presented
        // at the next opportunity, not dropped.
        const double lateness = arrivalMs - curr;
        const double qStep = std::max(0.02, periodMs * 0.004);
        marginMs_ += lateness > marginMs_ ? qStep * kMarginQuantile : -qStep * (1.0 - kMarginQuantile);
        marginMs_ = std::clamp(marginMs_, 0.0, periodMs * 0.5);
        const double snap = slotMs * kSnapFraction;
        const double lockTol = std::max(slotMs * kLockFraction, kLockFloorMs);
        const double unlockTol = std::max(slotMs * kUnlockFraction, kUnlockFloorMs);
        const double lockedLead = slotMs - 0.5 * unlockTol;
        const bool locked = lockCount_ >= kLockFrames && lockedLead > slotMs * (kUnlockedLead + 0.1);
        const double wantLead = locked ? lockedLead : slotMs * kUnlockedLead;
        if (!anchored_ || leadMs_ < 0.0) {
            leadMs_ = wantLead;
        } else if (wantLead != leadMs_) {
            const double step = wantLead > leadMs_ ? slotMs * 0.02 : slotMs * 0.1;
            leadMs_ = std::clamp(wantLead, leadMs_ - step, leadMs_ + step);
        }
        const double d = std::max(0.0, periodMs - leadMs_ + marginMs_ + extraDelayMs);
        if (!anchored_) {
            nextMs_ = curr + d; // put the newest real frame on a slot
            anchored_ = true;
        } else {
            // A different hold-back maps the grid to different content times; move the grid with it so the real
            // frames keep their place on it. (Changes are small and gradual, except a new arrival-jitter peak.)
            nextMs_ += d - holdBackMs_;
        }
        holdBackMs_ = d;
        if (nextMs_ < nowMs - 2.0 * slotMs) { // permanently behind (> 2 slots): drop whole missed slots, keep the grid
            const double k = std::ceil((nowMs - slotMs * 1.5 - nextMs_) / slotMs);
            nextMs_ += k * slotMs;
            skipped = static_cast<uint32_t>(k);
        }
        uint32_t n = 0;
        bool realShown = false;
        double realOffset = 0.0;
        const double span = curr - prev;
        while (n < capacity) {
            const double c = nextMs_ - d;
            if (c > curr + snap) break;
            if (c >= curr - snap) {
                out[n++] = {nextMs_, 1.0f, true};
                realShown = true;
                realOffset = c - curr;
                nextMs_ += slotMs;
                break;
            }
            // c <= prev can only follow a growing hold-back; such a slot repeats (about) the previous real frame.
            const double t = std::clamp((c - prev) / span, 0.0, 1.0);
            out[n++] = {nextMs_, static_cast<float>(t), false};
            nextMs_ += slotMs;
        }
        if (!realShown && n == capacity) {
            // Far more slots than the schedule can hold (an extreme rate ratio): end with the real frame and move the
            // grid on to it.
            out[n - 1] = {out[n - 1].displayMs, 1.0f, true};
            nextMs_ = std::max(nextMs_, curr + d + slotMs * 0.5);
        } else if (n == 0) {
            // The grid ran ahead of the content (an early frame): show the real frame at the next slot.
            out[n++] = {nextMs_, 1.0f, true};
            nextMs_ += slotMs;
        }
        {
            // Offset of the nearest grid time from the newest real frame. Nudging by it locks integer ratios onto the
            // real frames; for fractional ratios the offsets cycle and the nudges cancel out.
            double offset = realOffset;
            if (!realShown) {
                offset = nextMs_ - d - curr;
                offset -= slotMs * std::round(offset / slotMs);
            }
            const double lim = slotMs * kNudgeMaxFraction;
            nextMs_ -= std::clamp(offset * kNudgeGain, -lim, lim);
        }
        if (!realShown) lockCount_ = 0;
        else if (std::abs(realOffset) < lockTol) lockCount_ = std::min(lockCount_ + 1, kLockFrames);
        else if (lockCount_ < kLockFrames || std::abs(realOffset) > unlockTol) lockCount_ = 0;
        return n;
    }

    // Current hold-back of the content behind the display (the added latency of interpolation), 0 when idle.
    double HoldBackMs() const { return anchored_ ? holdBackMs_ : 0.0; }
    // The grid is locked onto the real frames (integer rate ratio): the shorter hold-back is in use.
    bool Locked() const { return anchored_ && lockCount_ >= kLockFrames; }
    double LeadMs() const { return leadMs_; }

private:
    void Restart(double arrivalMs) {
        contentMs_ = arrivalMs;
        timelineFrames_ = 1;
        haveContent_ = true;
        anchored_ = false;
        lockCount_ = 0;
        holdBackMs_ = 0.0;
        leadMs_ = -1.0;
    }

    bool haveContent_ = false, anchored_ = false;
    uint32_t lockCount_ = 0;
    uint32_t timelineFrames_ = 0;
    double contentMs_ = 0.0; // smoothed content time of the newest real frame
    double nextMs_ = 0.0;    // display time of the next grid slot
    double marginMs_ = 0.0;
    double holdBackMs_ = 0.0;
    double leadMs_ = -1.0;   // current lead (see the class comment); < 0 until anchored
};

// Auto GPU-limit controller. Counts only SKIPPED generated slots (presentation more than one slot behind), never
// late-presented ones: with the low-latency hold-back, late presents are routine and say nothing about GPU load.
// Real frames are never skipped. Evaluated once per stats window; returns the new cap (0 = none) and sets `changed`.
class AutoLoadController {
public:
    static constexpr uint32_t kMinSlots = 20;      // windows with fewer generated slots are ignored
    static constexpr double kOverloadShare = 0.1;  // skipped share that counts as overload ...
    static constexpr int kOverloadWindows = 2;     // ... for this many consecutive windows
    static constexpr double kQuietShare = 0.01;    // below this a window is quiet
    static constexpr int kQuietWindows = 10;       // quiet windows before trying a higher rate again

    void Reset() { overload_ = quiet_ = 0; }

    double Evaluate(uint32_t slots, uint32_t skipped, double targetFps, double gameFps, double requestedFps,
                    double capFps, bool& changed) {
        changed = false;
        if (slots < kMinSlots) return capFps;
        const double share = static_cast<double>(skipped) / slots;
        overload_ = share > kOverloadShare ? overload_ + 1 : 0;
        quiet_ = share < kQuietShare ? quiet_ + 1 : 0;
        if (overload_ >= kOverloadWindows) {
            overload_ = 0;
            const double cap = std::max(gameFps * 1.1, targetFps * 0.85);
            if (cap < targetFps) { changed = true; return cap; }
        } else if (quiet_ >= kQuietWindows && capFps > 0.0) {
            quiet_ = 0;
            changed = true;
            const double raised = capFps * 1.1;
            return raised >= requestedFps ? 0.0 : raised;
        }
        return capFps;
    }

private:
    int overload_ = 0, quiet_ = 0;
};

} // namespace moonsr
