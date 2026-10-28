# Shared benchmark capacity pool

Use the owner's installed scheduler and sibling registry/agent:

```powershell
& 'C:/Users/Matthew/.pyenv/pyenv-win/versions/3.12.5/python.exe' `
  'C:/Users/Matthew/.local/bin/benchmark_hosts.py' probe `
  --cpu 1.25 --gpu 20 --memory 1 --disk 1 --owner beads-mc7.74 --out headroom.json

& 'C:/Users/Matthew/.pyenv/pyenv-win/versions/3.12.5/python.exe' `
  'C:/Users/Matthew/.local/bin/benchmark_hosts.py' run --os linux `
  --cpu 1.25 --gpu 20 --memory 1 --disk 1 --owner beads-mc7.74 `
  --job-file workload.json --out reservation.json
```

The pool rotates this PC, SERVER and Steam Deck SSH. Both Deck aliases share
one physical host and ledger. Choose an OS/backend-compatible native workload;
verify its runtime before timing. Supply exact JSON `argv` and `env` in the job
file, especially on Windows, and declare realistic physical CPU-core, GPU%, RAM
GiB and workspace GiB costs. Adjust the sample budgets above to the actual job.
Missing required telemetry does not pass admission.

Capacity reservations are per physical host. Work may coexist when declared
budgets and measured headroom fit. Other games, compilers and services are not
automatic exclusions. Never require an otherwise idle machine, stop owner work
or force an old lock. Keep each complete matched A/B pair on one host/config.

Retain host telemetry, source/native-binary identities, settings and raw per-game
samples. Headroom does not establish a speed result; use the workload's A/A,
balanced-pair variation, correctness and per-game regression checks. Preserve
historical quiet-host gate files and receipts unchanged; label new capacity
protocol runs separately. Target rotation does not authorize extra benchmark
matrices or repeated accepted gameplay checks.

Keep automated work hidden/muted and native saves/settings intact. Track bounded
staging and clean disposable outputs when evidence is secured. The temporary
exclusive reservation implementation from commit975b7a3 is superseded; new jobs
must use the installed shared scheduler rather than recreating its ledger.
