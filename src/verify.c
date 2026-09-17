// verify.c - acceptance checks for ICanSeeMyTrackpadNow.
// Exit 0 = all checks passed.
#include <dlfcn.h>
#include <stdio.h>
#include <stdbool.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
typedef void*MTDeviceRef;
static int fails=0;
static void chk(const char*name,int ok,const char*detail){
  printf("  [%s] %s%s%s\n", ok?"PASS":"FAIL", name, detail&&*detail?" - ":"", detail?detail:"");
  if(!ok)fails++;
}
int main(void){setvbuf(stdout,NULL,_IONBF,0);
  void*h=dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport",RTLD_LAZY);
  if(!h){printf("dlopen FAIL\n");return 2;}
  typedef MTDeviceRef(*cd_t)(void); cd_t cd=(cd_t)dlsym(h,"MTDeviceCreateDefault");
  typedef CFArrayRef(*cl_t)(void); cl_t cl=(cl_t)dlsym(h,"MTDeviceCreateList");
  typedef int(*i2_t)(MTDeviceRef,int*); i2_t fam=(i2_t)dlsym(h,"MTDeviceGetFamilyID");
  typedef void(*s_t)(MTDeviceRef,int); s_t S=(s_t)dlsym(h,"MTDeviceStart");
  typedef bool(*r_t)(MTDeviceRef); r_t RUN=(r_t)dlsym(h,"MTDeviceIsRunning");
  char buf[128];
  printf("ICanSeeMyTrackpadNow - acceptance checks\n");

  MTDeviceRef d=cd(); int df=-1; fam(d,&df);
  int is_tp = (df==108 || df==113);
  snprintf(buf,sizeof buf,"MTDeviceCreateDefault family=%d",df);
  chk("default device is the built-in trackpad", is_tp, buf);
  chk("trackpad family is app-recognizable (108, not 113)",
      df==108 || getenv("ICSMT_KEEP_FAMILY"), buf);
  int running = -1;
  if(is_tp){ S(d,0); running=RUN?RUN(d):-1; snprintf(buf,sizeof buf,"running=%d",running);
    chk("default device starts", running==1, buf); }

  CFArrayRef a=cl(); long n=(long)CFArrayGetCount(a);
  snprintf(buf,sizeof buf,"count=%ld",n); chk("device list is non-empty", n>0, buf);
  int hasTP=0,hasTB=0; int firstFam=-1,lastFam=-1;
  for(long i=0;i<n;i++){int f=-1;fam((MTDeviceRef)CFArrayGetValueAtIndex(a,i),&f);
    if(f==113||f==108)hasTP=1; if(f==176)hasTB=1; if(i==0)firstFam=f; if(i==n-1)lastFam=f;}
  chk("trackpad present in list", hasTP, "");
  chk("touch bar present in list", hasTB, "");
  snprintf(buf,sizeof buf,"first=%d",firstFam); chk("trackpad is ordered first", firstFam!=176 && (firstFam==113||firstFam==108), buf);
  snprintf(buf,sizeof buf,"last=%d",lastFam); chk("touch bar is ordered last", lastFam==176, buf);

  // Touch Bar must remain usable.
  MTDeviceRef tb=0;
  for(long i=0;i<n;i++){int f=-1;MTDeviceRef x=(MTDeviceRef)CFArrayGetValueAtIndex(a,i);fam(x,&f);if(f==176)tb=x;}
  int tbrun=-1;
  if(tb){S(tb,0);tbrun=RUN?RUN(tb):-1;}
  snprintf(buf,sizeof buf,"running=%d",tbrun);
  chk("touch bar still starts", tbrun==1, buf);

  int fr=-1;
  if(n>0){ MTDeviceRef first=(MTDeviceRef)CFArrayGetValueAtIndex(a,0); S(first,0); fr=RUN?RUN(first):-1; }
  snprintf(buf,sizeof buf,"running=%d",fr);
  chk("first listed device (trackpad) starts", fr==1, buf);

  printf("\nRESULT: %s (%d check(s) failed)\n", fails?"FAIL":"PASS", fails);
  return fails?1:0;}
