# Am-Q Phase 0 filter accuracy report

Digital magnitude vs analog prototype, max |error| below 0.90 x Nyquist. Target (R12): <= 0.10 dB. Magnitudes are floored at -60 dB before comparison (secondary columns: floored at -40 dB, i.e. only levels that matter audibly). Grid: 384 log points from 10 Hz, 1/48-octave points within +/-2 octaves of f0, and a finer window (step 1/(16 Q_max) octave) for high-Q sections.

Sweep: 10 shapes, 10 slopes, 36 centre frequencies (10 Hz-30 kHz), 8 gains, 6 Q values, sample rates 44.1k 48k 88.2k 96k 176.4k 192k 384k. 508536 cases in 339.5 s.

Revised R12 (area 01, 2026-11-14): <= 0.1 dB below 0.9 x Nyquist for f0 <= 0.9 x Nyquist; 1.1 dB for f0 > 0.9 x Nyquist with Q <= 3; TBD for f0 > 0.9 x Nyquist with Q > 3 (reported separately); levels below -60 dB only need to stay below -60 dB.

Q convention: PROVISIONAL Q convention (R14 open): Bell / Notch / Band Pass: displayed Q is the bandwidth Q of the low-pass to band-pass mapping (centre/bandwidth), prototype order = floor(order/2). Cuts, shelves, Tilt Shelf, All Pass: Butterworth sections; displayed Q multiplies the Q of the dominant (highest-Q) pole pair, so Q = 1 is maximally flat; first-order (6 dB/oct) shapes ignore Q. Flat Tilt ignores Q and slope.

## Default design (matched + joint minimax refinement), by shape

| Shape | Cases | **Revised R12 pass** | TBD cases (Q > 3, f0 > 0.9 Nyq): max error | Uses correction section | Pass <= 0.10 dB everywhere | Max error (dB) | Worst case |
| --- | ---: | ---: | --- | ---: | ---: | ---: | --- |
| Bell | 108864 | 99.999 % (1 miss) | 720: 0.11 dB | 2.254 % | 100.0 % | 0.144 | Bell, 6 dB/oct, f0 20480.0 Hz, gain -30 dB, Q 3, fs 44.1 kHz (worst at 16491 Hz; R12 tol 1.1 dB; 1/1 sections) |
| Low Shelf | 108864 | 99.999 % (1 miss) | 720: 15.68 dB | 1.861 % | 99.9 % | 15.679 | Low Shelf, 30 dB/oct, f0 25803.2 Hz, gain -30 dB, Q 10, fs 44.1 kHz (worst at 18246 Hz; R12 tol TBD dB; 3/3 sections) |
| Low Cut | 13608 | 100.000 % (0 miss) | 90: 0.77 dB | 3.476 % | 99.5 % | 0.768 | Low Cut, 18 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 40, fs 44.1 kHz (worst at 2071 Hz; R12 tol TBD dB; 2/2 sections) |
| High Shelf | 108864 | 99.999 % (1 miss) | 720: 28.19 dB | 1.863 % | 99.9 % | 28.191 | High Shelf, 18 dB/oct, f0 25803.2 Hz, gain +30 dB, Q 40, fs 44.1 kHz (worst at 14482 Hz; R12 tol TBD dB; 2/2 sections) |
| High Cut | 13608 | 100.000 % (0 miss) | 90: 0.24 dB | 12.059 % | 100.0 % | 0.236 | High Cut, 18 dB/oct, f0 30000.0 Hz, gain +0 dB, Q 10, fs 48.0 kHz (worst at 21600 Hz; R12 tol TBD dB; 2/2 sections) |
| Notch | 13608 | 100.000 % (0 miss) | 90: 0.05 dB | 3.366 % | 99.9 % | 0.448 | Notch, 6 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 0.1, fs 44.1 kHz (worst at 19845 Hz; R12 tol 1.1 dB; 1/1 sections) |
| Band Pass | 13608 | 100.000 % (0 miss) | 90: 0.73 dB | 25.243 % | 99.9 % | 0.732 | Band Pass, 6 dB/oct, f0 25803.2 Hz, gain +0 dB, Q 10, fs 44.1 kHz (worst at 19303 Hz; R12 tol TBD dB; 1/1 sections) |
| Tilt Shelf | 108864 | 99.999 % (1 miss) | 720: 27.62 dB | 1.863 % | 99.9 % | 27.617 | Tilt Shelf, 30 dB/oct, f0 25803.2 Hz, gain +30 dB, Q 40, fs 44.1 kHz (worst at 18246 Hz; R12 tol TBD dB; 3/3 sections) |
| Flat Tilt | 2016 | 100.000 % (0 miss) | 0: 0.00 dB | 0.000 % | 100.0 % | 0.033 | Flat Tilt, 6 dB/oct, f0 8127.5 Hz, gain +15 dB, Q 1, fs 88.2 kHz (worst at 27338 Hz; R12 tol 0.1 dB; 10/10 sections) |
| All Pass | 13608 | 100.000 % (0 miss) | 90: 0.00 dB | 0.000 % | 100.0 % | 0.000 | All Pass, 96 dB/oct, f0 10.0 Hz, gain +0 dB, Q 40, fs 384.0 kHz (worst at 10 Hz; R12 tol 0.1 dB; 8/8 sections) |
| **All** | 505512 | **99.999 %** (4 miss) | 3330: 28.19 dB | 2.877 % | 99.9 % | 28.191 | |

