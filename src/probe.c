// probe.c - runtime inspection of MultitouchSupport.framework device enumeration.
// Determines the real ABI of the private getters and dumps the device list.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef void *MTDeviceRef;

typedef CFArrayRef (*create_list_t)(void);
typedef MTDeviceRef (*create_default_t)(void);
typedef int (*int_out_t)(MTDeviceRef, int *);
typedef int (*int_ret_t)(MTDeviceRef);
typedef bool (*bool_ret_t)(MTDeviceRef);
typedef void (*void_1arg_t)(MTDeviceRef);
typedef void (*start_t)(MTDeviceRef, int);

static const char *FW =
    "/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport";

#define RESOLVE(var, name)                                            \
    do {                                                              \
        *(void **)(&var) = dlsym(h, name);                            \
        printf("  %-32s %s\n", name, (var) ? "found" : "MISSING");    \
    } while (0)

int main(void) {
    void *h = dlopen(FW, RTLD_LAZY);
    if (!h) { printf("dlopen FAILED: %s\n", dlerror()); return 2; }
    printf("dlopen OK\n\nSymbols:\n");

    create_list_t createList;
    create_default_t createDefault;
    int_out_t getFamilyID_out, getSensorRows_out, getDeviceID_out, getSensorCols_out;
    int_ret_t getFamilyID_ret, getSensorRows_ret, getDeviceID_ret, getSensorCols_ret;
    bool_ret_t isBuiltIn, isOpaque, isRunning;
    start_t start;
    void_1arg_t stop, release;

    RESOLVE(createList, "MTDeviceCreateList");
    RESOLVE(createDefault, "MTDeviceCreateDefault");
    RESOLVE(getFamilyID_out, "MTDeviceGetFamilyID");
    RESOLVE(getFamilyID_ret, "MTDeviceGetFamilyID");
    RESOLVE(getSensorRows_out, "MTDeviceGetSensorRows");
    RESOLVE(getSensorRows_ret, "MTDeviceGetSensorRows");
    RESOLVE(getDeviceID_out, "MTDeviceGetDeviceID");
    RESOLVE(getDeviceID_ret, "MTDeviceGetDeviceID");
    RESOLVE(getSensorCols_out, "MTDeviceGetSensorColumns");
    RESOLVE(getSensorCols_ret, "MTDeviceGetSensorColumns");
    RESOLVE(isBuiltIn, "MTDeviceIsBuiltIn");
    RESOLVE(isOpaque, "MTDeviceIsOpaqueSurface");
    RESOLVE(isRunning, "MTDeviceIsRunning");
    RESOLVE(start, "MTDeviceStart");
    RESOLVE(stop, "MTDeviceStop");
    RESOLVE(release, "MTDeviceRelease");

    printf("\n--- ABI probe ---\n");
    if (createDefault) {
        MTDeviceRef d = createDefault();
        printf("MTDeviceCreateDefault -> %p\n", d);
        if (d) {
            int out = -999;
            int r = getFamilyID_ret ? getFamilyID_ret(d) : -1;
            if (getFamilyID_out) { out = -999; getFamilyID_out(d, &out); }
            printf("  GetFamilyID: return-form=%d  out-param-form=%d\n", r, out);
            int sr_out = -999, sr = -1;
            if (getSensorRows_ret) sr = getSensorRows_ret(d);
            if (getSensorRows_out) { sr_out = -999; getSensorRows_out(d, &sr_out); }
            printf("  GetSensorRows: return-form=%d  out-param-form=%d\n", sr, sr_out);
            printf("  IsBuiltIn=%d IsOpaque=%d\n",
                   isBuiltIn ? isBuiltIn(d) : -1, isOpaque ? isOpaque(d) : -1);
        }
    } else {
        printf("MTDeviceCreateDefault MISSING\n");
    }

    if (!createList) { printf("MTDeviceCreateList MISSING - cannot continue\n"); return 3; }

    CFArrayRef list = createList();
    CFIndex n = list ? CFArrayGetCount(list) : 0;
    printf("\n--- MTDeviceCreateList() ---\ncount=%ld\n", (long)n);
    int seenTrackpad = 0, seenTouchBar = 0;
    for (CFIndex i = 0; i < n; i++) {
        MTDeviceRef d = (MTDeviceRef)CFArrayGetValueAtIndex(list, i);
        int fam = getFamilyID_ret ? getFamilyID_ret(d) : -1;
        int rows = getSensorRows_ret ? getSensorRows_ret(d) : -1;
        int cols = getSensorCols_ret ? getSensorCols_ret(d) : -1;
        int devid = getDeviceID_ret ? getDeviceID_ret(d) : -1;
        printf("[%ld] ref=%p family=%d rows=%d cols=%d deviceID=%d builtin=%d opaque=%d running=%d\n",
               (long)i, d, fam, rows, cols, devid,
               isBuiltIn ? isBuiltIn(d) : -1,
               isOpaque ? isOpaque(d) : -1,
               isRunning ? isRunning(d) : -1);
        if (fam == 176) seenTouchBar = 1;
        if (fam == 113) seenTrackpad = 1;
    }
    printf("\nhasTouchBar(176)=%d hasTrackpad(113)=%d\n", seenTouchBar, seenTrackpad);
    if (seenTouchBar && seenTrackpad) printf("RESULT: BOTH PRESENT\n");
    else if (seenTouchBar) printf("RESULT: TOUCHBAR ONLY (bug reproduced)\n");
    else if (seenTrackpad) printf("RESULT: TRACKPAD ONLY\n");
    else printf("RESULT: NEITHER via list\n");
    return 0;
}
