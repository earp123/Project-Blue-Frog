# Indoor fixed-range RF test plan

2026-10-23. Live copy: [Indoor Fixed-Range RF Test Plan](https://claude.ai/artifact/PDT4QK8myxakzV5Tzyf38M)
(the doc may have moved on from this snapshot).

Measure what our units actually deliver at one known indoor distance, then
change one thing at a time to put a dB figure on each suspect: PCB antenna,
orientation, body, TX output and power source.

## Why this test

The pitch walks ([`soaks/RangeTest_1022/`](../soaks/RangeTest_1022)) lost
60–94 % of packets at ~120 m, but they cannot say why the signal was that
weak.

| Finding from the walks and desk checks | What it rules in or out |
| --- | --- |
| At ~120 m, bodies in the way: −102 to −104 dBm, SNR ≤ 0, 60–94 % loss per 20 s | 120 m is the edge of the link as built |
| Loss starts near −95 dBm; line of sight held at −95 dBm with 1–3 % loss | Orientation and blockage are worth 10+ dB |
| All three receivers hit SNR 0 at −102 to −103 dBm in the field | Urban noise was the same for every unit, a few dB at most |
| Bench noise floor: master −109 dBm, near the SX1262's own floor | The receiver chip is performing |
| Every link symmetric within 1.5 dB | No single unit is weak; a loss shared by all three would not show |
| Boosted RX gain was off until 2026-10-23 | ~3 dB recovered, not yet measured over the air |

**Working hypothesis:** the shortfall is mostly the compact PCB antenna and
its orientation, at both ends of every link. A dipole is not an option, since
it would snap off in sports gear.

**On the chip:** the SX1262 is not the prime suspect. Its receiver measured
at its datasheet floor. The bigger chip-level lever is the modulation we
picked: SF5 at 500 kHz is LoRa's fastest and least sensitive setting, chosen
to fit 20 ms TDMA slots. TX output is the one chip-side number not yet
measured; the power ladder below checks it.

## Setup

Two identical shield units on stands, ~25 m (27 yd) apart in the top-floor
gym, clear line of sight, nobody within 2 m of either unit.

- **Units:** master (1050080691) and S1 (960166366), both shield hardware.
  Leave the TFT unit out: its bench noise floor is 8 dB higher.
- **Firmware:** `TDMA_shim` at 620f54d or later (boosted RX gain on, `noise`
  command).
- **Placement:** chairs or stands at the same height, about 1 m. Same
  orientation on both, PCB antenna edge vertical. Mark each spot with tape so
  runs repeat.
- **Distance:** measure it. Every comparison hangs on it.
- **Logging:** one 1-minute soak per condition, SD on, so each condition is
  its own file. Write the file number and the condition on the run sheet.
- **Laptop and J-Link cable** (optional): for `rtt_link.py noise` on each
  unit in the gym.

**Reference point:** in free space, +22 dBm with 0 dBi antennas at 25 m
arrives at about −38 dBm (path loss ~60 dB at 915 MHz). Indoors, reflections
move any one spot by about ±10 dB, which is why the baseline averages several
spots. Anything far below that is antenna or hardware loss.

## Test matrix

Each run changes one thing from the baseline. Runs 1, 2 and 4 matter most.

| # | Run | What changes | Isolates | If healthy |
| --- | --- | --- | --- | --- |
| 0 | Noise floor | `noise 2000` on each unit, no soak | Gym RF noise vs the bench (−109 / −106 dBm) | Within a few dB of the bench |
| 1 | Baseline | 4 spots ~30 cm apart, 30 s each, +22 dBm | Absolute RSSI at a known distance | Near −38 dBm, less the PCB antennas' own loss |
| 2 | TX power ladder | +22, +16, +10, +4 dBm, same spot | Whether the PA reaches +22 | RSSI falls 6 dB per step |
| 3 | Power source | +22 dBm on a USB power bank, then laptop USB | Supply sag at +22 | No difference |
| 4 | Reference antenna | A 915 MHz whip on one unit, if the board can take one | The PCB antenna's cost vs a known antenna | Gap = the PCB antenna penalty |
| 5 | Orientation sweep | Rotate one unit 0°, 90°, 180°, 270°, then lay it flat | Pattern nulls and polarization of the PCB antenna | Spread of a few dB; nulls show as deep dips |
| 6 | On body | Held at chest, at the hip, in a jersey or pack pocket; facing toward, then away | Body loss and detuning in real wear | The worst case is the design case |
| 7 | Edge of link | TX down to −9 dBm, then add a body or rotate until RSSI sits at −95 to −100 dBm | Loss vs RSSI under controlled conditions | Loss onset near −95 dBm, as in the field |

## Run checklist

- [ ] Charge power banks; fresh SD cards in both units; both boot to `CARD OK`
- [ ] Measure and tape the two spots; note the distance and the stand height
- [ ] Run 0: `noise 2000` on each unit (laptop), the other unit off
- [ ] Run 1: baseline, 4 spots × 30 s at +22 dBm (one 2-minute soak)
- [ ] Run 2: power ladder, one 1-minute soak per step: +22, +16, +10, +4 dBm
- [ ] Run 4: reference antenna on one unit, if available, at +22 dBm
- [ ] Run 5: orientation sweep, 30 s per position
- [ ] Run 6: on-body positions, 30 s each, toward and away
- [ ] Run 3: power bank vs laptop USB at +22 dBm
- [ ] Run 7: edge of link at −9 dBm
- [ ] Pull logs (`rtt_link.py sdget`) or copy the cards; keep the run sheet

## Reading the results

| Result | Means | Next move |
| --- | --- | --- |
| Baseline −40 to −50 dBm | Hardware is fine; the walks were lost to bodies, height and distance | Design for body loss and mounting; consider a slower modulation |
| Baseline −55 to −65 dBm | PCB antenna loss at both ends, roughly what compact antennas cost (approximate) | Antenna layout and matching become the main lever |
| Baseline −70 dBm or below | A loss on top of the antenna, shared by all units | Check runs 2 and 3, then the RF path: switch, connector, matching |
| Ladder falls < 6 dB from +22 to +16 | The PA is not reaching +22 | Measure the supply under TX; check the PA config |
| Reference antenna 10+ dB above the PCB antenna | The PCB antenna is the problem | Better PCB antenna or layout; a flexible or embedded antenna |
| Orientation spread > 15 dB | Deep pattern nulls | Mounting rules, or two antennas with diversity |
| On-body loss 15–25 dB | Wear position dominates | Pick the mount by this data; budget the worst position |

**Rough budget for the product:** baseline at 25 m, minus the worst on-body
loss, minus 20 dB for each tenfold increase in distance, gives a range
estimate to sanity-check against the −95 dBm loss onset. Ground and
Fresnel-zone effects outdoors make long range worse, so treat it as an upper
bound.

## Open questions

- Can the Wio-SX1262 board take a u.FL whip for run 4, or is the PCB antenna
  fixed?
- Which wear positions are realistic for the product (chest, shoulder,
  helmet, back)?
- Is an SDR or spectrum analyzer available to measure TX output directly?
- If the antenna is the limit, how much slot time could a slower modulation
  take? SF5 to SF7, or 500 to 250 kHz, gains roughly 3–5 dB but lengthens
  each packet.
