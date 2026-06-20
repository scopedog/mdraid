# mdraid

> **Build note:** This is one component of the raidkm mdraid stack and is not
> meant to be built on its own. Please use
> [mdraid-super](https://github.com/scopedog/mdraid-super) to build
> the entire package — it assembles this repo together with the other
> components in the correct order.

Out-of-tree Linux kernel module with targeted improvements to `drivers/md/`
(md, raid5, raid6), focused on RAID-5/6 performance and rebuild throughput for
modern storage.

**On-disk compatible with stock mdraid.** All changes here are
execution-side — stripe layout, parity math, and superblock format
match the upstream md core byte-for-byte. An array written by this
build can be assembled and read by a stock kernel, and vice versa,
with no conversion. This includes the optional `raid_isal.ko` GFNI
override for raid6 syndrome (verified byte-identical against the
in-tree implementation at module load time).

For arbitrary k+m Reed-Solomon erasure coding (m > 2) — the layout
needed for high-durability storage targets — see the companion repo
[md-kmec](https://github.com/scopedog/md-kmec). md-kmec
is *not* on-disk compatible with stock mdraid (it adds a new md
personality, level=70) and consumes `isal_lib.ko` exported from
this tree.

Base: upstream Linux 6.12 `drivers/md/` with a RHEL 10 compatibility shim
(`md/compat-rhel10.h`) for building against RHEL 10.x kernel headers.

## Changes

| # | Change | Files | Impact |
|---|--------|-------|--------|
| 1 | `chunk_sectors` / stripe cache counts: `int` → `unsigned int` | md.h, raid5.h, raid5.c, md.c | Enables compiler power-of-two shift optimization in hot-path divisions |
| 2 | Stripe cache `max_nr_stripes` limit: 32 768 → 262 144 | raid5.h, raid5.c | Allows larger stripe caches on servers with 64 GB+ RAM |
| 3 | `recovery_active`: `atomic_t` → `atomic64_t`; `md_done_sync` blocks: `int` → `sector_t` | md.h, md.c | Prevents 32-bit wrap on >1 TB of in-flight recovery I/O |
| 4 | Badblock `sectors` param: `int` → `sector_t` | md.h, md.c | Correctness for >1 TB badblock regions on large drives |
| 5 | Bitmap `bytes`/`chunks`: `unsigned long` → `u64` | md-bitmap.c, md-bitmap.h | Portability; eliminates latent 32-bit overflow |
| 6 | `NR_STRIPE_HASH_LOCKS` documented at 32 (already upstream) | raid5.h | Reduces stripe-hash lock contention on 32-core systems; 64 blocked by kernel lock-depth limit |
| 7 | `stripe_head` hot/cold field split + `SLAB_HWCACHE_ALIGN` | raid5.h, raid5.c | Reduces false sharing of hot state fields under concurrent I/O |
| 8 | Recovery read-ahead: submit window of `RAID5_SYNC_WINDOW=128` stripes per call | raid5.c | Eliminates per-call overhead and jiffy sleep bottleneck during rebuild |
| 9 | NUMA-aware worker group allocation (`kzalloc_node` per group) | raid5.c | Eliminates remote-memory traffic for `r5worker` structs on multi-socket systems |
| 10 | `MAX_STRIPE_BATCH`: 8 → 32 (separate `STRIPE_BATCH_WORKERS=8` for spawn heuristic) | raid5.h, raid5.c | Reduces lock/context-switch overhead per unit of work; preserves worker spawn sensitivity |
| 11 | Rebuild stripe-cache high-water mark (`RAID5_SYNC_HWMARK=2`, gated on `preread_active_stripes`) | raid5.h, raid5.c | Caps rebuild at 50% of stripe cache only when user I/O is competing; no penalty when array is idle |
| 3c | Remove default rebuild speed cap; reduce throttle sleep 500 ms → 50 ms | md.c | Uncaps rebuild on modern storage; ~2× rebuild throughput on VM |
| 3b | Partial drain on not-idle: wait for 50% in-flight instead of zero | md.c | Maintains rebuild pipeline during concurrent user I/O |
| 3a | Parallel rebuild workers via `sync_thread_cnt` sysfs knob (default 1) | md.c, md.h | Removed: the `sync_thread_cnt` field shifted struct mddev's `bitmap_ops` 8 bytes past RHEL's offset, causing NULL-deref crashes in `raid5_sync_request` and `raid5_read_one_chunk`. Values > 1 were already experimental (31× regression at 4 threads from spinlock serialisation), so the field was removed for binary ABI parity with RHEL's md core |
| 12 | `find_get_stripe`: skip `device_lock` for idle stripes via `STRIPE_ON_INACTIVE_LIST` flag | raid5.c, raid5.h | When a `find_get_stripe()` lookup succeeds but the stripe is idle (count==0) on `inactive_list[hash]`, the unlink can be done under `hash_locks[hash]` alone — that bucket is already protected by it.  A new `STRIPE_ON_INACTIVE_LIST` state bit, set in `release_inactive_stripe_list()` right before the splice and cleared by `get_free_stripe()` / the fast path itself, distinguishes "on inactive_list" from "still on temp_inactive_list" (which would need device_lock).  Earlier device_lock-skip without the flag (commit `0706ca7`, reverted in `dc17c72`) tripped `__list_add_valid_or_report` because the state alone couldn't tell those two list locations apart. |
| 13 | ISA-L GFNI `gen_syndrome` override via `raid_isal.ko` | isa-l/raid6_isal.c, isa-l/Kbuild | Routes RAID-6 P+Q syndrome through ISA-L's `gf_vect_dot_prod_avx2_gfni` when host CPU supports GFNI; in-tree `lib/raid6` (avx2x4 etc.) is used as a byte-for-byte reference at install time |
| 15 | Auto-default `group_thread_cnt` for new raid4/5/6 arrays | raid5.c | Stock kernel disables Shaohua Li's r5worker groups by default (`group_thread_cnt=0`), funneling all `handle_stripe()` work through the single `raid5d` kthread which pegs ~75 % of one core under load. New arrays now start with `group_thread_cnt = min(2, num_online_cpus()/2)` so worker offload is on out of the box. Override via the `default_group_thread_cnt` module parameter (`-1` auto, `0` disabled, `N` literal). Per-array sysfs `group_thread_cnt` is unchanged. Validated on the README workload set (rd07 GFNI bhyve guest): seq-write +24 %, scrub +51 %, rebuild +34 %; rand-4K and seq-read within noise. With `raid_isal` layered on top: seq-write +11 %, scrub +31 %, rebuild +43 %. |


## Benchmark results

### raid6 vs stock — 2026-04-28 (with #12 + #15)

Comparison on an Intel i5-1340P (Raptor Lake, GFNI) running a bhyve
Linux guest with 4 vCPUs, kernel 6.12.0-124.8.1.el10_1.  6× 256 MiB
sparse-file loop devices, 64 KiB chunk, 1 GiB array, fio --direct=1
--iodepth=8 --runtime=8s --bs as shown.  All loops back to one XFS
file on a single virtio disk, so per-disk physical parallelism is
**not** measured here — the numbers reflect kernel-side EC work,
lock/scheduling overhead, and stripe pipelining.  Columns A' and B'
show changes #15 (auto-default `group_thread_cnt`) and #12
(`find_get_stripe` device_lock skip via `STRIPE_ON_INACTIVE_LIST`)
layered on top of stock raid6 and raid_isal.

| Workload                    | bs   | A. stock raid6 | A'. ours raid6 (#12 + #15) | B. stock + raid_isal | B'. ours + raid_isal |
|-----------------------------|------|---------------:|---------------------------:|---------------------:|---------------------:|
| seq write (full-stripe)     | 256K | 1539 MB/s      | **1870 MB/s** (+22 %)      | 1663 MB/s            | **1885 MB/s** (+13 %)|
| seq read                    | 256K | **3347 MB/s**  | 3204 MB/s                  | 3121 MB/s            | **3367 MB/s** (+8 %) |
| rand write (sub-stripe RMW) | 4K   | 90 MB/s        | 93 MB/s                    | 92 MB/s              | **94 MB/s**          |
| rand read                   | 4K   | **147 MB/s**   | 140 MB/s                   | 126 MB/s             | **157 MB/s** (+25 %) |
| scrub (256 MiB / disk)      | -    | 272 MB/s       | **375 MB/s** (+38 %)       | 322 MB/s             | **406 MB/s** (+26 %) |
| rebuild (256 MiB / disk)    | -    | 254 MB/s       | **347 MB/s** (+37 %)       | 250 MB/s             | **356 MB/s** (+42 %) |

Scrub and rebuild numbers are 3-run averages timed via `dmesg`
start/end timestamps (`md: check of RAID array …` →
`md: md127: check done.`), which is robust against sub-second
operations on tmpfs that ordinary `/proc/mdstat` polling would
miss.

**Headline wins on raid6 (A → A'):** seq write **+22 %**, scrub
**+38 %**, rebuild **+37 %**.  Read and rand-4K paths are within
noise — the existing `raid5d` single-thread bottleneck mainly
hurts the write hot path.  With `raid_isal` layered on top
(B → B'): seq write **+13 %**, scrub **+26 %**, rebuild **+42 %**.

The two changes hit different surfaces.  #15 (auto-GTC) takes the
single `raid5d` core out of the seq-write critical path by
spreading `handle_stripe()` across r5worker groups; that's where
the seq-write and recovery wins concentrate.  #12 (find_get_stripe
fast rescue) shaves `device_lock` off the idle-stripe lookup hot
path; on this single-virtio-loop bench the I/O is disk-limited so
#12's contribution is mostly invisible, but it removes a real
single-core lock bottleneck that shows up under stripe-cache
pressure on real arrays.

raid_isal.ko (GFNI override of the raid6 syndrome) is essentially
indistinguishable from stock raid6 on this single-virtio-loop
setup — the GFNI math isn't the bottleneck here.  Its wins live
elsewhere (real disks, high-IOPS workloads, AVX-512 hosts).

### raid456 changes (#1–#12) — earlier 4-phase benchmark

3-run benchmark on RHEL 10 / Linux 6.12.0, virtio block, 3-disk RAID-5, 512 KB chunk.
Four phases: (1) healthy I/O, (2) rebuild with no user I/O, (3) user I/O during
speed-capped rebuild, (4) rebuild competing with uncapped user I/O.

**All changes combined (raid456.ko + md-mod.ko) vs stock:**

| Phase | Metric | Stock | This repo | Delta |
|-------|--------|-------|-----------|-------|
| 1 — healthy I/O | READ | 661 MB/s | **732 MB/s** | +10.7% |
| 1 — healthy I/O | WRITE | 662 MB/s | **732 MB/s** | +10.6% |
| 2 — rebuild, no I/O | rebuild | 1090 MB/s | **1154 MB/s** | +5.9% |
| 3 — I/O + capped rebuild | READ | 394 MB/s | **404 MB/s** | +2.5% |
| 4 — rebuild + I/O | rebuild | 316 MB/s | **388 MB/s** | +22.8% |
| 4 — rebuild + I/O | fio READ | 396 MB/s | **409 MB/s** | +3.3% |
| 4 — rebuild + I/O | fio WRITE | 397 MB/s | **410 MB/s** | +3.3% |

Phase 4 (rebuild competing with live I/O) is the most contentious case and shows
the largest gain: +22.8% rebuild throughput with user I/O throughput unchanged.

## Building

Requires Linux kernel headers 6.12 or later.  RHEL 10.x is the primary tested
configuration.

```sh
# Check prerequisites
./configure

# Build (uses running kernel by default)
make

# Install
sudo make install

# Override kernel version or build directory
make KVER=6.12.0-124.8.1.el10_1.x86_64
make KDIR=/path/to/kernel/build
```

After `make install`, unload and reload the modules:

```sh
sudo modprobe -r raid456 md-mod
sudo modprobe md-mod raid456
```

Verify with `modinfo raid456 | grep filename` that the new module is loaded from
`/lib/modules/$(uname -r)/extra/` rather than the kernel tree.

### GFNI acceleration (`raid_isal.ko`)

`make` also builds `isa-l/raid_isal.ko`, a small module that overrides
`raid6_call.gen_syndrome` with ISA-L's GFNI implementation. It can be
loaded on top of either this repo's `raid456.ko` or a stock in-tree
`raid456.ko` — `raid6_call` is a writable global in `raid6_pq.ko` that
all consumers route through indirectly.

```sh
sudo modprobe raid456                       # autoloads raid6_pq + async_*
sudo insmod isa-l/isal_lib.ko               # shared ISA-L primitives (must be loaded first)
sudo insmod isa-l/raid_isal.ko
dmesg | tail -1
# expect: raid6_isal: installed GFNI gen_syndrome (was 'avx2x4'); xor_syndrome unchanged
```

At install, `raid_isal.ko` runs a self-validation that compares its
output byte-for-byte against the in-tree `gen_syndrome` and
`xor_syndrome` implementations the kernel selected at boot, for
`k = 1..8` (and several `[start, stop]` windows for the RMW path).
It refuses to install on mismatch, so `insmod` will fail cleanly
rather than corrupt parity.

Requires a CPU with GFNI (Intel Tiger Lake / Ice Lake-SP and later,
AMD Zen 5). On hosts without GFNI the module logs a notice and exits
with `-ENODEV`. Both full-stripe writes (`gen_syndrome`) and partial-
stripe RMW writes (`xor_syndrome`) are routed through GFNI, using
2vect kernels that compute P and Q in one inner-loop pass. AVX-512
variants are dispatched automatically on hosts that advertise
`AVX512F | AVX512BW | GFNI`; otherwise the module falls back to AVX2
2vect. dmesg shows which path was chosen:

```
raid6_isal: installed AVX2 GFNI gen_syndrome + xor_syndrome (was 'avx2x4')
raid6_isal: installed AVX-512 GFNI gen_syndrome + xor_syndrome (was 'avx2x4')
```

See `isa-l/raid6_isal.c` and the `notes/gfni_raid6_integration.md`
writeup for design and end-to-end test results.

### k+m Reed-Solomon personality

The k+m Reed-Solomon md personality (`kmec.ko`) that consumes the
GFNI primitives exported by `isal_lib.ko` lives in its own repo:
[scopedog/md-kmec](https://github.com/scopedog/md-kmec).
Build mdraid first, then md-kmec — kmec picks up `isal_lib.ko`'s
exports from this tree.

## Status and roadmap

| Item | Status |
|------|--------|
| Changes #1–#11 (raid5/md correctness + performance) | Done |
| Changes 3b+3c (rebuild pipeline, speed cap) | Done |
| Change 3a (parallel rebuild workers) | Removed — `sync_thread_cnt` deleted from struct mddev to match RHEL's binary ABI |
| Change #12 (`find_get_stripe` device_lock skip via `STRIPE_ON_INACTIVE_LIST`) | Done — fast path runs under `hash_locks[hash]` only when the new state bit confirms the stripe is on `inactive_list[hash]`; slow path keeps `device_lock` for everything else (handle/delayed/r5c/sync_pool/expanding/temp_inactive_list).  Earlier device_lock-skip without the flag (`0706ca7`, reverted) tripped `__list_add_valid_or_report` under fio writes; the flag closes that race.  Verified clean under heavy 4-job seq writes + randwrite + scrub. |
| Change #13 (`raid_isal.ko` GFNI override: `gen_syndrome`, `xor_syndrome`, 2vect AVX2, AVX-512 fallback) | Done — full-stripe write, RMW partial-stripe write, scrub, fail+rebuild all verified on RHEL 10 + bhyve + i5-1340P. AVX-512 path vendored and integrated but not runtime-tested (no Sapphire Rapids / Ice Lake-SP host); install-time self-validation byte-for-byte against the in-tree impl catches any regression before parity is written. |
| In-kernel arbitrary k+m Reed-Solomon primitives (`ec_encode_data_avx2_gfni` / `ec_encode_data_avx512_gfni` in `gfni_glue.c`) | Done — verified byte-for-byte against `ec_encode_data_base` for k up to 16, m up to 8. AVX-512 dispatcher built but unexercised (no AVX-512 test host). |
| Shared `isal_lib.ko` for the ec_base + GFNI primitives | Done — `raid_isal.ko` and `isal_test.ko` link only their unique `.o`; `isal_lib.ko` carries the shared code and exports the public surface via `EXPORT_SYMBOL_GPL`. Also consumed by the `md-kmec` repo. |
| Change #15 (auto-default `group_thread_cnt` for raid4/5/6) | Done — new arrays start at `min(2, ncpus/2)` so worker offload is on by default; `default_group_thread_cnt` module param overrides. Validated on rd07: seq-write +24 %, scrub +51 %, rebuild +34 % vs stock; deltas are larger again with `raid_isal` (#13) layered on top. Caps at 2 because raid5d retains ~45 % CPU even after offload (next bottleneck is its irreducible completion-path / `device_lock` work). |

## License

GPL-2.0-only.  This project is derived from the Linux kernel source tree
(`drivers/md/`).  See [LICENSE](LICENSE).
