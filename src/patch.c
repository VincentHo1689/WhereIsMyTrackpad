// patch.c - ICanSeeMyTrackpadNow
//
// Interposes three symbols in MultitouchSupport.framework so apps that ask for
// "the" multitouch device receive the built-in trackpad instead of the Touch Bar,
// and recognise it as a trackpad.
//
// Verified root cause (MacBook Pro 13" M2, Touch Bar; macOS 26):
//   MTDeviceCreateDefault() returned Family ID 176 (Touch Bar) rather than the
//   trackpad (Family ID 113), because the Touch Bar registers first. Apps such as
//   StrokeMouse and LaunchNext select their device via MTDeviceCreateDefault(),
//   so they only ever saw Touch Bar frames and never the trackpad.
//
// Fix:
//   - MTDeviceCreateDefault(): if the framework returns the Touch Bar, substitute
//     a trackpad device built from its IOKit service (startable, retained for the
//     process lifetime).
//   - MTDeviceCreateList(): reorder so the trackpad comes first and the Touch Bar
//     last, for apps that take the first element of the list.
//   - MTDeviceGetFamilyID(): report the built-in trackpad (raw family 113) as
//     family 108 ("MacBook trackpad"), because several apps classify 113 as a
//     Magic Mouse and therefore refuse to treat it as a trackpad.
//
// Both Touch Bar and trackpad remain present and usable.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TOUCHBAR_FAMILY 176
#define TRACKPAD_FAMILY 113
// Some apps classify family 113 as "Magic Mouse" rather than a trackpad (LaunchNext
// ships OpenMultitouchSupport, whose table maps 112/113 to Magic Mouse), so the
// built-in trackpad is filtered out even when it is returned first. Report a family
// those apps treat as a MacBook trackpad instead. ICSMT_KEEP_FAMILY=1 disables.
#define TRACKPAD_FAMILY_REPORTED 108

typedef void *MTDeviceRef;
typedef CFArrayRef (*MTDeviceCreateList_t)(void);
typedef MTDeviceRef (*MTDeviceCreateDefault_t)(void);
typedef MTDeviceRef (*MTDeviceCreateFromService_t)(io_service_t);
typedef int (*MTDeviceGetFamilyID_t)(MTDeviceRef, int *);
typedef void (*MTRegisterContactFrameCallback_t)(MTDeviceRef, const void *);
typedef void (*MTDeviceStart_t)(MTDeviceRef, int);

extern CFArrayRef MTDeviceCreateList(void);
extern MTDeviceRef MTDeviceCreateDefault(void);
extern int MTDeviceGetFamilyID(MTDeviceRef, int *);
extern void MTRegisterContactFrameCallback(MTDeviceRef, const void *);
extern void MTDeviceStart(MTDeviceRef, int);

// Direct references to the interposed symbols. dyld binds references *inside* the
// interposing image to the original implementations (an image is never interposed
// against itself), so these are the real framework functions. dlsym() must not be
// used for these: on the framework handle it returns our own interposers.
static MTDeviceCreateList_t orig_createList = MTDeviceCreateList;
static MTDeviceCreateDefault_t orig_createDefault = MTDeviceCreateDefault;
static MTDeviceGetFamilyID_t orig_getFamily = MTDeviceGetFamilyID;
static MTRegisterContactFrameCallback_t orig_registerContact = MTRegisterContactFrameCallback;
static MTDeviceStart_t orig_start = MTDeviceStart;

// Helpers we do not interpose, resolved through the framework image handle.
static io_service_t (*sym_getService)(MTDeviceRef);
static MTDeviceCreateFromService_t sym_createFromService;
static bool (*sym_isBuiltIn)(MTDeviceRef);

static MTDeviceRef cached_trackpad; // +1, kept for the process lifetime
static int resolved;                // 0 = untried, 1 = attempted

static int debug_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("ICSMT_DEBUG");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

// Startup logging. A handful of lines per process, so it is always appended to
// ~/Library/Logs/ICanSeeMyTrackpadNow.log (useful when the app was launched by
// Finder/launchd, where stderr is discarded); stderr is only used under ICSMT_DEBUG.
// honey: single static FILE*, unlocked, fine for a few startup lines; lock or rotate
// if call volume ever grows.
static void icsmt_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    if (debug_on()) fprintf(stderr, "[ICanSeeMyTrackpadNow] %s\n", line);

    static FILE *f;
    static int tried;
    if (!tried) {
        tried = 1;
        const char *home = getenv("HOME");
        if (home) {
            char dir[1024], path[1024];
            snprintf(dir, sizeof dir, "%s/Library/Logs", home);
            mkdir(dir, 0755);
            snprintf(path, sizeof path, "%s/ICanSeeMyTrackpadNow.log", dir);
            f = fopen(path, "a");
        }
    }
    if (f) { fprintf(f, "[%d] %s\n", (int)getpid(), line); fflush(f); }
}
#define LOG(...) icsmt_log(__VA_ARGS__)