Not evaluated (Brickwall, a Phase 2 design): 3024 cases.

## Matched, least-squares per section (previous default) (comparison), by shape

| Shape | Cases | **Revised R12 pass** | TBD cases (Q > 3, f0 > 0.9 Nyq): max error | Uses correction section | Pass <= 0.10 dB everywhere | Max error (dB) | Worst case |
| --- | ---: | ---: | --- | ---: | ---: | ---: | --- |
| Bell | 108864 | 86.678 % (14407 miss) | 720: 15.36 dB | 0.000 % | 86.0 % | 49.377 | Bell, 96 dB/oct, f0 20480.0 Hz, gain +30 dB, Q 40, fs 48.0 kHz (worst at 20186 Hz; R12 tol 0.1 dB; 8/8 sections) |
| Low Shelf | 108864 | 80.859 % (20700 miss) | 720: 48.77 dB | 0.000 % | 79.5 % | 51.583 | Low Shelf, 24 dB/oct, f0 20480.0 Hz, gain -1 dB, Q 40, fs 48.0 kHz (worst at 20276 Hz; R12 tol 0.1 dB; 2/2 sections) |
| Low Cut | 13608 | 90.065 % (1343 miss) | 90: 14.79 dB | 0.000 % | 89.3 % | 14.790 | Low Cut, 12 dB/oct, f0 30000.0 Hz, gain +0 dB, Q 40, fs 44.1 kHz (worst at 2242 Hz; R12 tol TBD dB; 1/1 sections) |
| High Shelf | 108864 | 80.858 % (20701 miss) | 720: 67.77 dB | 0.000 % | 79.5 % | 75.534 | High Shelf, 18 dB/oct, f0 16255.0 Hz, gain +30 dB, Q 40, fs 48.0 kHz (worst at 16485 Hz; R12 tol 0.1 dB; 2/2 sections) |
| High Cut | 13608 | 56.850 % (5833 miss) | 90: 9.24 dB | 0.000 % | 55.4 % | 9.237 | High Cut, 96 dB/oct, f0 25803.2 Hz, gain +0 dB, Q 40, fs 48.0 kHz (worst at 20751 Hz; R12 tol TBD dB; 8/8 sections) |
| Notch | 13608 | 91.374 % (1166 miss) | 90: 3.16 dB | 0.000 % | 90.7 % | 17.025 | Notch, 96 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 0.707, fs 44.1 kHz (worst at 14111 Hz; R12 tol 1.1 dB; 8/8 sections) |
| Band Pass | 13608 | 34.443 % (8862 miss) | 90: 13.64 dB | 0.000 % | 34.0 % | 27.410 | Band Pass, 96 dB/oct, f0 30000.0 Hz, gain +0 dB, Q 3, fs 48.0 kHz (worst at 21600 Hz; R12 tol 1.1 dB; 8/8 sections) |
| Tilt Shelf | 108864 | 80.858 % (20701 miss) | 720: 54.32 dB | 0.000 % | 79.5 % | 61.687 | Tilt Shelf, 18 dB/oct, f0 30000.0 Hz, gain +30 dB, Q 40, fs 96.0 kHz (worst at 31362 Hz; R12 tol 0.1 dB; 2/2 sections) |
| Flat Tilt | 2016 | 93.353 % (134 miss) | 0: 0.00 dB | 0.000 % | 92.9 % | 0.134 | Flat Tilt, 6 dB/oct, f0 30000.0 Hz, gain +30 dB, Q 1, fs 44.1 kHz (worst at 15440 Hz; R12 tol 1.1 dB; 10/10 sections) |
| All Pass | 13608 | 100.000 % (0 miss) | 90: 0.00 dB | 0.000 % | 100.0 % | 0.000 | All Pass, 96 dB/oct, f0 10.0 Hz, gain +0 dB, Q 40, fs 384.0 kHz (worst at 10 Hz; R12 tol 0.1 dB; 8/8 sections) |
| **All** | 505512 | **81.312 %** (93847 miss) | 3330: 67.77 dB | 0.000 % | 80.2 % | 75.534 | |

