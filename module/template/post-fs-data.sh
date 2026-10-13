MODDIR=${0%/*}
CONFIG="$MODDIR/config.txt"
BIN="/system/bin/resetprop-rs"
PROPS="$MODDIR/config/device.prop"
LOG="$MODDIR/last_apply.log"

# Preserve the original module's app-target configuration.
[ -f "$CONFIG" ] && chmod 0644 "$CONFIG"

umask 022
: > "$LOG"
log() { echo "[$(date '+%H:%M:%S' 2>/dev/null)] $*" >> "$LOG"; }

# DevicePreset property application is independent from config.txt / Zygisk targeting.
if [ ! -r "$PROPS" ]; then
    log "INFO: no device property config at $PROPS; skipping property application"
    exit 0
fi
if [ ! -r "$BIN" ]; then
    log "ERROR: resetprop-rs missing or not readable at fixed system path: $BIN"
    exit 0
fi
if [ ! -x /system/bin/linker64 ]; then
    log "ERROR: Android linker is missing or not executable: /system/bin/linker64"
    exit 0
fi
if [ "$(getprop ro.product.cpu.abi 2>/dev/null)" != "arm64-v8a" ] && [ "$(uname -m 2>/dev/null)" != "aarch64" ]; then
    log "ERROR: resetprop-rs at $BIN requires an ARM64 device; skipping property application"
    exit 0
fi

count=0
applied=0
skipped=0
failed=0
mismatch=0
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        ''|\#*) continue ;;
        *=*) ;;
        *) log "SKIP malformed line"; skipped=$((skipped + 1)); continue ;;
    esac
    name=${line%%=*}
    value=${line#*=}
    case "$name" in
        ''|ro.hardware|ro.product.hardware|ro.board.platform|ro.boot.board.platform)
            log "SKIP protected key: $name"
            skipped=$((skipped + 1))
            continue
            ;;
    esac
    count=$((count + 1))
    # Use the user-provided fixed system path; do not search inside the module.
    # Invoke through Android linker explicitly; use --stealth because it is the verified working mode on-device.
    output=$( /system/bin/linker64 "$BIN" --stealth "$name" "$value" 2>&1)
    rc=$?
    if [ "$rc" -eq 0 ]; then
        actual=$(getprop "$name" 2>/dev/null)
        if [ "$actual" = "$value" ]; then
            log "OK/VERIFIED: $name"
            applied=$((applied + 1))
        else
            log "WARN/READBACK_MISMATCH: $name expected=[$value] actual=[$actual]"
            mismatch=$((mismatch + 1))
        fi
    else
        # Keep diagnostics readable if launch errors contain non-text bytes.
        output=$(printf '%s' "$output" | tr '\000-\010\013\014\016-\037' '?' | cut -c1-240)
        log "ERROR ($rc): $name ${output}"
        failed=$((failed + 1))
    fi
done < "$PROPS"
log "SUMMARY total=$count verified=$applied readback_mismatch=$mismatch skipped=$skipped failed=$failed"
exit 0
