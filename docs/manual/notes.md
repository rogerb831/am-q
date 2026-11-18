# Am-Q manual notes

Text the user manual must carry, collected as the plan settles it. Each section names the plan
requirement it comes from.

## Brickwall cuts in Natural Phase (area 03 R16)

In Natural Phase, Am-Q corrects the phase of its filters so they behave more like analog filters
near the top of the spectrum. Low Cut and High Cut bands set to the Brickwall slope are the
exception: they are left out of that correction and keep their normal (minimum-phase) response.

What this means in use:

- A Brickwall cut sounds and measures the same in Natural Phase as in Zero Latency, only delayed
  by Natural Phase's latency, like everything else in that mode.
- Its steep edge rings slightly after transients, as a steep minimum-phase filter does. Natural
  Phase does not change that.
- For a steep cut with no phase shift at all, use Linear Phase mode instead, which handles
  Brickwall cuts like every other band (with pre-ringing instead of post-ringing, and more
  latency).
