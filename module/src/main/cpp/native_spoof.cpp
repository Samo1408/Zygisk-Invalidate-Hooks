#include "native_spoof.hxx"
#include "log.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <mutex>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace {
using namespace std;

static const char *kDeviceProps = "/data/adb/modules/inline_hook_spoof/config/device.prop";
static const char *kProcOverrides = "/data/adb/modules/inline_hook_spoof/config/proc-overrides.conf";
static unordered_map<string, string> gProps;
static string gCpuHardware;
static string gKernelVersion;
static bool gLoaded = false;
static bool gInstalled = false;

using prop_get_fn = int (*)(const char *, char *);
using prop_find_fn = const prop_info *(*)(const char *);
using prop_read_fn = int (*)(const prop_info *, char *, char *);
using prop_callback_fn = void (*)(void *, const char *, const char *, uint32_t);
using prop_read_callback_fn = void (*)(const prop_info *, prop_callback_fn, void *);
using prop_serial_fn = uint32_t (*)(const prop_info *);
using open_fn = int (*)(const char *, int, ...);
using openat_fn = int (*)(int, const char *, int, ...);
using open2_fn = int (*)(const char *, int);
using openat2_fn = int (*)(int, const char *, int);
using fopen_fn = FILE *(*)(const char *, const char *);
using fdopen_fn = FILE *(*)(int, const char *);
using dlopen_fn = void *(*)(const char *, int);
using android_dlopen_ext_fn = void *(*)(const char *, int, const void *);

static prop_get_fn real_prop_get = nullptr;
static prop_find_fn real_prop_find = nullptr;
static prop_read_fn real_prop_read = nullptr;
static prop_read_callback_fn real_prop_read_callback = nullptr;
static prop_serial_fn real_prop_serial = nullptr;
static open_fn real_open = nullptr;
static open_fn real_open64 = nullptr;
static openat_fn real_openat = nullptr;
static openat_fn real_openat64 = nullptr;
static open2_fn real_open_2 = nullptr;
static open2_fn real_open64_2 = nullptr;
static openat2_fn real_openat_2 = nullptr;
static openat2_fn real_openat64_2 = nullptr;
static fopen_fn real_fopen = nullptr;
static fopen_fn real_fopen64 = nullptr;
static fdopen_fn real_fdopen = nullptr;
static dlopen_fn real_dlopen = nullptr;
static android_dlopen_ext_fn real_android_dlopen_ext = nullptr;

struct FakeProp { alignas(16) unsigned char opaque[64]{}; string name; };
static map<string, unique_ptr<FakeProp>> gFakeProps;
static unordered_map<const prop_info *, string> gFakeNames;
static mutex gFakeMutex;

static string trimLine(string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

static void loadKeyValueFile(const char *path, unordered_map<string, string> &out) {
    ifstream in(path);
    string line;
    while (getline(in, line)) {
        line = trimLine(line);
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos == string::npos || pos == 0) continue;
        string key = line.substr(0, pos);
        string value = line.substr(pos + 1);
        out[key] = value;
    }
}

static const string *overrideFor(const char *name) {
    if (!name) return nullptr;
    auto it = gProps.find(name);
    return it == gProps.end() ? nullptr : &it->second;
}

static string fakeNameCopy(const prop_info *pi) {
    lock_guard<mutex> lock(gFakeMutex);
    auto it = gFakeNames.find(pi);
    return it == gFakeNames.end() ? string() : it->second;
}

static const prop_info *fakePropFor(const string &name) {
    lock_guard<mutex> lock(gFakeMutex);
    auto it = gFakeProps.find(name);
    if (it == gFakeProps.end()) {
        unique_ptr<FakeProp> p(new FakeProp());
        p->name = name;
        const prop_info *key = reinterpret_cast<const prop_info *>(p->opaque);
        gFakeNames[key] = name;
        it = gFakeProps.emplace(name, std::move(p)).first;
    }
    return reinterpret_cast<const prop_info *>(it->second->opaque);
}

static uint32_t hook_prop_serial(const prop_info *pi) {
    string fakeName = fakeNameCopy(pi);
    if (!fakeName.empty()) return 0;
    return real_prop_serial ? real_prop_serial(pi) : 0;
}

