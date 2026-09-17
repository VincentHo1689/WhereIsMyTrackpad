// appsim.c - mimics how LaunchNext/StrokeMouse pick a device.
#include <dlfcn.h>
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
typedef void*MTDeviceRef;
static const char*FW="/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport";
int main(void){setvbuf(stdout,NULL,_IONBF,0);
  void*h=dlopen(FW,RTLD_LAZY);
  if(!h){printf("dlopen FAIL\n");return 2;}
  typedef MTDeviceRef(*cd_t)(void); cd_t cd=(cd_t)dlsym(h,"MTDeviceCreateDefault");
  typedef CFArrayRef(*cl_t)(void); cl_t cl=(cl_t)dlsym(h,"MTDeviceCreateList");
  typedef int(*i2_t)(MTDeviceRef,int*); i2_t fam=(i2_t)dlsym(h,"MTDeviceGetFamilyID");
  MTDeviceRef d=cd(); int f=-1; fam(d,&f);
  printf("MTDeviceCreateDefault -> family=%d %s\n",f,f==176?"(Touch Bar)":f==113?"(Trackpad)":"?");
  CFArrayRef a=cl(); long n=(long)CFArrayGetCount(a);
  printf("MTDeviceCreateList count=%ld order:",n);
  for(long i=0;i<n;i++){int g=-1;fam((MTDeviceRef)CFArrayGetValueAtIndex(a,i),&g);printf(" [%ld]=%d",i,g);}
  printf("\n");
  printf("%s\n", f==113?"PASS: default is the trackpad":"FAIL: default is not the trackpad");
  return f==113?0:1;}
