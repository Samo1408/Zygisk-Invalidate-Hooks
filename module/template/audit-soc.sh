#!/system/bin/sh
# Read-only diagnostic for SoC properties versus exposed kernel data.
MODDIR=${0%/*}
PROPS="$MODDIR/config/device.prop"
get_expected() { sed -n "s/^$1=//p" "$PROPS" | head -n 1; }
BIN="/system/bin/resetprop-rs"
LINKER="/system/bin/linker64"
show_prop() {
    name="$1"
    actual=$(getprop "$name" 2>/dev/null)
    tool_actual="(unavailable)"
    if [ -r "$BIN" ] && [ -x "$LINKER" ]; then
        tool_actual=$("$LINKER" "$BIN" "$name" 2>/dev/null | tail -n 1)
    fi
    printf '%-34s expected=[%s] getprop=[%s] resetprop_rs=[%s]\n' "$name" "$(get_expected "$name")" "$actual" "$tool_actual"
}
echo '=== Configured vs live SoC properties ==='
for p in ro.soc.model ro.soc.manufacturer ro.hardware.soc.model ro.hardware.soc.manufacturer ro.boot.soc.model ro.boot.soc.manufacturer ro.board.platform ro.hardware ro.boot.hardware; do
    show_prop "$p"
done
echo
echo '=== Live properties containing soc/cpu/hardware (filtered) ==='
getprop 2>/dev/null | grep -iE '\[(ro\.|persist\.)[^]]*(soc|cpu|hardware|baseband)[^]]*\]' || true
echo
echo '=== Kernel SoC files (read-only; may not exist) ==='
for f in /sys/devices/soc0/machine /sys/devices/soc0/soc_id /sys/devices/soc0/family /sys/devices/soc0/revision /sys/devices/system/cpu/cpu0/regs/identification/midr_el1; do
    if [ -r "$f" ]; then printf '%s=' "$f"; cat "$f" 2>/dev/null; fi
done
echo
echo '=== Selected /proc/cpuinfo fields ==='
if [ -r /proc/cpuinfo ]; then
    grep -iE '^(hardware|model name|processor|cpu implementer|cpu part|cpu architecture)[[:space:]]*:' /proc/cpuinfo | head -n 40 || true
fi
echo
echo 'NOTE: This script only reads values; it does not change properties or module targeting.'


PROC_CONFIG="$MODDIR/config/proc-overrides.conf"
echo
echo '=== Saved procfs override configuration (not yet runtime-applied) ==='
if [ -r "$PROC_CONFIG" ]; then
    cat "$PROC_CONFIG"
else
    echo "No procfs override file at $PROC_CONFIG"
fi
echo
echo '=== Current kernel-provided identity lines ==='
grep -E '^(Hardware|Linux version)[[:space:]]*:' /proc/cpuinfo /proc/version 2>/dev/null || {
    grep -E '^(Hardware|Linux version)' /proc/cpuinfo /proc/version 2>/dev/null || true
}