static int hook_prop_get(const char *name, char *value) {
    if (const string *v = overrideFor(name)) {
        if (!value) return static_cast<int>(v->size());
        size_t n = min(v->size(), static_cast<size_t>(PROP_VALUE_MAX - 1));
        memcpy(value, v->data(), n);
        value[n] = '\0';
        return static_cast<int>(n);
    }
    return real_prop_get ? real_prop_get(name, value) : 0;
}

static const prop_info *hook_prop_find(const char *name) {
    if (const string *v = overrideFor(name)) {
        if (real_prop_find) {
            const prop_info *actual = real_prop_find(name);
            if (actual) return actual;
        }
        return fakePropFor(name);
    }
    return real_prop_find ? real_prop_find(name) : nullptr;
}

static int hook_prop_read(const prop_info *pi, char *name, char *value) {
    string fakeName = fakeNameCopy(pi);
    if (!fakeName.empty()) {
        const string *v = overrideFor(fakeName.c_str());
        if (!v) return 0;
        if (name) { strncpy(name, fakeName.c_str(), PROP_NAME_MAX - 1); name[PROP_NAME_MAX - 1] = '\0'; }
        if (value) { size_t n = min(v->size(), static_cast<size_t>(PROP_VALUE_MAX - 1)); memcpy(value, v->data(), n); value[n] = '\0'; }
        return static_cast<int>(v->size());
    }
    int result = real_prop_read ? real_prop_read(pi, name, value) : 0;
    if (result > 0 && name && value) {
        if (const string *v = overrideFor(name)) {
            size_t n = min(v->size(), static_cast<size_t>(PROP_VALUE_MAX - 1));
            memcpy(value, v->data(), n); value[n] = '\0';
            result = static_cast<int>(n);
        }
    }
    return result;
}

struct CallbackContext { prop_callback_fn callback; void *cookie; };
static void propCallbackBridge(void *opaque, const char *name, const char *value, uint32_t serial) {
    CallbackContext *ctx = static_cast<CallbackContext *>(opaque);
    if (!ctx || !ctx->callback) return;
    if (const string *v = overrideFor(name)) ctx->callback(ctx->cookie, name, v->c_str(), serial);
    else ctx->callback(ctx->cookie, name, value, serial);
}

static void hook_prop_read_callback(const prop_info *pi, prop_callback_fn callback, void *cookie) {
    if (!callback) return;
    string fakeName = fakeNameCopy(pi);
    if (!fakeName.empty()) {
        if (const string *v = overrideFor(fakeName.c_str())) callback(cookie, fakeName.c_str(), v->c_str(), 0);
        return;
    }
    if (!real_prop_read_callback) return;
    CallbackContext ctx{callback, cookie};
    real_prop_read_callback(pi, propCallbackBridge, &ctx);
}

