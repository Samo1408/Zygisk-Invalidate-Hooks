# DevicePreset app ↔ Zygisk module bridge

The companion Android app is in `companion-app/`. It is built as a separate Android project so the existing native module build configuration is unchanged.

## Shared data flow
1. DevicePreset imports/edits a profile.
2. Save verifies `/data/adb/modules/inline_hook_spoof/module.prop` exists and the module is not disabled.
3. Save atomically updates `config/device.prop` (runtime property list) and `config/devicepreset-profile.json` (editable source values).
4. On reboot, `post-fs-data.sh` applies the property list via `/system/bin/resetprop-rs --if-diff`.
5. On next app launch, the app restores editable values from the source profile.

The module's original `config.txt` is not modified; it remains dedicated to the original Zygisk package-selection logic. Save is not an immediate property apply operation: reboot is required for the module's intended early-boot application timing.

The property engine is expected at the fixed device path `/system/bin/resetprop-rs`; it is not bundled in the module ZIP. DevicePreset property application remains system-wide and separate from `inline_hook_spoof` webroot app targeting.


## Baseband and SoC property coverage

- `ro.hardware` is intentionally disabled because it can cause lock-screen problems. The boot script skips it even if it remains in an older saved `device.prop`.
- Baseband profile values generate `gsm.version.baseband`, `ro.baseband`, and `ro.boot.baseband`.
- SoC model values generate `ro.soc.model`, `ro.hardware.soc.model`, and `ro.boot.soc.model`.
- SoC manufacturer values generate `ro.soc.manufacturer`, `ro.hardware.soc.manufacturer`, and `ro.boot.soc.manufacturer`.
- `service.sh` reapplies these keys after `sys.boot_completed`, because the radio/framework may publish the baseband after `post-fs-data`.
