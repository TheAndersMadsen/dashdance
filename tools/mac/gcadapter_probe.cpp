#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <cstdio>
#include <unistd.h>
template <class T> T** q(IOCFPlugInInterface** p, CFUUIDRef id){T** o=nullptr;(*p)->QueryInterface(p,CFUUIDGetUUIDBytes(id),(LPVOID*)&o);return o;}
int main(){alarm(10);
 CFMutableDictionaryRef m=IOServiceMatching(kIOUSBHostDeviceClassName);int v=0x057e,p=0x0337;
 CFDictionarySetValue(m,CFSTR(kUSBVendorID),CFNumberCreate(0,kCFNumberSInt32Type,&v));CFDictionarySetValue(m,CFSTR(kUSBProductID),CFNumberCreate(0,kCFNumberSInt32Type,&p));
 io_iterator_t it=0;IOServiceGetMatchingServices(kIOMainPortDefault,m,&it);io_service_t s=IOIteratorNext(it);if(!s){puts("no adapter");return 1;}
 IOCFPlugInInterface**pl=0;SInt32 sc;IOCreatePlugInInterfaceForService(s,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&pl,&sc);
 auto d=q<IOUSBDeviceInterface942>(pl,kIOUSBDeviceInterfaceID942);printf("dev open %08x\n",(*d)->USBDeviceOpenSeize(d));
 IOUSBFindInterfaceRequest r{kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare};
 io_iterator_t ii=0;(*d)->CreateInterfaceIterator(d,&r,&ii);io_service_t i=IOIteratorNext(ii);
 if(!i){UInt8 nc=0;(*d)->GetNumberOfConfigurations(d,&nc);IOUSBConfigurationDescriptorPtr cd;(*d)->GetConfigurationDescriptorPtr(d,0,&cd);printf("no interface; configs %d, setting config %d -> %08x\n",nc,cd->bConfigurationValue,(*d)->SetConfiguration(d,cd->bConfigurationValue));(*d)->CreateInterfaceIterator(d,&r,&ii);i=IOIteratorNext(ii);if(!i){puts("still no interface");return 1;}}
 IOCFPlugInInterface**ip=0;IOCreatePlugInInterfaceForService(i,kIOUSBInterfaceUserClientTypeID,kIOCFPlugInInterfaceID,&ip,&sc);
 auto f=q<IOUSBInterfaceInterface942>(ip,kIOUSBInterfaceInterfaceID942);printf("iface open %08x\n",(*f)->USBInterfaceOpenSeize(f));
 UInt8 n=0,pin=0,pout=0;(*f)->GetNumEndpoints(f,&n);
 for(UInt8 k=1;k<=n;k++){UInt8 di,nu,ty,iv;UInt16 pk;(*f)->GetPipeProperties(f,k,&di,&nu,&ty,&pk,&iv);printf("pipe %d dir %d interval %d\n",k,di,iv);if(di==kUSBIn)pin=k;else pout=k;}
 printf("SetPipePolicy 1ms -> %08x\n",(*f)->SetPipePolicy(f,pin,37,1));
 UInt8 init=0x13;printf("init %08x\n",(*f)->WritePipe(f,pout,&init,1));
 CFAbsoluteTime t0=CFAbsoluteTimeGetCurrent();int cnt=0;UInt8 b[37]={0};
 while(CFAbsoluteTimeGetCurrent()-t0<1.5){UInt32 sz=37;if((*f)->ReadPipe(f,pin,b,&sz)==kIOReturnSuccess)cnt++;}
 printf("%.0f reports/s; last:",cnt/(CFAbsoluteTimeGetCurrent()-t0));for(int x=0;x<10;x++)printf(" %02x",b[x]);puts("");
 (*f)->USBInterfaceClose(f);(*d)->USBDeviceClose(d);}
