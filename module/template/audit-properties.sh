#!/system/bin/sh
# Read-only audit of configured Android system properties.
# This does not inspect TelephonyManager, sysfs, device tree, or native hardware APIs.
MODDIR=${0%/*}
PROPS="$MODDIR/config/device.prop"

if [ ! -r "$PROPS" ]; then
    echo "ERROR: property configuration is not readable: $PROPS"
    exit 1
fi

checked=0
matched=0
mismatched=0
skipped=0
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in ''|\#*) continue ;; *=*) ;; *) skipped=$((skipped + 1)); continue ;; esac
    name=${line%%=*}
    expected=${line#*=}
    case "$name" in
        ''|ro.hardware|ro.product.hardware|ro.board.platform|ro.boot.board.platform)
            skipped=$((skipped + 1))
            continue
            ;;
    esac
    actual=$(getprop "$name" 2>/dev/null)
    checked=$((checked + 1))
    if [ "$actual" = "$expected" ]; then
        echo "MATCH $name"
        matched=$((matched + 1))
    else
        echo "MISMATCH $name expected=[$expected] actual=[$actual]"
        mismatched=$((mismatched + 1))
    fi
done < "$PROPS"

echo "SUMMARY checked=$checked matched=$matched mismatched=$mismatched skipped=$skipped"
echo "NOTE: This audits getprop values only; it cannot validate independent framework or hardware APIs."
[ "$mismatched" -eq 0 ]