static void resolve_helpers(void) {
    if (sym_getService && sym_createFromService && sym_isBuiltIn) return;
    void *h = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport",
                     RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport",
                       RTLD_LAZY);
    if (!h) return;
    if (!sym_getService) sym_getService = (io_service_t (*)(MTDeviceRef))dlsym(h, "MTDeviceGetService");
    if (!sym_createFromService)
        sym_createFromService = (MTDeviceCreateFromService_t)dlsym(h, "MTDeviceCreateFromService");
    if (!sym_isBuiltIn) sym_isBuiltIn = (bool (*)(MTDeviceRef))dlsym(h, "MTDeviceIsBuiltIn");
}

static CFStringRef cs(const char *s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
}

static int svc_numprop(io_service_t s, const char *key) {
    CFStringRef k = cs(key);
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    int r = -1;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &r);
    if (v) CFRelease(v);
    return r;
}

static int svc_strprop_equals(io_service_t s, const char *key, const char *want) {
    CFStringRef k = cs(key);
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    int match = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        char buf[256] = {0};
        if (CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8))
            match = strstr(buf, want) != NULL;
    }
    if (v) CFRelease(v);
    return match;
}

// Family ID is returned via an out-parameter on modern macOS and via the return
// value on older releases. Passing the extra argument is ABI-safe either way, so
// read both and keep whichever yields a plausible value.
static int family_of(MTDeviceRef d) {
    if (!orig_getFamily) return -1;
    int out = -1;
    int ret = orig_getFamily(d, &out);
    if (out >= 0 && out < 0x10000) return out;
    if (ret >= 0 && ret < 0x10000) return ret;
    return -1;
}

static int keep_family(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("ICSMT_KEEP_FAMILY");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

// Interposed MTDeviceGetFamilyID: keep the internal detection honest (the framework
// still sees 113) but report the MacBook-trackpad family to callers for the
// built-in trackpad, so apps that only accept known trackpad families work.
static int my_getFamily(MTDeviceRef d, int *out) {
    if (!orig_getFamily) return -1;
    int ret = orig_getFamily(d, out);
    if (keep_family()) return ret;
    int fam = (out && *out >= 0 && *out < 0x10000) ? *out
              : (ret >= 0 && ret < 0x10000) ? ret : -1;
    if (fam != TRACKPAD_FAMILY) return ret;
    resolve_helpers();
    if (sym_isBuiltIn && !sym_isBuiltIn(d)) return ret; // external Magic Mouse etc.
    if (out) *out = TRACKPAD_FAMILY_REPORTED;
    if (ret == TRACKPAD_FAMILY) ret = TRACKPAD_FAMILY_REPORTED;
    static int logged = 0;
    if (!logged) {
        logged = 1;
        LOG("reporting built-in trackpad family %d as %d for app compatibility",
            TRACKPAD_FAMILY, TRACKPAD_FAMILY_REPORTED);
    }
    return ret;
}

// Score an IOKit multitouch service as "the built-in trackpad"; Touch Bar excluded.
static int service_score(io_service_t s) {
    int fam = svc_numprop(s, "Family ID");
    if (fam == TOUCHBAR_FAMILY) return -1;
    int score = 0;
    if (fam == TRACKPAD_FAMILY) score += 10;
    if (svc_strprop_equals(s, "Product", "Trackpad")) score += 20;
    if (svc_strprop_equals(s, "Transport", "FIFO") ||
        svc_strprop_equals(s, "Transport", "SPI")) score += 5;
    return score;
}

static io_service_t best_trackpad_service(void) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("AppleMultitouchDevice"), &it) != KERN_SUCCESS)
        return 0;
    io_service_t svc, best = 0;
    int best_score = 0;
    while ((svc = IOIteratorNext(it))) {
        int score = service_score(svc);
        LOG("service 0x%x family=%d score=%d", (unsigned)svc, svc_numprop(svc, "Family ID"), score);
        if (score > best_score) {
            if (best) IOObjectRelease(best);
            best = svc; // keep the winner open (also keeps its IOKit entry alive)
            best_score = score;
        } else {
            IOObjectRelease(svc);
        }
    }
    IOObjectRelease(it);
    return best;
}