static bool endsWith(const string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static string resolveAtPath(int dirfd, const char *path) {
    if (!path) return string();
    if (path[0] == '/') return string(path);
    if (dirfd == AT_FDCWD) return string();
    char linkPath[64], target[PATH_MAX + 1];
    snprintf(linkPath, sizeof(linkPath), "/proc/self/fd/%d", dirfd);
    long n = syscall(SYS_readlinkat, AT_FDCWD, linkPath, target, PATH_MAX);
    if (n <= 0) return string();
    target[n] = '\0';
    string out(target);
    if (!out.empty() && out.back() != '/') out.push_back('/');
    out += path;
    return out;
}

static bool isCpuInfoPath(const string &p) { return p == "/proc/cpuinfo" || endsWith(p, "/proc/cpuinfo"); }
static bool isProcVersionPath(const string &p) { return p == "/proc/version" || endsWith(p, "/proc/version"); }

static string readOriginalProc(const char *path) {
    string out;
    long fd = syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return out;
    char buf[4096];
    for (;;) {
        long n = syscall(SYS_read, static_cast<int>(fd), buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 1024 * 1024) break;
    }
    syscall(SYS_close, static_cast<int>(fd));
    return out;
}

static string replaceCpuHardware(const string &src) {
    if (gCpuHardware.empty()) return src;
    istringstream in(src);
    string line, out;
    bool found = false;
    while (getline(in, line)) {
        string trimmed = line;
        size_t first = trimmed.find_first_not_of(" \t");
        if (first != string::npos) trimmed.erase(0, first);
        if (trimmed.compare(0, 8, "Hardware") == 0) {
            size_t colon = line.find(':');
            if (colon != string::npos) {
                out += line.substr(0, colon + 1) + " " + gCpuHardware + "\n";
                found = true;
                continue;
            }
        }
        out += line;
        out += '\n';
    }
    if (!found) out += "Hardware : " + gCpuHardware + "\n";
    return out;
}

static string replaceKernelVersion(const string &src) {
    if (gKernelVersion.empty()) return src;
    string version = gKernelVersion;
    const string prefix = "Linux version ";
    if (version.compare(0, prefix.size(), prefix) == 0) version.erase(0, prefix.size());
    size_t endVersion = version.find_first_of(" \t\r\n");
    if (endVersion != string::npos) version.erase(endVersion);
    if (version.empty()) return src;
    string out = src;
    size_t start = out.find(prefix);
    if (start == string::npos) return prefix + version + "\n";
    start += prefix.size();
    size_t end = out.find_first_of(" \t\r\n", start);
    if (end == string::npos) end = out.find('\n', start);
    if (end == string::npos) end = out.size();
    out.replace(start, end - start, version);
    return out;
}

static int createMemfd(const string &contents) {
#ifdef SYS_memfd_create
    int fd = static_cast<int>(syscall(SYS_memfd_create, "devicepreset-proc", MFD_CLOEXEC));
    if (fd < 0) return -1;
    size_t offset = 0;
    while (offset < contents.size()) {
        long n = syscall(SYS_write, fd, contents.data() + offset, contents.size() - offset);
        if (n <= 0) { syscall(SYS_close, fd); return -1; }
        offset += static_cast<size_t>(n);
    }
    if (syscall(SYS_lseek, fd, 0, SEEK_SET) < 0) { syscall(SYS_close, fd); return -1; }
    return fd;
#else
    (void)contents;
    return -1;
#endif
}

static int spoofProcFd(const string &resolvedPath) {
    const char *sourcePath = isCpuInfoPath(resolvedPath) ? "/proc/cpuinfo" : "/proc/version";
    string contents = readOriginalProc(sourcePath);
    if (contents.empty()) return -1;
    if (isCpuInfoPath(resolvedPath)) contents = replaceCpuHardware(contents);
    else contents = replaceKernelVersion(contents);
    return createMemfd(contents);
}

static int callRealOpen(const char *path, int flags, mode_t mode, bool hasMode, bool use64) {
    open_fn fn = use64 && real_open64 ? real_open64 : real_open;
    if (fn) return hasMode ? fn(path, flags, mode) : fn(path, flags);
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, hasMode ? mode : 0));
}

static int callRealOpenAt(int dirfd, const char *path, int flags, mode_t mode, bool hasMode, bool use64) {
    openat_fn fn = use64 && real_openat64 ? real_openat64 : real_openat;
    if (fn) return hasMode ? fn(dirfd, path, flags, mode) : fn(dirfd, path, flags);
    return static_cast<int>(syscall(SYS_openat, dirfd, path, flags, hasMode ? mode : 0));
}

