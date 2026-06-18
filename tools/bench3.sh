#!/bin/bash
# bench10.sh <label>
# 10-run benchmark: 4 phases
#   1. Normal I/O (healthy, no rebuild)
#   2. Rebuild speed (no I/O)
#   3. Normal I/O during rebuild (rebuild capped 200 MB/s)
#   4. Rebuild speed during normal I/O (rebuild unlimited, I/O concurrent)
# Expects: md127 healthy on vdc+vdd+vde, vdb free

LABEL=${1:-unnamed}
ARRAY=/dev/md127
VICTIM=/dev/vdc
SPARE=/dev/vdb
RUNS=3
FIO_ARGS="--filename=$ARRAY --bs=64k --numjobs=4 --ioengine=libaio --iodepth=16 \
    --direct=1 --group_reporting --output-format=terse"

stats() {
    awk 'BEGIN{n=0;s=0;s2=0;min=1e18;max=-1e18}
         {n++;s+=$1;s2+=$1*$1;if($1<min)min=$1;if($1>max)max=$1}
         END{mean=s/n; sd=sqrt(s2/n-mean*mean);
             printf "mean=%6.0f  sd=%5.0f  min=%6.0f  max=%6.0f  n=%d\n",
             mean,sd,min,max,n}'
}
bw_r()  { awk -F';' 'NR==1{printf "%.0f",$7/1024}'; }
bw_w()  { awk -F';' 'NR==1{printf "%.0f",$48/1024}'; }

add_spare_and_activate() {
    sudo mdadm --zero-superblock $SPARE 2>/dev/null; true
    sudo mdadm $ARRAY --fail   $VICTIM >/dev/null 2>&1; true
    sudo mdadm $ARRAY --remove $VICTIM >/dev/null 2>&1; true
    sudo mdadm $ARRAY --add    $SPARE  >/dev/null 2>&1; true
    sudo mdadm --readwrite $ARRAY      >/dev/null 2>&1; true
}

restore_and_wait() {
    sudo mdadm --zero-superblock $VICTIM 2>/dev/null; true
    sudo mdadm $ARRAY --fail   $SPARE  >/dev/null 2>&1; true
    sudo mdadm $ARRAY --remove $SPARE  >/dev/null 2>&1; true
    sudo mdadm $ARRAY --add    $VICTIM >/dev/null 2>&1; true
    sudo mdadm --readwrite $ARRAY      >/dev/null 2>&1; true
    while grep -qE 'recovery|resync' /proc/mdstat 2>/dev/null; do sleep 1; done
}

poll_rebuild_speed() {
    # Poll for up to $1 seconds, print speed samples, fill global SPEEDS array
    local limit=${1:-120}
    local t0=$SECONDS
    while true; do
        LINE=$(grep -A4 'md127' /proc/mdstat | grep -E 'recovery|resync' || true)
        [ -z "$LINE" ] && break
        S=$(echo "$LINE" | grep -oE 'speed=[0-9]+K' | grep -oE '[0-9]+' || true)
        [ -n "$S" ] && SPEEDS+=($S)
        [ $((SECONDS-t0)) -ge $limit ] && break
        sleep 1
    done
}

echo "============================================"
echo "  10-run benchmark: $LABEL"
echo "  $(date)"
echo "============================================"

# ── Phase 1: normal I/O ────────────────────────────────────────────────────
echo ""
echo "── Phase 1: normal I/O (${RUNS}×20s randrw 64k, healthy) ──"
P1_R=(); P1_W=()
for i in $(seq 1 $RUNS); do
    OUT=$(sudo fio --name=p1 $FIO_ARGS --rw=randrw --time_based --runtime=20 2>/dev/null)
    R=$(echo "$OUT" | bw_r); W=$(echo "$OUT" | bw_w)
    P1_R+=($R); P1_W+=($W)
    printf "  run %2d:  R %4d MB/s  W %4d MB/s\n" $i $R $W
done
echo "  READ  $(printf '%s\n' "${P1_R[@]}" | stats)"
echo "  WRITE $(printf '%s\n' "${P1_W[@]}" | stats)"

# ── Phase 2: rebuild speed (no I/O) ───────────────────────────────────────
echo ""
echo "── Phase 2: rebuild speed, no I/O (${RUNS} runs, unlimited) ──"
echo 100000000 | sudo tee /proc/sys/dev/raid/speed_limit_max >/dev/null
echo 0         | sudo tee /proc/sys/dev/raid/speed_limit_min >/dev/null

