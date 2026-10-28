# Performance test rotation

The owner authorized this Windows machine, SERVER and the Steam Deck as targets
on 2026-10-28. The campaign's local `fleet-targets.json` records its destinations;
keep personal paths and temporary campaign roots outside the source repository.
`fleet_probe.py --registry <fleet-targets.json> --output <new-receipt.json>` checks each independently. Choose any
eligible machine with a compatible native build; an occupied Windows host must
not prevent testing on an eligible Deck or SERVER.

Reservations belong to a **host**, not the whole fleet. Windows retains
`Local\Codex_Performance_Benchmark` and the held exclusive handle on the existing
empty `G:/Temp/Codex/.benchmark.lock`. Linux uses nonblocking `flock` on
`/home/deck/.cache/codex-performance/benchmark.lock`. Never truncate, remove or
replace these files to acquire a turn. A nonempty/unexpected file is preserved
and admission declines. Never terminate another console's test or owner game.

Use `host_reservation.lease()` around a complete workload/comparison and call
`headroom()` **inside** that lease immediately before launching. Fleet probes
release their lease; their results do not grant a future reservation.
`guarded_workload.py <spec.json> --output <new-receipt.json>` provides this
contract for a batch of native executables: it rechecks between runs, retains
one selected core, bounds each process, mutes unattended output and records
full stdout/stderr hashes plus execution load. Its successful execution status
does not establish workload correctness or a game speed verdict.

Admission requires two complete one-second samples with every sibling of the
selected physical core at or below20% CPU, GPU at or below10% when required,
workspace disk queue at or below2, and sufficient ready storage (default4GiB;
raise this to cover the workload's staging/output). Linux also checks IO
pressure, and records power and temperature. Missing required measurements
decline admission. Headless CPU-only workloads may explicitly observe rather
than require GPU headroom; record that distinction. Process names are evidence,
not admission exclusions: another game/compiler can run if measured headroom
and the subsequent comparison controls remain acceptable.

Preserve the workload's existing per-leg load, A/A, balanced-pair variation,
correctness and per-game regression controls. Continue observing load during
the actual workload. A ready host is not a performance result. Keep A/B on the
same host with matched features, power settings and inputs; record its native
binary identities. Windows EXEs require Windows; prepare and qualify native
Linux builds before counting Deck game results. Do not pool unlike hosts or
optimization packages. A reproducible regression for one game still blocks.

On busy hosts, preserve declined/partial receipts and recheck in about30minutes
or after an actual relevant resource release. Rotate immediately to a different
eligible host. Retain older frozen package helpers and evidence unchanged; new
runs use this revision and record the updated reservation protocol separately.
Keep all unattended work muted/hidden, owner saves/settings intact, and Deck
staging bounded. Do not change the Deck's system configuration or owner power
profile to create headroom.