static int hook_open_common(const char *path, int flags, mode_t mode, bool hasMode, bool use64) {
    string resolved = resolveAtPath(AT_FDCWD, path);
    if (!resolved.empty() && ((!gCpuHardware.empty() && isCpuInfoPath(resolved)) || (!gKernelVersion.empty() && isProcVersionPath(resolved)))) {
        int fd = spoofProcFd(resolved);
        if (fd >= 0) return fd;
    }
    return callRealOpen(path, flags, mode, hasMode, use64);
}
static int hook_open(const char *path, int flags, ...) {
    mode_t mode = 0; bool hasMode = (flags & O_CREAT) != 0;
#ifdef O_TMPFILE
    hasMode = hasMode || ((flags & O_TMPFILE) == O_TMPFILE);
#endif
    if (hasMode) { va_list ap; va_start(ap, flags); mode = static_cast<mode_t>(va_arg(ap, int)); va_end(ap); }
    return hook_open_common(path, flags, mode, hasMode, false);
}
static int hook_open64(const char *path, int flags, ...) {
    mode_t mode = 0; bool hasMode = (flags & O_CREAT) != 0;
#ifdef O_TMPFILE
    hasMode = hasMode || ((flags & O_TMPFILE) == O_TMPFILE);
#endif
    if (hasMode) { va_list ap; va_start(ap, flags); mode = static_cast<mode_t>(va_arg(ap, int)); va_end(ap); }
    return hook_open_common(path, flags, mode, hasMode, true);
}
static int hook_openat_common(int dirfd, const char *path, int flags, mode_t mode, bool hasMode, bool use64) {
    string resolved = resolveAtPath(dirfd, path);
    if (!resolved.empty() && ((!gCpuHardware.empty() && isCpuInfoPath(resolved)) || (!gKernelVersion.empty() && isProcVersionPath(resolved)))) {
        int fd = spoofProcFd(resolved);
        if (fd >= 0) return fd;
    }
    return callRealOpenAt(dirfd, path, flags, mode, hasMode, use64);
}
static int hook_openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0; bool hasMode = (flags & O_CREAT) != 0;
#ifdef O_TMPFILE
    hasMode = hasMode || ((flags & O_TMPFILE) == O_TMPFILE);
#endif
    if (hasMode) { va_list ap; va_start(ap, flags); mode = static_cast<mode_t>(va_arg(ap, int)); va_end(ap); }
    return hook_openat_common(dirfd, path, flags, mode, hasMode, false);
}
static int hook_open_2(const char *path, int flags) { return hook_open_common(path, flags, 0, false, false); }
static int hook_open64_2(const char *path, int flags) { return hook_open_common(path, flags, 0, false, true); }
static int hook_openat_2(int dirfd, const char *path, int flags) { return hook_openat_common(dirfd, path, flags, 0, false, false); }
static int hook_openat64_2(int dirfd, const char *path, int flags) { return hook_openat_common(dirfd, path, flags, 0, false, true); }
static int hook_openat64(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0; bool hasMode = (flags & O_CREAT) != 0;
#ifdef O_TMPFILE
    hasMode = hasMode || ((flags & O_TMPFILE) == O_TMPFILE);
#endif
    if (hasMode) { va_list ap; va_start(ap, flags); mode = static_cast<mode_t>(va_arg(ap, int)); va_end(ap); }
    return hook_openat_common(dirfd, path, flags, mode, hasMode, true);
}

static FILE *hook_fopen_common(const char *path, const char *mode, bool use64) {
    string resolved = resolveAtPath(AT_FDCWD, path);
    if (!resolved.empty() && ((!gCpuHardware.empty() && isCpuInfoPath(resolved)) || (!gKernelVersion.empty() && isProcVersionPath(resolved)))) {
        int fd = spoofProcFd(resolved);
        if (fd >= 0) {
            if (!real_fdopen) real_fdopen = reinterpret_cast<fdopen_fn>(dlsym(RTLD_DEFAULT, "fdopen"));
            if (real_fdopen) return real_fdopen(fd, mode ? mode : "r");
            syscall(SYS_close, fd);
            return nullptr;
        }
    }
    fopen_fn fn = use64 && real_fopen64 ? real_fopen64 : real_fopen;
    if (fn) return fn(path, mode);
    int fd = static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0));
    if (fd < 0) return nullptr;
    if (!real_fdopen) real_fdopen = reinterpret_cast<fdopen_fn>(dlsym(RTLD_DEFAULT, "fdopen"));
    if (!real_fdopen) { syscall(SYS_close, fd); return nullptr; }
    return real_fdopen(fd, mode ? mode : "r");
}
static FILE *hook_fopen(const char *path, const char *mode) { return hook_fopen_common(path, mode, false); }
static FILE *hook_fopen64(const char *path, const char *mode) { return hook_fopen_common(path, mode, true); }

