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
    mismatch=0
    failed=0
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|\#*) continue ;; *=*) ;; *) continue ;; esac
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
            actual=$(getprop "$name" 2>/dev/null)
            if [ "$actual" = "$value" ]; then
                ok=$((ok + 1))
            else
                echo "[$(date '+%H:%M:%S' 2>/dev/null)] LATE/READBACK_MISMATCH: $name expected=[$value] actual=[$actual]" >> "$LOG"
                mismatch=$((mismatch + 1))
            fi
        else
            output=$(printf '%s' "$output" | tr -cd '[:print:]\n' | cut -c1-180)
            echo "[$(date '+%H:%M:%S' 2>/dev/null)] LATE/ERROR ($rc): $name $output" >> "$LOG"
            failed=$((failed + 1))
        fi
    done < "$PROPS"
    echo "[$(date '+%H:%M:%S' 2>/dev/null)] LATE SUMMARY total=$count verified=$ok readback_mismatch=$mismatch failed=$failed (baseband/SoC reapply)" >> "$LOG"
fi

# Diagnostic only: sample SoC values after the late apply to identify whether
# Android/vendor services republish the hardware values later in boot.
# This section does not write properties and does not touch Zygisk app targeting.
if [ -r "$PROPS" ]; then
    soc_model_expected=$(sed -n 's/^ro.soc.model=//p' "$PROPS" | head -n 1)
    soc_mfr_expected=$(sed -n 's/^ro.soc.manufacturer=//p' "$PROPS" | head -n 1)
    for delay in 0 2 5 10 20 30 60; do
        [ "$delay" -eq 0 ] || sleep "$((delay - prev_delay))"
        prev_delay=$delay
        actual_model=$(getprop ro.soc.model 2>/dev/null)
        actual_mfr=$(getprop ro.soc.manufacturer 2>/dev/null)
        actual_hw_model=$(getprop ro.hardware.soc.model 2>/dev/null)
        actual_boot_model=$(getprop ro.boot.soc.model 2>/dev/null)
        echo "[$(date '+%H:%M:%S' 2>/dev/null)] SOC-SAMPLE t=${delay}s model_expected=[$soc_model_expected] model=[$actual_model] manufacturer_expected=[$soc_mfr_expected] manufacturer=[$actual_mfr] hw_model=[$actual_hw_model] boot_model=[$actual_boot_model]" >> "$LOG"
    done
fi