static MTDeviceRef resolve_trackpad(void) {
    if (resolved) return cached_trackpad;
    resolved = 1;
    resolve_helpers();
    if (!sym_createFromService) {
        LOG("MTDeviceCreateFromService unavailable; cannot resolve trackpad");
        return NULL;
    }
    io_service_t svc = best_trackpad_service();
    if (!svc) {
        LOG("no trackpad service found");
        return NULL;
    }
    // MTDeviceCreateFromService follows the Create rule: the returned ref is +1
    // and owned by us, so it stays valid for the process lifetime.
    cached_trackpad = sym_createFromService(svc);
    // Keep svc open for the process lifetime as well; releasing it is safe but we
    // avoid any dependency on the framework retaining the IOKit entry itself.
    LOG("selected trackpad ref=%p family=%d", cached_trackpad,
        cached_trackpad ? family_of(cached_trackpad) : -1);
    return cached_trackpad;
}

static MTDeviceRef my_createDefault(void) {
    resolve_helpers();
    MTDeviceRef orig = orig_createDefault ? orig_createDefault() : NULL;
    int fam = orig ? family_of(orig) : -1;
    if (orig && fam != TOUCHBAR_FAMILY) return orig; // already an acceptable device
    MTDeviceRef tp = resolve_trackpad();
    if (!tp) {
        LOG("trackpad unresolved; returning original device (family=%d)", fam);
        return orig;
    }
    LOG("substituting trackpad for framework default (family=%d -> %d)", fam, family_of(tp));
    return tp;
}

static CFArrayRef my_createList(void) {
    CFArrayRef orig = orig_createList ? orig_createList() : NULL;
    if (!orig) return NULL;
    CFIndex n = CFArrayGetCount(orig);
    if (n <= 1) return orig;

    resolve_helpers();
    CFMutableArrayRef out = CFArrayCreateMutable(kCFAllocatorDefault, n, &kCFTypeArrayCallBacks);
    if (!out) return orig;
    for (CFIndex i = 0; i < n; i++) { // non-Touch-Bar first
        MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(orig, i);
        if (family_of(d) != TOUCHBAR_FAMILY) CFArrayAppendValue(out, d);
    }
    for (CFIndex i = 0; i < n; i++) { // Touch Bar last
        MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(orig, i);
        if (family_of(d) == TOUCHBAR_FAMILY) CFArrayAppendValue(out, d);
    }
    if (CFArrayGetCount(out) != n) { // unknown device slipped through; keep original
        CFRelease(out);
        return orig;
    }
    LOG("reordered device list (%ld entries)", (long)n);
    CFRelease(orig);
    return out;
}

__attribute__((used)) static struct { const void *replacement; const void *replacee; }
    interpose_createList __attribute__((section("__DATA,__interpose"))) =
        { (const void *)my_createList, (const void *)MTDeviceCreateList };
__attribute__((used)) static struct { const void *replacement; const void *replacee; }
    interpose_createDefault __attribute__((section("__DATA,__interpose"))) =
        { (const void *)my_createDefault, (const void *)MTDeviceCreateDefault };
__attribute__((used)) static struct { const void *replacement; const void *replacee; }
    interpose_getFamily __attribute__((section("__DATA,__interpose"))) =
        { (const void *)my_getFamily, (const void *)MTDeviceGetFamilyID };

// Pass-through interposers used only to observe which device an app actually
// starts. They never change behaviour; the logging is debug-gated.
static void my_registerContact(MTDeviceRef d, const void *cb) {
    LOG("app registered a contact callback on family=%d", family_of(d));
    if (orig_registerContact) orig_registerContact(d, cb);
}

static void my_start(MTDeviceRef d, int mode) {
    LOG("app started device family=%d", family_of(d));
    if (orig_start) orig_start(d, mode);
}

__attribute__((used)) static struct { const void *replacement; const void *replacee; }
    interpose_registerContact __attribute__((section("__DATA,__interpose"))) =
        { (const void *)my_registerContact, (const void *)MTRegisterContactFrameCallback };
__attribute__((used)) static struct { const void *replacement; const void *replacee; }
    interpose_start __attribute__((section("__DATA,__interpose"))) =
        { (const void *)my_start, (const void *)MTDeviceStart };

__attribute__((constructor)) static void icsmt_init(void) {
    LOG("loaded (createList=%p createDefault=%p getFamily=%p)",
        (void *)orig_createList, (void *)orig_createDefault, (void *)orig_getFamily);
}
