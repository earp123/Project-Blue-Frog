# Task: make `tdma_port_add_phase_adj()` accumulate

**Status:** ready for implementation. **Scope:** one semantic fix + comment updates + CHANGELOG. No behavior change for the current call pattern.

## Context

`tdma_port_add_phase_adj()` in `src/tdma/tdma_port.c` is named "add" but implements *set*:

```c
void tdma_port_add_phase_adj(int32_t adj_us)
{
	atomic_set(&phase_adj_us, adj_us);
}
```

The consumer is `slot_alarm_cb()`, which folds the entire pending value into one boundary:

```c
next_target += slot_ticks + (uint32_t)atomic_clear(&phase_adj_us);
```

With overwrite semantics, any second post before the next boundary silently discards the first. This is **latent today**: `tdma_core_sync_feed()` posts exactly one correction per beacon (one per frame), and boundaries consume 4x per frame, so there is never more than one outstanding value — set and add are currently indistinguishable. It becomes a real bug the moment two posts can land between consumptions (a slew-limited loop splitting a large error into partial steps, or a drift/ppm feed-forward writer added alongside the beacon correction — both on the table for the M3 filter work). A dropped correction self-heals at the next beacon but is transiently wrong; at 20 ms slots with `TDMA_SYNC_LOCK_ERR_US` tightened to ~250 us, a dropped ms-scale correction can bounce a unit out of RUNNING.

## Change

`src/tdma/tdma_port.c`:

```diff
 void tdma_port_add_phase_adj(int32_t adj_us)
 {
-	atomic_set(&phase_adj_us, adj_us);
+	atomic_add(&phase_adj_us, adj_us);
 }
```

Notes for the implementer:

- **Behavior-identical for the current single-post pattern**: adding to a zeroed pending value equals setting it. The acquisition snap (`fwd` up to one frame) and the steady-state clamped step both still apply in full at the next boundary.
- **Race with the consumer is benign under add**: a post either lands before the ISR's `atomic_clear` (applied at this boundary) or after (pending for the next). Nothing is ever lost. This was also true under set for a single writer; add extends the guarantee to any number of posts.
- The `(uint32_t)` cast on the consumer side stays — negative adjustments work through two's-complement wraparound on the unsigned add, as today.
- `tdma_port_schedule_start()` already zeroes `phase_adj_us` before arming; unchanged.

## Comment updates (same commit)

1. `src/tdma/tdma_port.h` — the contract comment on `tdma_port_add_phase_adj()`: state that corrections **accumulate** into a single pending value consumed in full at the next slot boundary, and that multiple posts between boundaries sum.
2. `src/tdma/tdma_port.c` — the `slot_alarm_cb()` block comment currently reads "A pending sync correction is folded in once." Amend to: pending corrections accumulate and the sum is folded in once.

## Explicitly out of scope

**No consumption-side per-boundary clamp.** Post-side clamping already exists where it matters: the steady-state step is clamped to `+/-TDMA_SYNC_STEP_CLAMP_US` at the call site in `tdma_core_sync_feed()`, and the acquisition snap is deliberately unclamped and forward-only (see its comment — a clamped loop would take tens of seconds to pull in a worst-case offset, and forward-only guarantees the alarm target never lands in the past). Slewing the snap at the consumer would fight that design. A per-boundary application clamp belongs with the M3 sync-filter rework (`TODO(M3+)` in `tdma_core_sync_feed()`), where multiple partial posts per frame become a real pattern.

## Verification

1. Build the TDMA field console variant; flash both bench units.
2. Cold start: secondary acquires and locks in the usual ~6 s (snap behavior unchanged).
3. 10+ min soak at 50 ms: `ph` converges to single-digit us as before; `dtx`/`drx` hold ~8292 us; no new `stale_retx` or slot timeouts.
4. Behavior must be indistinguishable from the previous build — this change is semantic future-proofing, not a tuning change.

## CHANGELOG

Mark the 2026-07-21 open item ("`tdma_port_add_phase_adj()` overwrites rather than accumulates a pending correction") resolved with the date, and note the accumulate semantics under a new entry. This is step 3 (first half) of the 50 ms -> 20 ms tightening sequence; the `TDMA_SYNC_LOCK_ERR_US` tightening to ~250 us follows as a separate change once this soaks clean.