// Declarations are needed by the relocation table before the implementations below.
static void *hook_dlopen(const char *filename, int flags);
static void *hook_android_dlopen_ext(const char *filename, int flags, const void *extinfo);

struct HookSpec { const char *name; void *replacement; void **original; };
static HookSpec gHooks[] = {
    {"__system_property_get", reinterpret_cast<void *>(hook_prop_get), reinterpret_cast<void **>(&real_prop_get)},
    {"__system_property_find", reinterpret_cast<void *>(hook_prop_find), reinterpret_cast<void **>(&real_prop_find)},
    {"__system_property_read", reinterpret_cast<void *>(hook_prop_read), reinterpret_cast<void **>(&real_prop_read)},
    {"__system_property_read_callback", reinterpret_cast<void *>(hook_prop_read_callback), reinterpret_cast<void **>(&real_prop_read_callback)},
    {"__system_property_serial", reinterpret_cast<void *>(hook_prop_serial), reinterpret_cast<void **>(&real_prop_serial)},
    {"open", reinterpret_cast<void *>(hook_open), reinterpret_cast<void **>(&real_open)},
    {"open64", reinterpret_cast<void *>(hook_open64), reinterpret_cast<void **>(&real_open64)},
    {"openat", reinterpret_cast<void *>(hook_openat), reinterpret_cast<void **>(&real_openat)},
    {"openat64", reinterpret_cast<void *>(hook_openat64), reinterpret_cast<void **>(&real_openat64)},
    {"__open_2", reinterpret_cast<void *>(hook_open_2), reinterpret_cast<void **>(&real_open_2)},
    {"__open64_2", reinterpret_cast<void *>(hook_open64_2), reinterpret_cast<void **>(&real_open64_2)},
    {"__openat_2", reinterpret_cast<void *>(hook_openat_2), reinterpret_cast<void **>(&real_openat_2)},
    {"__openat64_2", reinterpret_cast<void *>(hook_openat64_2), reinterpret_cast<void **>(&real_openat64_2)},
    {"fopen", reinterpret_cast<void *>(hook_fopen), reinterpret_cast<void **>(&real_fopen)},
    {"fopen64", reinterpret_cast<void *>(hook_fopen64), reinterpret_cast<void **>(&real_fopen64)},
    {"dlopen", reinterpret_cast<void *>(hook_dlopen), reinterpret_cast<void **>(&real_dlopen)},
    {"android_dlopen_ext", reinterpret_cast<void *>(hook_android_dlopen_ext), reinterpret_cast<void **>(&real_android_dlopen_ext)},
};

static bool isRelroAddress(const dl_phdr_info *info, uintptr_t address) {
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &p = info->dlpi_phdr[i];
        if (p.p_type != PT_GNU_RELRO) continue;
        uintptr_t start = static_cast<uintptr_t>(info->dlpi_addr) + p.p_vaddr;
        uintptr_t end = start + p.p_memsz;
        if (address >= start && address < end) return true;
    }
    return false;
}

static int protForAddress(const dl_phdr_info *info, uintptr_t address) {
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD) continue;
        uintptr_t start = static_cast<uintptr_t>(info->dlpi_addr) + p.p_vaddr;
        uintptr_t end = start + p.p_memsz;
        if (address >= start && address < end) {
            int prot = 0;
            if (p.p_flags & PF_R) prot |= PROT_READ;
            if (p.p_flags & PF_W) prot |= PROT_WRITE;
            if (p.p_flags & PF_X) prot |= PROT_EXEC;
            if (isRelroAddress(info, address)) prot &= ~PROT_WRITE;
            return prot;
        }
    }
    return PROT_READ;
}

static bool patchSlot(const dl_phdr_info *info, uintptr_t slotAddress, HookSpec &hook) {
    void **slot = reinterpret_cast<void **>(slotAddress);
    if (!slot || !*slot || *slot == hook.replacement) return false;
    if (!*hook.original) *hook.original = *slot;
    if (!*hook.original || *hook.original == hook.replacement) return false;
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return false;
    uintptr_t page = slotAddress & ~(static_cast<uintptr_t>(pageSize) - 1U);
    int originalProt = protForAddress(info, slotAddress);
    if (mprotect(reinterpret_cast<void *>(page), static_cast<size_t>(pageSize), originalProt | PROT_WRITE) != 0) return false;
    *slot = hook.replacement;
    __builtin___clear_cache(reinterpret_cast<char *>(slot), reinterpret_cast<char *>(slot + 1));
    mprotect(reinterpret_cast<void *>(page), static_cast<size_t>(pageSize), originalProt);
    return true;
}

