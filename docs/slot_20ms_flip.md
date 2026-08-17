# Task: flip `TDMA_SLOT_DURATION_US` to 20 ms + acceptance soak

**Status:** ready. **Decisions (2026-08-16):** straight constant flip (no runtime
selector first); `frame_ctr` wrap fix deferred — soaks at 20 ms capped under
1.4 h; units 3/4 not yet available — this task validates on the 2-unit bench,
the 4-unit soak and field campaign are gated on hardware.

## The change

`src/tdma/tdma.h`:

```diff
-#define TDMA_SLOT_DURATION_US	50000
+#define TDMA_SLOT_DURATION_US	20000
```

Everything else derives or is width-independent, verified at head:

| Constant | At 20 ms | Check |
|---|---|---|
| `TDMA_FRAME_DURATION_US` | 80 ms (derived, 4 slots) | beacon cadence 12.5 Hz |
| `TDMA_SLOT_ACTIVE_US` | 18 ms (derived, 90%) | > 8.3 ms worst in-slot completion (532 + 7760) — TX timeout only, RX is continuous |
| `TDMA_TOA_US`, `TDMA_TX_START_LATENCY_US` | unchanged | width-independent; `dtx` = 8292 µs is the regression check |
| `TDMA_SYNC_STEP_CLAMP_US` | 500 µs **per 80 ms frame** | max slew 6.25 ms/s (was 2.5) — intended, no change |
| `TDMA_RX_MSGQ_DEPTH` | 32 | header comment already sizes this for the 20 ms 4-unit case |

## Pre-flight audit (before flashing)

1. Confirm every `tdma_config.slot_duration_us` initializer in the app variants
   passes the macro, not a literal — `tdma_init()` rejects a mismatch.
2. Comment touch-up in `tdma.h`: the per-slot tallies comment says "the
   programmed 45 ms" (the 50 ms-era `SLOT_ACTIVE`); make it width-neutral.
   The `TDMA_SLOT_DURATION_US` comment's "test-build / production target"
   framing inverts with the flip — update it.
3. No soak-preset code change. Constraint recorded instead: at 20 ms the
   counter wraps at **1.46 h**. Within-file continuity is wrap-safe (modular
   deltas); absolute cross-file alignment past a wrap is not. Until the wrap
   card closes: use the 5 min / 30 min / 1 h-equivalent presets for anything
   needing cross-file pairing; a continuous run is valid but only
   pairwise-alignable up to the first wrap.

## Acceptance soak

30 min preset, 20 ms slots, both bench units, +0 dBm (same as the 250 µs
threshold soak, for comparability), SD-logged both sides, decode both files.

Rate sanity: 2 units at 80 ms frames = 1 TX + 1 RX record per frame per unit
= 25 rec/s = 1.6 KB/s — half the SD budget.

**Pass criteria** (50 ms baselines in parentheses):

- Lock: SYNCING → RUNNING within single-digit seconds (2.3 s). The one
  failure mode the one-way transition allows is *slow or absent lock* — a
  unit stuck in SYNCING fails the soak regardless of anything else.
- `dtx` = 8292 ± tens of µs, **both roles** (8292 exactly); `drx` collapses
  to the same value on both (8290/8291).
- `phase_err`: median ≈ 0, p95 |err| ≤ 50 µs (24 µs), worst |err| < 125 µs —
  half the 250 µs bar. Beacons arrive 2.5× more often, so equal-or-tighter
  is the expectation; growth toward the bar is a red flag even if it locks.
- Link: PER 0.000%, 0 missed / 0 dup, 0 `seq` gaps, `stale_retx` /
  `busy_timeouts` / `slot_timeouts` all 0 (all match the 11-min baseline).
- `ppm`: mean within a few ppm of −6.9 (same crystal pair). Per-sample σ up
  to ~170 is *expected*, not a regression — the estimator divides the same
  ±13 µs timestamp jitter by an 80 ms frame instead of 200 ms. The mean is
  the signal.

## Out of scope / gated

- **4-unit soak**: gated on building units 3 and 4. The open re-validation
  question (three peer slots between corrections during acquisition) can only
  be answered there.
- **Field campaign at 20 ms**: after the 4-unit bench soak.
- **`frame_ctr` widen/unwrap** and **runtime slot-width selector**: separate
  cards, unchanged by this task.

## CHANGELOG

New entry: slot width 50 → 20 ms with the soak results and the pass/fail
table above; note the 1.4 h soak-duration constraint at 20 ms and reference
the wrap limitation. Update the "what remains before calling 20 ms proven"
line to: 4-unit soak (hardware pending) + field campaign.