Not evaluated (Brickwall, a Phase 2 design): 3024 cases.

## Matched, three-point (Vicanek 2016 baseline) (comparison), by shape

| Shape | Cases | **Revised R12 pass** | TBD cases (Q > 3, f0 > 0.9 Nyq): max error | Uses correction section | Pass <= 0.10 dB everywhere | Max error (dB) | Worst case |
| --- | ---: | ---: | --- | ---: | ---: | ---: | --- |
| Bell | 108864 | 80.602 % (20978 miss) | 720: 60.00 dB | 0.000 % | 79.8 % | 72.428 | Bell, 96 dB/oct, f0 16255.0 Hz, gain +15 dB, Q 0.707, fs 44.1 kHz (worst at 13800 Hz; R12 tol 0.1 dB; 8/8 sections) |
| Low Shelf | 108864 | 73.529 % (28627 miss) | 720: 64.31 dB | 0.000 % | 72.6 % | 64.307 | Low Shelf, 72 dB/oct, f0 25803.2 Hz, gain +15 dB, Q 10, fs 44.1 kHz (worst at 19053 Hz; R12 tol TBD dB; 6/6 sections) |
| Low Cut | 13608 | 68.383 % (4274 miss) | 90: 50.50 dB | 0.000 % | 67.9 % | 50.496 | Low Cut, 72 dB/oct, f0 30000.0 Hz, gain +0 dB, Q 40, fs 48.0 kHz (worst at 18034 Hz; R12 tol TBD dB; 6/6 sections) |
| High Shelf | 108864 | 73.529 % (28627 miss) | 720: 60.68 dB | 0.000 % | 72.6 % | 69.501 | High Shelf, 30 dB/oct, f0 30000.0 Hz, gain +30 dB, Q 0.1, fs 88.2 kHz (worst at 17838 Hz; R12 tol 0.1 dB; 3/3 sections) |
| High Cut | 13608 | 65.476 % (4667 miss) | 90: 63.55 dB | 0.000 % | 64.8 % | 63.548 | High Cut, 24 dB/oct, f0 25803.2 Hz, gain +0 dB, Q 10, fs 48.0 kHz (worst at 18956 Hz; R12 tol TBD dB; 2/2 sections) |
| Notch | 13608 | 52.582 % (6410 miss) | 90: 59.86 dB | 0.000 % | 52.3 % | 59.859 | Notch, 72 dB/oct, f0 25803.2 Hz, gain +0 dB, Q 10, fs 44.1 kHz (worst at 19074 Hz; R12 tol TBD dB; 6/6 sections) |
| Band Pass | 13608 | 43.394 % (7652 miss) | 90: 41.19 dB | 0.000 % | 43.0 % | 82.784 | Band Pass, 96 dB/oct, f0 5120.0 Hz, gain +0 dB, Q 0.1, fs 176.4 kHz (worst at 216 Hz; R12 tol 0.1 dB; 8/8 sections) |
| Tilt Shelf | 108864 | 73.529 % (28627 miss) | 720: 64.31 dB | 0.000 % | 72.6 % | 64.307 | Tilt Shelf, 72 dB/oct, f0 25803.2 Hz, gain -15 dB, Q 10, fs 44.1 kHz (worst at 19053 Hz; R12 tol TBD dB; 6/6 sections) |
| Flat Tilt | 2016 | 82.937 % (344 miss) | 0: 0.00 dB | 0.000 % | 82.1 % | 0.405 | Flat Tilt, 6 dB/oct, f0 5120.0 Hz, gain +30 dB, Q 1, fs 44.1 kHz (worst at 7035 Hz; R12 tol 0.1 dB; 10/10 sections) |
| All Pass | 13608 | 100.000 % (0 miss) | 90: 0.00 dB | 0.000 % | 100.0 % | 0.000 | All Pass, 96 dB/oct, f0 10.0 Hz, gain +0 dB, Q 40, fs 384.0 kHz (worst at 10 Hz; R12 tol 0.1 dB; 8/8 sections) |
| **All** | 505512 | **74.072 %** (130206 miss) | 3330: 64.31 dB | 0.000 % | 73.3 % | 82.784 | |

