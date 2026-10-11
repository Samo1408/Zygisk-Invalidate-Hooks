# shellcheck disable=SC2034
SKIPUNZIP=1

DEBUG=@DEBUG@
SONAME=@SONAME@
SUPPORTED_ABIS="@SUPPORTED_ABIS@"

if [ "$BOOTMODE" ] && [ "$KSU" ]; then
  ui_print "- Installing from KernelSU app"
  ui_print "- KernelSU version: $KSU_KERNEL_VER_CODE (kernel) + $KSU_VER_CODE (ksud)"
  if [ "$(which magisk)" ]; then
    ui_print "*********************************************************"
    ui_print "! Multiple root implementation is NOT supported!"
    ui_print "! Please uninstall Magisk before installing Zygisk Next"
    abort    "*********************************************************"
  fi
elif [ "$BOOTMODE" ] && [ "$MAGISK_VER_CODE" ]; then
  ui_print "- Installing from Magisk app"
else
  ui_print "*********************************************************"
  ui_print "! Install from recovery is not supported"
  ui_print "! Please install from KernelSU or Magisk app"
  abort    "*********************************************************"
fi

VERSION=$(grep_prop version "${TMPDIR}/module.prop")
ui_print "- Installing $SONAME $VERSION"

# check architecture
support=false
for abi in $SUPPORTED_ABIS
do
  if [ "$ARCH" == "$abi" ]; then
    support=true
  fi
done
if [ "$support" == "false" ]; then
  abort "! Unsupported platform: $ARCH"
else
  ui_print "- Device platform: $ARCH"
fi

ui_print "- Extracting verify.sh"
unzip -o "$ZIPFILE" 'verify.sh' -d "$TMPDIR" >&2
if [ ! -f "$TMPDIR/verify.sh" ]; then
  ui_print "*********************************************************"
  ui_print "! Unable to extract verify.sh!"
  ui_print "! This zip may be corrupted, please try downloading again"
  abort    "*********************************************************"
fi
. "$TMPDIR/verify.sh"
extract "$ZIPFILE" 'customize.sh'  "$TMPDIR/.vunzip"
extract "$ZIPFILE" 'verify.sh'     "$TMPDIR/.vunzip"
extract "$ZIPFILE" 'sepolicy.rule' "$TMPDIR"

ui_print "- Extracting module files"
# Preserve settings from an already-installed version when updating the module.
OLDMODPATH="/data/adb/modules/$SONAME"
SAVED_DEVICE_PROP="$TMPDIR/device.prop.saved"
SAVED_DEVICE_PROFILE="$TMPDIR/devicepreset-profile.json.saved"
if [ -f "$OLDMODPATH/config/device.prop" ]; then cp -f "$OLDMODPATH/config/device.prop" "$SAVED_DEVICE_PROP"; fi
if [ -f "$OLDMODPATH/config/devicepreset-profile.json" ]; then cp -f "$OLDMODPATH/config/devicepreset-profile.json" "$SAVED_DEVICE_PROFILE"; fi

extract "$ZIPFILE" 'module.prop'     "$MODPATH"
extract "$ZIPFILE" 'post-fs-data.sh' "$MODPATH"
extract "$ZIPFILE" 'service.sh'      "$MODPATH"
mkdir -p "$MODPATH/webroot" "$MODPATH/config" "$MODPATH/bin"
extract "$ZIPFILE" 'webroot/index.html' "$MODPATH/webroot" true
extract "$ZIPFILE" 'config/device.prop' "$MODPATH"
extract "$ZIPFILE" 'bin/resetprop-rs' "$MODPATH"
extract "$ZIPFILE" 'bin/BINARY-SHA256.txt' "$MODPATH"
# Restore saved DevicePreset settings after extracting defaults.
[ ! -f "$SAVED_DEVICE_PROP" ] || cp -f "$SAVED_DEVICE_PROP" "$MODPATH/config/device.prop"
[ ! -f "$SAVED_DEVICE_PROFILE" ] || cp -f "$SAVED_DEVICE_PROFILE" "$MODPATH/config/devicepreset-profile.json"
mv "$TMPDIR/sepolicy.rule" "$MODPATH"

if [ ! -f "$MODPATH/config.txt" ]; then
  ui_print "- Creating default config"
  echo "0:libart.so" > "$MODPATH/config.txt"
fi

HAS32BIT=false && ([ $(getprop ro.product.cpu.abilist32) ] || [ $(getprop ro.system.product.cpu.abilist32) ]) && HAS32BIT=true

mkdir "$MODPATH/zygisk"

if [ "$ARCH" = "x86" ] || [ "$ARCH" = "x64" ]; then
  if [ "$HAS32BIT" = true ]; then
    ui_print "- Extracting x86 libraries"
    extract "$ZIPFILE" "lib/x86/lib$SONAME.so" "$MODPATH/zygisk/" true
    mv "$MODPATH/zygisk/lib$SONAME.so" "$MODPATH/zygisk/x86.so"
  fi

  ui_print "- Extracting x64 libraries"
  extract "$ZIPFILE" "lib/x86_64/lib$SONAME.so" "$MODPATH/zygisk" true
  mv "$MODPATH/zygisk/lib$SONAME.so" "$MODPATH/zygisk/x86_64.so"
else
  if [ "$HAS32BIT" = true ]; then
    extract "$ZIPFILE" "lib/armeabi-v7a/lib$SONAME.so" "$MODPATH/zygisk" true
    mv "$MODPATH/zygisk/lib$SONAME.so" "$MODPATH/zygisk/armeabi-v7a.so"
  fi

  ui_print "- Extracting arm64 libraries"
  extract "$ZIPFILE" "lib/arm64-v8a/lib$SONAME.so" "$MODPATH/zygisk" true
  mv "$MODPATH/zygisk/lib$SONAME.so" "$MODPATH/zygisk/arm64-v8a.so"
fi

ui_print "- Setting permissions"
set_perm_recursive "$MODPATH" 0 0 0755 0644
# Magisk/KernelSU must be able to execute module lifecycle scripts.
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/bin/resetprop-rs" 0 0 0755
[ ! -f "$MODPATH/config/devicepreset-profile.json" ] || set_perm "$MODPATH/config/devicepreset-profile.json" 0 0 0644