P2_AVGS=(); ALL_P2=()
for i in $(seq 1 $RUNS); do
    add_spare_and_activate
    sleep 3
    SPEEDS=()
    poll_rebuild_speed 120
    while grep -qE 'recovery|resync' /proc/mdstat 2>/dev/null; do sleep 1; done
    ALL_P2+=("${SPEEDS[@]}")

    if [ ${#SPEEDS[@]} -gt 0 ]; then
        SUM=0; for s in "${SPEEDS[@]}"; do SUM=$((SUM+s)); done
        AVG=$((SUM/${#SPEEDS[@]}))
        P2_AVGS+=($AVG)
        printf "  run %2d:  avg %7d K/sec (%4d MB/s)  n=%d\n" \
            $i $AVG $((AVG/1024)) ${#SPEEDS[@]}
    else
        printf "  run %2d:  completed < 3s\n" $i
    fi
    restore_and_wait
done
echo "  per-run avgs (K/sec): $(printf '%s\n' "${P2_AVGS[@]}" | stats)"
echo "  all samples  (K/sec): $(printf '%s\n' "${ALL_P2[@]}"  | stats)"

# ── Phase 3: normal I/O during rebuild (capped rebuild) ───────────────────
echo ""
echo "── Phase 3: normal I/O during rebuild (${RUNS}×20s, rebuild cap 200 MB/s) ──"
echo 200000 | sudo tee /proc/sys/dev/raid/speed_limit_max >/dev/null
echo 50000  | sudo tee /proc/sys/dev/raid/speed_limit_min >/dev/null

P3_R=(); P3_W=()
for i in $(seq 1 $RUNS); do
    add_spare_and_activate
    sleep 3
    if ! grep -qE 'recovery|resync' /proc/mdstat 2>/dev/null; then
        printf "  run %2d:  rebuild not running — skipped\n" $i
    else
        OUT=$(sudo fio --name=p3 $FIO_ARGS --rw=randrw --time_based --runtime=20 2>/dev/null)
        R=$(echo "$OUT" | bw_r); W=$(echo "$OUT" | bw_w)
        P3_R+=($R); P3_W+=($W)
        printf "  run %2d:  R %4d MB/s  W %4d MB/s\n" $i $R $W
    fi
    while grep -qE 'recovery|resync' /proc/mdstat 2>/dev/null; do sleep 1; done
    restore_and_wait
done
echo "  READ  $(printf '%s\n' "${P3_R[@]}" | stats)"
echo "  WRITE $(printf '%s\n' "${P3_W[@]}" | stats)"

# ── Phase 4: rebuild speed during normal I/O ──────────────────────────────
echo ""
echo "── Phase 4: rebuild speed during I/O (${RUNS} runs, unlimited) ──"
echo 100000000 | sudo tee /proc/sys/dev/raid/speed_limit_max >/dev/null
echo 0         | sudo tee /proc/sys/dev/raid/speed_limit_min >/dev/null

P4_AVGS=(); ALL_P4=(); P4_FIO_R=(); P4_FIO_W=()
for i in $(seq 1 $RUNS); do
    add_spare_and_activate
    sleep 3
    if ! grep -qE 'recovery|resync' /proc/mdstat 2>/dev/null; then
        printf "  run %2d:  rebuild not running — skipped\n" $i
        restore_and_wait
        continue
    fi

    # Run fio concurrently; poll rebuild speed in background
    SPEEDS=()
    sudo fio --name=p4 $FIO_ARGS --rw=randrw --time_based --runtime=20 \
        >/tmp/p4_fio.out 2>/dev/null &
    FIO_PID=$!
    poll_rebuild_speed 25   # poll while fio runs (~20s) + a little buffer
    wait $FIO_PID

    while grep -qE 'recovery|resync' /proc/mdstat 2>/dev/null; do sleep 1; done

    R=$(cat /tmp/p4_fio.out | bw_r); W=$(cat /tmp/p4_fio.out | bw_w)
    P4_FIO_R+=($R); P4_FIO_W+=($W)
    ALL_P4+=("${SPEEDS[@]}")

    if [ ${#SPEEDS[@]} -gt 0 ]; then
        SUM=0; for s in "${SPEEDS[@]}"; do SUM=$((SUM+s)); done
        AVG=$((SUM/${#SPEEDS[@]}))
        P4_AVGS+=($AVG)
        printf "  run %2d:  rebuild avg %7d K/sec (%4d MB/s)  fio R %4d MB/s  W %4d MB/s  n=%d\n" \
            $i $AVG $((AVG/1024)) $R $W ${#SPEEDS[@]}
    else
        printf "  run %2d:  no rebuild samples  fio R %4d MB/s  W %4d MB/s\n" $i $R $W
    fi
    restore_and_wait
done
echo "  rebuild avgs (K/sec): $(printf '%s\n' "${P4_AVGS[@]}" | stats)"
echo "  all samples  (K/sec): $(printf '%s\n' "${ALL_P4[@]}"  | stats)"
echo "  fio READ  $(printf '%s\n' "${P4_FIO_R[@]}" | stats)"
echo "  fio WRITE $(printf '%s\n' "${P4_FIO_W[@]}" | stats)"

echo 100000000 | sudo tee /proc/sys/dev/raid/speed_limit_max >/dev/null
echo 0         | sudo tee /proc/sys/dev/raid/speed_limit_min >/dev/null

echo ""
echo "============================================"
echo "  done: $LABEL  $(date +%T)"
echo "============================================"