Not evaluated (Brickwall, a Phase 2 design): 3024 cases.

## Bilinear with pre-warp at f0 (fallback) (comparison), by shape

| Shape | Cases | **Revised R12 pass** | TBD cases (Q > 3, f0 > 0.9 Nyq): max error | Uses correction section | Pass <= 0.10 dB everywhere | Max error (dB) | Worst case |
| --- | ---: | ---: | --- | ---: | ---: | ---: | --- |
| Bell | 108864 | 70.402 % (32008 miss) | 720: 29.92 dB | 0.000 % | 70.1 % | 30.000 | Bell, 96 dB/oct, f0 20480.0 Hz, gain -30 dB, Q 3, fs 44.1 kHz (worst at 19454 Hz; R12 tol 1.1 dB; 8/8 sections) |
| Low Shelf | 108864 | 75.984 % (25972 miss) | 720: 41.26 dB | 0.000 % | 75.1 % | 41.263 | Low Shelf, 24 dB/oct, f0 20480.0 Hz, gain -30 dB, Q 40, fs 44.1 kHz (worst at 19648 Hz; R12 tol TBD dB; 2/2 sections) |
| Low Cut | 13608 | 70.062 % (4047 miss) | 90: 61.27 dB | 0.000 % | 69.6 % | 61.271 | Low Cut, 96 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 40, fs 44.1 kHz (worst at 19640 Hz; R12 tol TBD dB; 8/8 sections) |
| High Shelf | 108864 | 75.984 % (25972 miss) | 720: 41.26 dB | 0.000 % | 75.1 % | 41.263 | High Shelf, 24 dB/oct, f0 20480.0 Hz, gain -30 dB, Q 40, fs 44.1 kHz (worst at 19648 Hz; R12 tol TBD dB; 2/2 sections) |
| High Cut | 13608 | 47.951 % (7036 miss) | 90: 17.04 dB | 0.000 % | 47.4 % | 52.602 | High Cut, 96 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 40, fs 48.0 kHz (worst at 21600 Hz; R12 tol 0.1 dB; 8/8 sections) |
| Notch | 13608 | 66.556 % (4521 miss) | 90: 32.10 dB | 0.000 % | 66.5 % | 60.000 | Notch, 96 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 3, fs 44.1 kHz (worst at 19092 Hz; R12 tol 1.1 dB; 8/8 sections) |
| Band Pass | 13608 | 37.025 % (8513 miss) | 90: 60.00 dB | 0.000 % | 37.0 % | 60.000 | Band Pass, 96 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 3, fs 44.1 kHz (worst at 19698 Hz; R12 tol 1.1 dB; 8/8 sections) |
| Tilt Shelf | 108864 | 75.984 % (25972 miss) | 720: 41.26 dB | 0.000 % | 75.1 % | 41.263 | Tilt Shelf, 24 dB/oct, f0 20480.0 Hz, gain -30 dB, Q 40, fs 44.1 kHz (worst at 19648 Hz; R12 tol TBD dB; 2/2 sections) |
| Flat Tilt | 2016 | 52.976 % (948 miss) | 0: 0.00 dB | 0.000 % | 52.5 % | 9.250 | Flat Tilt, 6 dB/oct, f0 30000.0 Hz, gain -30 dB, Q 1, fs 48.0 kHz (worst at 1532 Hz; R12 tol 1.1 dB; 10/10 sections) |
| All Pass | 13608 | 100.000 % (0 miss) | 90: 0.00 dB | 0.000 % | 100.0 % | 0.000 | All Pass, 96 dB/oct, f0 10.0 Hz, gain +0 dB, Q 40, fs 384.0 kHz (worst at 10 Hz; R12 tol 0.1 dB; 8/8 sections) |
| **All** | 505512 | **73.120 %** (134989 miss) | 3330: 61.27 dB | 0.000 % | 72.5 % | 61.271 | |

