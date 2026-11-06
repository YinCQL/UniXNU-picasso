// SPDX-License-Identifier: BSD-2-Clause-Patent
// API model for executing the complete service. Not an XNU SDK or hardware.
#pragma once
#include <stdint.h>
#include <stddef.h>
extern "C" void *memcpy(void *, const void *, size_t);
using IOReturn = int;
enum { kIOReturnSuccess=0, kIOReturnNotReady=1, kIOReturnIOError=2,
       kIOReturnBadArgument=3, kIOReturnNoMemory=4, kIOReturnNotFound=5 };
class OSMetaClass;
class OSObject {
    unsigned refs=1;
public:
    virtual ~OSObject() {}
    virtual const OSMetaClass *getMetaClass() const { return nullptr; }
    virtual void free() { delete this; }
    void retain() { ++refs; }
    void release() { if (!--refs) free(); }
};
class OSSymbol:public OSObject {
public:
    const char *name;
    static const OSSymbol *withCString(const char *);
    void release() const { const_cast<OSSymbol *>(this)->OSObject::release(); }
};
class OSMetaClass {
    const OSMetaClass *parent;
public:
    OSMetaClass(const char *, const OSMetaClass *p, unsigned):parent(p) {}
    virtual ~OSMetaClass() {}
    virtual OSObject *alloc() const { return nullptr; }
    const OSMetaClass *getSuperClass() const { return parent; }
    static const OSMetaClass *getMetaClassWithName(const OSSymbol *);
};
class OSSerialize;
class OSNumber:public OSObject {
public:
    uint64_t value;
    unsigned bits=64;
    bool serialize(OSSerialize *) const;
    unsigned numberOfBits() const { return bits; }
    static OSNumber *withNumber(uint64_t,unsigned);
    uint64_t unsigned64BitValue() { return value; }
};
class OSSerialize:public OSObject {
public:
    uint32_t words[4]={};
    static OSSerialize *binaryWithCapacity(unsigned, void *editor=nullptr, void *reference=nullptr);
    unsigned getLength() const { return sizeof(words); }
    char *text() { return reinterpret_cast<char *>(words); }
};
class OSDictionary:public OSObject {
public:
    const char *keys[16]={}; OSObject *values[16]={};unsigned count=0;
    ~OSDictionary();
    static OSDictionary *withCapacity(unsigned);
    bool setObject(const char *,OSObject *);
    OSObject *getObject(const char *);
    unsigned getCount() const { return count; }
};
using IORegistryEntry = OSObject;
class IOService:public OSObject {
    OSDictionary properties;
public:
    static OSMetaClass gMetaClass;
    explicit IOService(const OSMetaClass * = nullptr) {}
    virtual bool start(IOService *) { return true; }
    virtual void stop(IOService *) {}
    virtual IOReturn setProperties(OSObject *) { return kIOReturnBadArgument; }
    virtual void registerService() {}
    bool setProperty(const char *, OSObject *);
    bool setProperty(const char *, bool);
    bool setProperty(const char *, uint64_t, unsigned);
    OSObject *getProperty(const char *key) { return properties.getObject(key); }
};
struct IOLock {bool locked=false;};
IOLock *IOLockAlloc();void IOLockFree(IOLock *);void IOLockLock(IOLock *);void IOLockUnlock(IOLock *);
class IOTimerEventSource:public OSObject {
public:
    using Action=void (*)(OSObject *,IOTimerEventSource *);
    OSObject *owner;Action action;bool added=false,armed=false;uint64_t due=0;
    static IOTimerEventSource *timerEventSource(OSObject *,Action);
    IOReturn setTimeoutMS(unsigned);IOReturn setTimeoutUS(unsigned);
    void cancelTimeout() { armed=false; }
};
class IOWorkLoop:public OSObject {
public:
    static IOWorkLoop *workLoop();
    IOReturn addEventSource(IOTimerEventSource *);
    void removeEventSource(IOTimerEventSource *);
};
uint64_t mach_absolute_time();void absolutetime_to_nanoseconds(uint64_t,uint64_t *);
void IOLog(const char *,...) __attribute__((format(printf,1,2)));
#define OSSafeReleaseNULL(p) do {if(p)(p)->release();(p)=nullptr;}while(0)
#ifdef PICASSO_BRIGHTNESS_ARM_API
#define OSDynamicCast(t,p) static_cast<t *>(p)
#else
#define OSDynamicCast(t,p) dynamic_cast<t *>(p)
#endif
// Mirrors the metaclass constructor shape; allocation itself is host C++.
#define OSDeclareDefaultStructors(C) \
public: class MetaClass:public OSMetaClass {public:MetaClass();OSObject *alloc() const override;}; \
static MetaClass gMetaClass;static const OSMetaClass * const metaClass; \
static const OSMetaClass * const superClass;C(); \
const OSMetaClass *getMetaClass() const override; \
protected:explicit C(const OSMetaClass *);~C() override;
#define OSMetaClassConstructorInit(C,S,I) C::MetaClass::MetaClass():OSMetaClass(#C,C::superClass,sizeof(C)) { I; }
#define OSDefineDefaultStructors(C,S) OSObject *C::MetaClass::alloc() const {return new C;} C::C():S(&gMetaClass) {}
