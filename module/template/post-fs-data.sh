MODDIR=${0%/*}
CONFIG="$MODDIR/config.txt"
BIN="$MODDIR/bin/resetprop-rs"
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
if [ ! -x "$BIN" ]; then
    log "ERROR: resetprop-rs missing or not executable: $BIN"
    exit 0
fi
if [ "$(getprop ro.product.cpu.abi 2>/dev/null)" != "arm64-v8a" ] && [ "$(uname -m 2>/dev/null)" != "aarch64" ]; then
    log "ERROR: bundled resetprop-rs supports arm64-v8a only; skipping property application"
    exit 0
fi

count=0
applied=0
skipped=0
failed=0
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        ''|\#*) continue ;;
        *=*) ;;
        *) log "SKIP malformed line"; skipped=$((skipped + 1)); continue ;;
    esac
    name=${line%%=*}
    value=${line#*=}
    case "$name" in
        ''|ro.product.hardware|ro.hardware|ro.board.platform|ro.boot.board.platform)
            log "SKIP protected key: $name"
            skipped=$((skipped + 1))
            continue
            ;;
    esac
    count=$((count + 1))
    # Invoke Android PIE through its dynamic linker in early-boot environments.
    if [ -x /system/bin/linker64 ]; then
        output=$(/system/bin/linker64 "$BIN" --if-diff "$name" "$value" 2>&1)
        rc=$?
    else
        output=$("$BIN" --if-diff "$name" "$value" 2>&1)
        rc=$?
    fi
    if [ "$rc" -eq 0 ]; then
        log "OK/UNCHANGED: $name"
        applied=$((applied + 1))
    else
        # Keep diagnostics readable if launch errors contain binary bytes.
        output=$(printf '%s' "$output" | tr '\000-\010\013\014\016-\037' '?' | cut -c1-240)
        log "SKIP/ERROR ($rc): $name ${output}"
        failed=$((failed + 1))
    fi
done < "$PROPS"
log "SUMMARY total=$count ok_or_unchanged=$applied skipped=$skipped failed=$failed"
exit 0