Not evaluated (Brickwall, a Phase 2 design): 3024 cases.

## Matched design by region (All Pass excluded)

Each cell: pass rate / max error (dB). "n/a" = shapes without Q (Flat Tilt).

| Centre f0 | Q <= 1 | 1 < Q <= 5 | Q > 5 | n/a |
| --- | --- | --- | --- | --- |
| f0 < 0.1 Nyq | 100.0 % / 0.093 | 100.0 % / 0.093 | 100.0 % / 0.095 | 100.0 % / 0.033 |
| 0.1-0.25 Nyq | 100.0 % / 0.094 | 100.0 % / 0.089 | 100.0 % / 0.091 | 100.0 % / 0.033 |
| 0.25-0.5 Nyq | 100.0 % / 0.090 | 100.0 % / 0.090 | 100.0 % / 0.090 | 100.0 % / 0.033 |
| 0.5-0.9 Nyq | 100.0 % / 0.089 | 100.0 % / 0.088 | 99.9 % / 0.119 | 100.0 % / 0.033 |
| 0.9-1.0 Nyq | 94.3 % / 0.448 | 93.5 % / 1.025 | 89.7 % / 10.727 | 100.0 % / 0.026 |
| f0 >= Nyq | 96.4 % / 0.238 | 96.9 % / 0.214 | 93.9 % / 28.191 | 100.0 % / 0.026 |

## Matched design by sample rate (All Pass excluded)

| Sample rate | Cases | Pass | Max error (dB) |
| --- | ---: | ---: | ---: |
|   44.1 kHz | 70272 | 99.6 % | 28.191 |
|   48.0 kHz | 70272 | 99.8 % | 0.736 |
|   88.2 kHz | 70272 | 100.0 % | 0.090 |
|   96.0 kHz | 70272 | 100.0 % | 0.091 |
|  176.4 kHz | 70272 | 100.0 % | 0.094 |
|  192.0 kHz | 70272 | 100.0 % | 0.093 |
|  384.0 kHz | 70272 | 100.0 % | 0.095 |

