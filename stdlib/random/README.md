# Random

Import "stdlib/random". seeded(u64) constructs a deterministic full-period
64-bit linear congruential generator. next, nextUnit, below(nonzeroUpper) and
fill(&mut bytes) provide reproducible sequences. below uses rejection sampling
to avoid modulo bias. This simple generator is not cryptographic.

systemFill(&mut bytes) uses BCryptGenRandom on Windows and getrandom on Linux,
handling interrupted/partial Linux reads. Failure can leave partially filled
output; discard it on Err. OS entropy never falls back to the seeded generator.
