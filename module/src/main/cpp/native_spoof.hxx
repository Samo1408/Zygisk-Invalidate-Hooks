#ifndef DEVICEPRESET_NATIVE_SPOOF_HXX
#define DEVICEPRESET_NATIVE_SPOOF_HXX

// Read override files while the Zygisk process still has module-file access.
void loadNativeOverrides();
// Install per-process PLT hooks after the selected app has been specialized.
void installNativeHooks();

#endif