Cases where at least one section's 3-point match was unrealisable and clamped: 2892

## Matched design pass rate by shape and centre frequency

| Shape | f0 < 0.1 Nyq | 0.1-0.25 Nyq | 0.25-0.5 Nyq | 0.5-0.9 Nyq | 0.9-1.0 Nyq | f0 >= Nyq |
| --- | --- | --- | --- | --- | --- | --- |
| Bell | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.12 | 97.9 % / 0.14 | 99.8 % / 0.11 |
| Low Shelf | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.11 | 92.8 % / 1.67 | 94.6 % / 15.68 |
| Low Cut | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.09 | 66.7 % / 0.77 | 79.6 % / 0.24 |
| High Shelf | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.11 | 91.9 % / 10.73 | 94.5 % / 28.19 |
| High Cut | 100.0 % / 0.10 | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.08 | 100.0 % / 0.06 | 99.5 % / 0.24 |
| Notch | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.08 | 79.6 % / 0.45 | 100.0 % / 0.05 |
| Band Pass | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.09 | 87.0 % / 0.29 | 98.1 % / 0.73 |
| Tilt Shelf | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.09 | 100.0 % / 0.11 | 92.6 % / 0.92 | 94.4 % / 27.62 |
| Flat Tilt | 100.0 % / 0.03 | 100.0 % / 0.03 | 100.0 % / 0.03 | 100.0 % / 0.03 | 100.0 % / 0.03 | 100.0 % / 0.03 |
| All Pass | 100.0 % / 0.00 | 100.0 % / 0.00 | 100.0 % / 0.00 | 100.0 % / 0.00 | 100.0 % / 0.00 | 100.0 % / 0.00 |

## All Pass phase at the centre frequency (bilinear, pre-warped at f0)

Cases with f0 below the limit: 13338, within 2 degrees: 13338, max error: 1.49e-04 degrees.

## 25 worst matched cases

