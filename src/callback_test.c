// callback_test.c - verifies the device returned by MTDeviceCreateDefault()
// actually delivers contact frames. Run with the patch injected.
#include <dlfcn.h>
#include <stdio.h>
#include <stdbool.h>
#include <CoreFoundation/CoreFoundation.h>
typedef void*MTDeviceRef;
static volatile int g_frames=0,g_maxFingers=0;
static int on_frame(void*dev,void*data,int nFingers,double ts,int frame){
  (void)dev;(void)data;(void)ts;(void)frame; g_frames++; if(nFingers>g_maxFingers)g_maxFingers=nFingers; return 0;
}
int main(int argc,char**argv){setvbuf(stdout,NULL,_IONBF,0);
  int seconds = argc>1?atoi(argv[1]):10;
  void*h=dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport",RTLD_LAZY);
  if(!h){printf("dlopen FAIL\n");return 2;}
  typedef MTDeviceRef(*cd_t)(void); cd_t cd=(cd_t)dlsym(h,"MTDeviceCreateDefault");
  typedef int(*i2_t)(MTDeviceRef,int*); i2_t fam=(i2_t)dlsym(h,"MTDeviceGetFamilyID");
  typedef void(*reg_t)(MTDeviceRef,void*); reg_t reg=(reg_t)dlsym(h,"MTRegisterContactFrameCallback");
  typedef void(*start_t)(MTDeviceRef,int); start_t start=(start_t)dlsym(h,"MTDeviceStart");
  typedef bool(*run_t)(MTDeviceRef); run_t running=(run_t)dlsym(h,"MTDeviceIsRunning");
  MTDeviceRef d=cd(); int f=-1; fam(d,&f);
  printf("default family=%d (%s)\n",f,f==176?"Touch Bar":f==113?"Trackpad":"?");
  printf("register=%p\n",(void*)reg);
  if(reg) reg(d,on_frame);
  if(start) start(d,0);
  printf("running=%d\n",running?running(d):-1);
  printf("Touch the %s for %d seconds...\n", f==113?"TRACKPAD":"TOUCH BAR", seconds);
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
  printf("RESULT frames=%d maxFingers=%d\n",g_frames,g_maxFingers);
  return g_frames>0?0:3;}
