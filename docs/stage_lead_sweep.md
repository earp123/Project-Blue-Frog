# Staging lead sweep: when must a payload reach the engine?

**Status:** done, 2026-09-25. **Why:** the Codec 2 transport test measured
78.7 ms from staging a payload to the peer's DIO1, almost a whole frame,
because the soak runner stages the next chunk right after its own TxDone.
Before the audio path is designed around a figure, find where the engine's
real deadline is, what moves it in the field, and what the latency is at
the deadline that always holds. This measures a mechanism; it does not tune
a number to the bench.

## Model

The engine takes a staged payload (`rx_slot_slack_work()`,
`src/tdma/tdma_core.c`) at two kinds of point, and sends the newest one it
took before the unit's TX boundary:

1. **every RX-slot entry**, after `SetRx`, in every slot that is not ours,
   occupied or not;
2. **after every good RxDone**, only when a packet from that slot is heard.

So the last pickup before our slot is the entry of the slot before ours
(always, one slot ahead of our boundary), or that slot's RxDone (only when
a unit transmits there *and* we hear it). A unit whose preceding slot is
empty (the master in a three-unit kit: slot 3) has only the first. In the
field, one lost packet in the preceding slot removes the second for that
frame.

## Method

- **Engine:** `tdma_next_tx_us()` (`tdma.h`), the slot-clock time of this
  unit's next TX boundary, from a frame anchor the radio thread
  republishes at every slot tick. Needed by any audio pacing anyway.
- **Runner** (RTT builds only; without RTT, `soak_data_fn` is still
  instruction-identical to `9d9a1a8`): `rtt_link.py lead <us>` stages each
  chunk that long before the next TX boundary. The TX record logs the lead
  achieved (`SOAK_F_TX_LEAD`, in `frame_ctr`). The data thread polls every
  2 ms, so achieved leads scatter over ~2 ms below each setting, which maps
  the deadline at sub-ms resolution.
- **Per chunk** (`tools/lead_sweep.py analyse`): stage-to-DIO1 minus lead is
  the air path (~8 ms, on time), or a frame more (late), or the chunk never
  arrives (lost). The clock offset between sender and receiver comes from
  common view of the third unit. Each chunk is also tagged with whether the
  sender heard its preceding slot in that frame.
- **Bench:** three units in clip mode, +0 dBm, 2 min per setting at 22, 21,
  20.5, 20, 19.5, 19, 16, 13, 12 and 11 ms, plus 25 ms and today's "at
  once". About 47,000 chunks after trimming run edges. Captures are in
  `soaks/rtt/lead/` (untracked).

## Results

| Unit | Preceding slot | Heard it? | Missed at leads | On time at every lead above |
|---|---|---|---|---|
| MASTER (0) | 3, empty | never | 8.9 to 19.60 ms | **19.60 ms** (4,239 chunks) |
| SEC 1 (1) | 0, master | always | 8.9 to 10.29 ms | 10.29 ms (14,331 chunks) |
| SEC 2 (2) | 1, SEC 1 | 15,590 frames | 8.9 to 10.34 ms | 10.34 ms (14,086 chunks) |
| SEC 2 (2) | 1, SEC 1 | 3 frames not | 15.7 to 18.7 ms | (all 3 missed) |

- **Guaranteed pickup: 19.6 ms before the boundary.** That is the slot
  (20 ms) less ~0.4 ms of `SetRx` and mode probe before the slack work
  runs. The master's cliff is sharp: in the 19.5–20.0 ms bin, 1,014 chunks
  were on time and 236 missed; below it, none were on time.
- **Opportunistic pickup: 10.3 ms, and only when the preceding slot is
  heard.** The preceding packet's DIO1 is 11.7 ms before our boundary
  (median 11.71 / 11.77), plus ~1.4 ms to drain it. The three SEC 2 frames
  that did not hear SEC 1 (right after lock, before SEC 1 transmitted)
  missed at 15.7–18.7 ms: the field failure, seen on the bench.
- **Stage-to-DIO1 at a lead is the lead plus ~8.2 ms of air path:** 29.2 ms
  at a 22 ms setting (all units), 32.2 ms at 25 ms, against 78.7 ms today.
- **How a miss shows:**
  - *Consistently* late (the master below 19.6 ms): every chunk rides one
    frame late, +80 ms, with no loss and no `stale_retx`.
  - *Intermittently* late (a secondary around 10.3 ms): the frame re-sends
    the previous chunk (`stale_retx`, a dup at the peer) and the late chunk
    is usually overwritten before it goes out (lost).
  - So staging jitter across the deadline is worse than being consistently
    late.
- **Steady state had no unexplained misses.** The rare misses at 14–19.5 ms
  are all at run edges, where the preceding unit had not started or had
  already stopped (heard = no), or the receivers had stopped listening.

## What to design to

- **Stage by the guaranteed pickup, never the opportunistic one.** Staging
  after 19.6 ms works on the bench for any unit whose preceding slot is
  occupied, and fails in the field on every lost preceding packet. It never
  works for a unit whose preceding slot is empty.
- **Margin above 19.6 ms has to cover:**
  - the producer's staging jitter (here the 2 ms poll; with audio, when
    the encoder finishes a chunk);
  - a secondary's sync correction, up to `TDMA_SYNC_STEP_CLAMP_US`
    (0.5 ms) per frame;
  - any SPI contention the radio thread sees at slot entry.

  A 22–24 ms target gives stage-to-DIO1 of about 30 ms with the engine as
  it is. The encode test adds its encode time in front of the stage.
- **The audio producer must be paced by the frame clock.**
  `tdma_next_tx_us()` is now the reference. A microphone on its own clock
  drifts against the 80 ms frame and eventually crosses the deadline; that
  pacing (or resampling) is a design question for the audio path.

## Engine work this points to

The 19.6 ms is one slot width, set by where the engine takes the payload,
not by the radio. Taking it at a dedicated point just before our own TX
would shrink the guaranteed lead to what that point costs (~1–2 ms), make it
the same for every unit whatever its neighbours do, and bring stage-to-DIO1
to ~10 ms. Two ways to do it:

1. a pickup inside the TX-slot entry, before `SetTx`. This lengthens the TX
   start path by one 40 B buffer write, so `TDMA_TX_START_LATENCY_US` needs
   re-measuring;
2. a second compare alarm a fixed time before our TX boundary, which leaves
   the TX start path alone.

Either way the sweep, the TX-record lead and `lead_sweep.py` measure the
result unchanged.
