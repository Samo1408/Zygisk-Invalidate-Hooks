# Zygisk Invalidate Hooks

This module restores original library bytes from disk to memory to remove inline hooks inserted by App Developers to block Xposed/LSPosed.

## Why this is needed?
App developers often place early inline hooks in `libart.so` during the `preAppSpecialize` fork stage. When LSPosed which uses the **LSPlant** engine attempts to hook the same methods it conflicts with the developer's hooks

This module invalidates wipes out those early hooks by restoring clean bytes from the disk before LSPosed starts its work

## Process Flowchart

```mermaid
graph TD
    Start([App Launch: Zygote Fork]) --> preAppSpecialize[preAppSpecialize stage]
    
    subgraph App_Side_Anti_Hook [App Developer Action]
        preAppSpecialize --> AppHook[App Developer places Early Inline Hooks in libart.so]
        AppHook --> AntiLSPosed{Anti-Hook Active?}
        AntiLSPosed -- Yes --> Block[Detects/Crashes LSPosed/LSPlant]
    end

    subgraph Module_Intervention [Your Module: Restoration]
        preAppSpecialize --> ReadConfig[Read config.txt]
        ReadConfig --> IsPackage{Target Package?}
        IsPackage -- Yes --> Invalidate[Call mainCore / capcap]
        
        Invalidate --> FindLib[Locate libart.so in Memory]
        FindLib --> DiskOriginal[Open libart.so from Disk]
        
        DiskOriginal --> Restore[Overwrite Hooked RAM with Clean Disk Bytes]
        Restore --> CleanMemory[libart.so is now CLEAN/Original]
    end

    subgraph Success_State [LSPosed Logic]
        CleanMemory --> LSPlant[LSPosed/LSPlant tries to hook libart.so]
        LSPlant --> Success([LSPlant Hooks successfully without Crash])
    end

    Block -.-> Conflict((CONFLICT/CRASH))
    IsPackage -- No --> End([Normal App Flow])
```

## Config Path
`/data/adb/modules/inline_hook_spoof/config.txt`

### Config Format:
`1:libart.so` (1 for enabled)  
`com.example.app` (package names line by line)


## DevicePreset settings integration

This source tree also carries DevicePreset's property-profile application mechanism. The included `template/config/device.prop` is generated from the bundled **Google Pixel 10 Pro** profile and is applied during `post-fs-data` using `/system/bin/resetprop-rs --if-diff`. Edit `config/device.prop` in the installed module to change values; `config.txt` remains the separate original Zygisk app-target list and is not overwritten.

The property applicator skips `ro.product.hardware`, `ro.hardware`, `ro.board.platform`, and `ro.boot.board.platform` to preserve the previously established boot-safety exclusions. The resetprop-rs executable expected at `/system/bin/resetprop-rs` is ARM64-only; on other architectures the property step logs and skips without stopping module initialization. Application results are recorded in `/data/adb/modules/inline_hook_spoof/last_apply.log`. The original native Zygisk source and its configuration format are otherwise unchanged. The resetprop-rs executable is provided at the fixed system path and is not bundled or searched for inside the module. DevicePreset property application remains system-wide and independent from the webroot app-target list.