| Error (dB) | Case |
| ---: | --- |
| 28.191 | High Shelf, 18 dB/oct, f0 25803.2 Hz, gain +30 dB, Q 40, fs 44.1 kHz (worst at 14482 Hz; R12 tol TBD dB; 2/2 sections) |
| 27.617 | Tilt Shelf, 30 dB/oct, f0 25803.2 Hz, gain +30 dB, Q 40, fs 44.1 kHz (worst at 18246 Hz; R12 tol TBD dB; 3/3 sections) |
| 27.617 | High Shelf, 30 dB/oct, f0 25803.2 Hz, gain +30 dB, Q 40, fs 44.1 kHz (worst at 18246 Hz; R12 tol TBD dB; 3/3 sections) |
| 15.679 | Low Shelf, 30 dB/oct, f0 25803.2 Hz, gain -30 dB, Q 10, fs 44.1 kHz (worst at 18246 Hz; R12 tol TBD dB; 3/3 sections) |
| 15.679 | High Shelf, 30 dB/oct, f0 25803.2 Hz, gain +30 dB, Q 10, fs 44.1 kHz (worst at 18246 Hz; R12 tol TBD dB; 3/3 sections) |
| 15.679 | Tilt Shelf, 30 dB/oct, f0 25803.2 Hz, gain +30 dB, Q 10, fs 44.1 kHz (worst at 18246 Hz; R12 tol TBD dB; 3/3 sections) |
| 15.323 | Low Shelf, 30 dB/oct, f0 25803.2 Hz, gain -30 dB, Q 40, fs 44.1 kHz (worst at 18246 Hz; R12 tol TBD dB; 3/3 sections) |
| 10.727 | High Shelf, 36 dB/oct, f0 20480.0 Hz, gain +15 dB, Q 40, fs 44.1 kHz (worst at 18806 Hz; R12 tol TBD dB; 3/3 sections) |
| 8.645 | High Shelf, 30 dB/oct, f0 20480.0 Hz, gain +6 dB, Q 40, fs 44.1 kHz (worst at 18463 Hz; R12 tol TBD dB; 3/3 sections) |
| 3.163 | High Shelf, 96 dB/oct, f0 20480.0 Hz, gain +30 dB, Q 40, fs 44.1 kHz (worst at 15336 Hz; R12 tol TBD dB; 8/8 sections) |
| 1.665 | Low Shelf, 36 dB/oct, f0 20480.0 Hz, gain -30 dB, Q 10, fs 44.1 kHz (worst at 18023 Hz; R12 tol TBD dB; 3/3 sections) |
| 1.025 | High Shelf, 48 dB/oct, f0 20480.0 Hz, gain +30 dB, Q 3, fs 44.1 kHz (worst at 17984 Hz; R12 tol 1.1 dB; 4/4 sections) |
| 0.995 | High Shelf, 36 dB/oct, f0 20480.0 Hz, gain +30 dB, Q 10, fs 44.1 kHz (worst at 18023 Hz; R12 tol TBD dB; 3/3 sections) |
| 0.919 | Tilt Shelf, 36 dB/oct, f0 20480.0 Hz, gain +30 dB, Q 10, fs 44.1 kHz (worst at 18023 Hz; R12 tol TBD dB; 3/3 sections) |
| 0.862 | Tilt Shelf, 48 dB/oct, f0 20480.0 Hz, gain +30 dB, Q 3, fs 44.1 kHz (worst at 17984 Hz; R12 tol 1.1 dB; 4/4 sections) |
| 0.768 | Low Cut, 18 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 40, fs 44.1 kHz (worst at 2071 Hz; R12 tol TBD dB; 2/2 sections) |
| 0.736 | Tilt Shelf, 12 dB/oct, f0 30000.0 Hz, gain -6 dB, Q 40, fs 48.0 kHz (worst at 21600 Hz; R12 tol TBD dB; 1/1 sections) |
| 0.736 | Low Shelf, 12 dB/oct, f0 30000.0 Hz, gain +6 dB, Q 40, fs 48.0 kHz (worst at 21600 Hz; R12 tol TBD dB; 1/1 sections) |
| 0.736 | High Shelf, 12 dB/oct, f0 30000.0 Hz, gain -6 dB, Q 40, fs 48.0 kHz (worst at 21600 Hz; R12 tol TBD dB; 1/1 sections) |
| 0.732 | Band Pass, 18 dB/oct, f0 25803.2 Hz, gain +0 dB, Q 10, fs 44.1 kHz (worst at 19303 Hz; R12 tol TBD dB; 1/1 sections) |
| 0.732 | Band Pass, 12 dB/oct, f0 25803.2 Hz, gain +0 dB, Q 10, fs 44.1 kHz (worst at 19303 Hz; R12 tol TBD dB; 1/1 sections) |
| 0.732 | Band Pass, 6 dB/oct, f0 25803.2 Hz, gain +0 dB, Q 10, fs 44.1 kHz (worst at 19303 Hz; R12 tol TBD dB; 1/1 sections) |
| 0.483 | Low Cut, 12 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 10, fs 44.1 kHz (worst at 19331 Hz; R12 tol TBD dB; 1/1 sections) |
| 0.448 | Notch, 6 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 0.1, fs 44.1 kHz (worst at 19845 Hz; R12 tol 1.1 dB; 1/1 sections) |
| 0.448 | Notch, 18 dB/oct, f0 20480.0 Hz, gain +0 dB, Q 0.1, fs 44.1 kHz (worst at 19845 Hz; R12 tol 1.1 dB; 1/1 sections) |
