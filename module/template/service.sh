DEBUG=@DEBUG@

MODDIR=${0%/*}

# DevicePreset late pass: radio/framework may republish baseband after post-fs-data.
# This is deliberately independent of config.txt and app-target selection.
BIN="/system/bin/resetprop-rs"
LINKER="/system/bin/linker64"
PROPS="$MODDIR/config/device.prop"
LOG="$MODDIR/last_apply.log"

if [ -r "$PROPS" ] && [ -r "$BIN" ] && [ -x "$LINKER" ]; then
    # Wait for Android boot completion so dynamic radio properties are already published.
    tries=0
    while [ "$(getprop sys.boot_completed 2>/dev/null)" != "1" ] && [ "$tries" -lt 90 ]; do
        sleep 2
        tries=$((tries + 1))
    done
    # Allow telephony to publish its final baseband value before overriding it.
    sleep 8
    count=0
    ok=0
    failed=0
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|\#*|*=*) ;; *) continue ;; esac
        case "$line" in *=*) ;; *) continue ;; esac
        name=${line%%=*}
        value=${line#*=}
        case "$name" in
            gsm.version.baseband|ro.baseband|ro.boot.baseband|ro.soc.model|ro.hardware.soc.model|ro.boot.soc.model|ro.soc.manufacturer|ro.hardware.soc.manufacturer|ro.boot.soc.manufacturer) ;;
            *) continue ;;
        esac
        [ -n "$value" ] || continue
        count=$((count + 1))
        output=$("$LINKER" "$BIN" --stealth "$name" "$value" 2>&1)
        rc=$?
        if [ "$rc" -eq 0 ]; then
            ok=$((ok + 1))
        else
            output=$(printf '%s' "$output" | tr '\000-\010\013\014\016-\037' '?' | cut -c1-180)
            echo "[$(date '+%H:%M:%S' 2>/dev/null)] LATE/ERROR ($rc): $name $output" >> "$LOG"
            failed=$((failed + 1))
        fi
    done < "$PROPS"
    echo "[$(date '+%H:%M:%S' 2>/dev/null)] LATE SUMMARY total=$count ok=$ok failed=$failed (baseband/SoC reapply)" >> "$LOG"
fi

