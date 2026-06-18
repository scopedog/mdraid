# Why kmec rand-4K-write is slow, and rebuild isn't (2026-04-27)

Following up on the headline numbers in `bench_2026-04-27.md`:

|                          | stock raid6 | kmec        |
|--------------------------|------------:|------------:|
| rand 4K write @iodepth=8 | 87 MB/s     | 39 MB/s     |
| rebuild (256 MiB / disk) | 1.86 s      | 1.19 s      |

The rand-write gap is real and structural; the rebuild story is different
from what the table suggests — kmec is actually 56 % faster per-byte
(232 MB/s vs raid6's 138 MB/s), it's just that "1.19 s" looks slow next
to anybody's expectations for a small array on virtio.

## Why kmec rand-4K-write is slow: fixed parity placement

iostat output during kmec rand-4K-write at iodepth=8 (8-second run, 75369
write ops at 38.6 MB/s):

| device | reads  | writes | util |
|--------|--------|--------|------|
| loop1  | 18 820 | 18 809 | 52 % |
| loop2  | 18 907 | 18 896 | 51 % |
| loop3  | 18 813 | 18 800 | 52 % |
| loop0  | 18 886 | 18 864 | 51 % |
| loop4  | **75 372** | **75 369** | **88 %** |
| loop5  | **75 372** | **75 369** | **88 %** |

The two parity disks (loop4, loop5) are doing 4× the I/O of any data
disk and saturate first — 88 % util while the data disks idle at ~51 %.

raid6 doing the same workload at iodepth=8 (85 MB/s, 163 817 write ops):

| device | reads  | writes | util |
|--------|--------|--------|------|
| loop0  | 78 699 | 82 738 | 77 % |
| loop1  | 79 194 | 82 208 | 76 % |
| loop2  | 78 853 | 82 710 | 75 % |
| loop3  | 78 435 | 83 196 | 76 % |
| loop4  | 78 276 | 83 228 | 75 % |
| loop5  | 78 300 | 83 229 | 75 % |

raid6 distributes load evenly across all six disks at 75–77 % util.
Why?  **raid6 rotates parity per stripe** (left-symmetric algorithm, the
default).  Every disk takes a turn holding P or Q, so over many stripes
the load averages out.  kmec's `tools/kmec-create.sh` lays out parity
fixed on the last m slots — it's RAID4-style for k+m, the simplification
called out in the file header comment of `kmec_main.c`.

Algebra of small-write RMW with k=4 m=2:

- A 4 KiB write touches **one** chunk on **one** data disk.
- Updating parity needs: read 1 OLD data + 2 OLD parity = 3 reads;
  write 1 NEW data + 2 NEW parity = 3 writes.  Six ops per user write.

Distribution across the array:

|                | per-write ops on this disk | per-write ops total |
|----------------|----:|----:|
| **kmec, fixed parity** (data slot d ∈ 0..3, parity slots 4–5) |||
| any data disk d | 1 read + 1 write if d hit, 0 otherwise | 2 (across 4 candidates → expected 0.5/disk) |
| each parity disk | 1 read + 1 write **always**           | 4 |
| **raid6 with rotation** (data + parity rotate per stripe) |||
| any disk        | 1 read + 1 write if it's the modified data **or** one of the parity slots, 0 otherwise | 6 ops spread across 6 disks → 1/disk |

The math reproduces the iostat: kmec's parity disks see 4× the per-disk
load of data disks (matching the 4:1 reads/writes ratio observed).
raid6's load is uniform.

This is the classic RAID4-vs-RAID5/6 tradeoff and is well-known.

iodepth scaling for kmec:

| iodepth | numjobs | MB/s |
|---:|---:|---:|
| 8   | 1 | 38.6 |
| 64  | 1 | 54.4 |
| 8   | 4 | 56.4 |

Higher queue depth helps a little (the cross-stripe pipeline picks up
parallelism the workqueue would otherwise leave on the table), but the
gain is bounded by parity-disk saturation: until the parity disks are
swapped out, more in-flight stripes just queue behind them.

### What would close the gap

1. **Parity rotation.**  ~~Add a layout that rotates parity per stripe~~
   **DONE 2026-04-28.**  kmec now rotates by `disk = (slot +
   stripe_idx) mod (k+m)`.  The post-rotation iostat at iodepth=8
   shows the expected uniform load:

   | device | reads  | writes | util |
   |--------|--------|--------|------|
   | loop0–loop5 | ~34 700 | ~34 700 | **~72 %** (uniform) |

   vs the pre-rotation pattern where loop4/loop5 were pinned at 88 %
   while data disks idled at 51 %.  Throughput on this single-virtio-
   loop test bench is unchanged because all six loops share one
   underlying device — rotation moved the bottleneck off "fixed
   parity disks" but the system was already capped by the underlying
   virtio device.

   ### Hardware extrapolation: which workloads rotation helps

   | Workload | Loop bench Δ | Real-disk Δ (predicted) | Why |
   |---|---:|---:|---|
   | rand 4K write | 0 % | **≈ 3×** | 6/2 disks shouldering parity load (was 2 fixed parities pinned) |
   | rand 4K read | 0 % | **≈ 1.5×** | 6/4 disks shouldering data reads (was 4 fixed data disks; parity disks were idle) |
   | seq write 256K | 0 % | 0 % | already touches every disk per stripe regardless |
   | seq read 256K | 0 % | 0 % | same |
   | scrub | 0 % | 0 % | reads/writes every disk every stripe |
   | rebuild | 0 % | 0 % | same |

   Anything that touches **all** disks per stripe (full-stripe
   write, sequential read, scrub, rebuild) gets no benefit from
   rotation — they were already balanced.  The two wins are exactly
   the workloads that touch a *subset* per request: random sub-
   stripe write (was capped by m fixed parity disks) and clean
   random read (was capped by k fixed data disks; parity disks
   contributed nothing).

2. **Async stripe state machine.**  raid6's stripe cache lets a stripe
   progress through phases (read → compute → write) without blocking a
   worker between phases — many more stripes are "in flight" simultaneously
   than there are workers.  kmec keeps the worker blocked across each
   stripe, so concurrency is bounded by `nr_cpu_ids` (4 vCPUs in this
   test, so at most 4 in-flight stripes).  With async chaining, hundreds.

3. **Bitmap.**  A write-intent bitmap would skip RMW for already-dirty
   stripes during scrub/rebuild, but doesn't help steady-state random
   writes.

For this PoC the fixed-parity layout is documented as a deliberate
simplification — it makes the encoding/decoding math easier to follow
and keeps the make_request → io_one_stripe path linear.  Adding parity
rotation is the natural next step if the rand-4K throughput matters.

## Why "rebuild" isn't actually slow

The 1.19 s figure is for rebuilding **one disk** worth of data — 256 MiB
on these loop-backed arrays.

Per-byte rates:

|                                    | wallclock | rate      |
|------------------------------------|----------:|----------:|
| kmec rebuild (256 MiB / disk)      | 1.19 s    | 232 MB/s  |
| stock raid6 rebuild (256 MiB / disk) | 1.86 s   | 138 MB/s  |

kmec is **56 % faster per-byte** than raid6 here.  Sampling
`sync_speed` / `sync_completed` during the kmec rebuild:

```
[t=0.0s] action=recover  done=0      / 524288 sectors
[t=0.1s] action=recover  done=32896
[t=0.2s] action=recover  done=98688
[t=0.3s] action=recover  done=131584
[t=0.4s] action=recover  done=197376
[t=0.5s] action=recover  done=263168
[t=0.6s] action=recover  done=296064
[t=0.7s] action=recover  done=361856
[t=0.8s] action=recover  done=394752
[t=0.9s] action=recover  done=460544
[t=1.0s] action=recover  done=493440
[t=1.1s] action=idle
```

The sync walks 1024 stripes (524 288 sectors / 512 sectors per chunk)
in 1.1 s — about 930 stripes/s, or 1.07 ms per stripe.  Each stripe
does k surviving-disk reads, a GFNI decode, then 1+m writes — roughly
seven disk ops and ~64 KiB of GFNI math per stripe.  ~155 µs per disk
op on virtio is in the right ballpark.

Where it could still be faster:

- **Cross-stripe parallelism in sync_request.**  Today md_do_sync calls
  our `kmec_sync_request` stripe-by-stripe in a single thread; the
  per-stripe parallel I/O via `kmec_io_multi` shaves wall time within a
  stripe but does not pipeline consecutive stripes.  Issuing several
  stripes' worth of sync work to `system_unbound_wq` (the same approach
  the user-I/O path took with `kmec_user_ctx`) would let the rebuild
  fan out to nr_cpu_ids workers in parallel — expected speedup roughly
  proportional to vCPU count.

- **Tighter sync chunking.**  Each sync_request returns `chunk_sectors`
  per call — md_do_sync re-enters every 64 KiB.  Returning a window of
  several chunks per call (the same idea raid5_sync_request uses with
  `RAID5_SYNC_WINDOW`) would reduce md_do_sync's per-call overhead.

Either fix is a small, well-bounded change.  Today's number isn't
"slow" relative to raid6 — it's just bottlenecked by single-threaded
sync iteration, not by the kmec I/O path itself.