static bool acceptedRelocType(ElfW(Xword) type) {
#if defined(__aarch64__)
    return type == R_AARCH64_JUMP_SLOT || type == R_AARCH64_GLOB_DAT;
#elif defined(__arm__)
    return type == R_ARM_JUMP_SLOT || type == R_ARM_GLOB_DAT;
#elif defined(__x86_64__)
    return type == R_X86_64_JUMP_SLOT || type == R_X86_64_GLOB_DAT;
#elif defined(__i386__)
    return type == R_386_JMP_SLOT || type == R_386_GLOB_DAT;
#else
    (void)type; return false;
#endif
}

static size_t symbolIndex(ElfW(Xword) info) {
#if defined(__LP64__)
    return static_cast<size_t>(ELF64_R_SYM(info));
#else
    return static_cast<size_t>(ELF32_R_SYM(info));
#endif
}
static ElfW(Xword) relocType(ElfW(Xword) info) {
#if defined(__LP64__)
    return static_cast<ElfW(Xword)>(ELF64_R_TYPE(info));
#else
    return static_cast<ElfW(Xword)>(ELF32_R_TYPE(info));
#endif
}

struct PatchContext { size_t slotsPatched = 0; };
static int patchLoadedObject(dl_phdr_info *info, size_t, void *opaque) {
    PatchContext *ctx = static_cast<PatchContext *>(opaque);
    if (!info || !info->dlpi_phdr) return 0;
    const ElfW(Dyn) *dyn = nullptr;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &p = info->dlpi_phdr[i];
        if (p.p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const ElfW(Dyn) *>(static_cast<uintptr_t>(info->dlpi_addr) + p.p_vaddr);
            break;
        }
    }
    if (!dyn) return 0;
    const char *strtab = nullptr;
    const ElfW(Sym) *symtab = nullptr;
    const ElfW(Rela) *rela = nullptr;
    size_t relaSize = 0;
    const ElfW(Rel) *rel = nullptr;
    size_t relSize = 0;
    uintptr_t jmprelAddr = 0;
    size_t jmprelSize = 0;
    ElfW(Sword) pltRelType = DT_REL;
    for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; ++d) {
        uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
        switch (d->d_tag) {
            case DT_STRTAB: strtab = reinterpret_cast<const char *>(base + d->d_un.d_ptr); break;
            case DT_SYMTAB: symtab = reinterpret_cast<const ElfW(Sym) *>(base + d->d_un.d_ptr); break;
            case DT_JMPREL: jmprelAddr = base + d->d_un.d_ptr; break;
            case DT_PLTRELSZ: jmprelSize = static_cast<size_t>(d->d_un.d_val); break;
            case DT_PLTREL: pltRelType = static_cast<ElfW(Sword)>(d->d_un.d_val); break;
            case DT_RELA: rela = reinterpret_cast<const ElfW(Rela) *>(base + d->d_un.d_ptr); break;
            case DT_RELASZ: relaSize = static_cast<size_t>(d->d_un.d_val); break;
            case DT_REL: rel = reinterpret_cast<const ElfW(Rel) *>(base + d->d_un.d_ptr); break;
            case DT_RELSZ: relSize = static_cast<size_t>(d->d_un.d_val); break;
            default: break;
        }
    }
    if (!strtab || !symtab) return 0;

    auto tryPatch = [&](uintptr_t offset, ElfW(Xword) rInfo) {
        if (!acceptedRelocType(relocType(rInfo))) return;
        const ElfW(Sym) &sym = symtab[symbolIndex(rInfo)];
        const char *name = strtab + sym.st_name;
        if (!name || !*name) return;
        for (HookSpec &h : gHooks) {
            if (strcmp(name, h.name) != 0) continue;
            uintptr_t slot = static_cast<uintptr_t>(info->dlpi_addr) + offset;
            if (patchSlot(info, slot, h)) ++ctx->slotsPatched;
            break;
        }
    };

    if (rela && relaSize) {
        size_t count = relaSize / sizeof(ElfW(Rela));
        for (size_t i = 0; i < count; ++i) tryPatch(rela[i].r_offset, rela[i].r_info);
    }
    if (rel && relSize) {
        size_t count = relSize / sizeof(ElfW(Rel));
        for (size_t i = 0; i < count; ++i) tryPatch(rel[i].r_offset, rel[i].r_info);
    }
    if (jmprelAddr && jmprelSize) {
        if (pltRelType == DT_RELA) {
            const ElfW(Rela) *plt = reinterpret_cast<const ElfW(Rela) *>(jmprelAddr);
            size_t count = jmprelSize / sizeof(ElfW(Rela));
            for (size_t i = 0; i < count; ++i) tryPatch(plt[i].r_offset, plt[i].r_info);
        } else {
            const ElfW(Rel) *plt = reinterpret_cast<const ElfW(Rel) *>(jmprelAddr);
            size_t count = jmprelSize / sizeof(ElfW(Rel));
            for (size_t i = 0; i < count; ++i) tryPatch(plt[i].r_offset, plt[i].r_info);
        }
    }
    return 0;
}

static void repatchAfterLibraryLoad() {
    PatchContext ctx;
    dl_iterate_phdr(patchLoadedObject, &ctx);
    if (ctx.slotsPatched != 0) {
        LOGI("DevicePreset native hooks extended to newly loaded libraries: patched_slots=%zu", ctx.slotsPatched);
    }
}

static void *hook_dlopen(const char *filename, int flags) {
    void *handle = real_dlopen ? real_dlopen(filename, flags) : nullptr;
    if (handle && gInstalled) repatchAfterLibraryLoad();
    return handle;
}

static void *hook_android_dlopen_ext(const char *filename, int flags, const void *extinfo) {
    void *handle = real_android_dlopen_ext
        ? real_android_dlopen_ext(filename, flags, extinfo)
        : (real_dlopen ? real_dlopen(filename, flags) : nullptr);
    if (handle && gInstalled) repatchAfterLibraryLoad();
    return handle;
}

static void resolveOriginals() {
    for (HookSpec &h : gHooks) {
        if (*h.original) continue;
        void *p = dlsym(RTLD_DEFAULT, h.name);
        if (p && p != h.replacement) *h.original = p;
    }
    real_fdopen = reinterpret_cast<fdopen_fn>(dlsym(RTLD_DEFAULT, "fdopen"));
    real_dlopen = reinterpret_cast<dlopen_fn>(dlsym(RTLD_DEFAULT, "dlopen"));
    real_android_dlopen_ext = reinterpret_cast<android_dlopen_ext_fn>(dlsym(RTLD_DEFAULT, "android_dlopen_ext"));
}
} // namespace

void loadNativeOverrides() {
    if (gLoaded) return;
    loadKeyValueFile(kDeviceProps, gProps);
    unordered_map<string, string> proc;
    loadKeyValueFile(kProcOverrides, proc);
    auto cpu = proc.find("CPUINFO_HARDWARE");
    if (cpu != proc.end()) gCpuHardware = cpu->second;
    auto kernel = proc.find("PROC_VERSION");
    if (kernel != proc.end()) gKernelVersion = kernel->second;
    gLoaded = true;
    LOGI("DevicePreset native overrides loaded: properties=%zu cpuinfo=%d proc_version=%d",
         gProps.size(), !gCpuHardware.empty(), !gKernelVersion.empty());
}

void installNativeHooks() {
    if (gInstalled) return;
    gInstalled = true;
    resolveOriginals();
    PatchContext ctx;
    dl_iterate_phdr(patchLoadedObject, &ctx);
    LOGI("DevicePreset Zygisk native PLT hooks installed: patched_slots=%zu properties=%zu cpuinfo=%d proc_version=%d",
         ctx.slotsPatched, gProps.size(), !gCpuHardware.empty(), !gKernelVersion.empty());
}
