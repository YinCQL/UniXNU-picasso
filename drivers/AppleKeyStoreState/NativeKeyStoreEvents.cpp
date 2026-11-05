// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from AppleKeyStoreState 42ff83e08b2265fe741d4adaf084bebe003452ba.
// State queries for Picasso's unprotected, ephemeral Data environment only.
// No keybag exists here. No selector creates, loads, unlocks or returns one.
// Contract mirrored from UniXNU/NativeKeyStoreEvents:
//   0, 4, 5, 9, 12, 13, 26, 27, 55, 101, 102: no-output bookkeeping
//   14: synthetic system keybag handle 0
//   7: unlocked since boot
//   17, 35: truthful device-state DER
//   31: empty configuration DER for the no-keybag environment
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>

extern "C" bool picasso_aks_state_enabled(void);
extern "C" void picasso_aks_observed(unsigned selector, IOReturn result,
    unsigned scalar_input_count, unsigned scalar_output_count,
    unsigned structure_input_size, unsigned structure_output_size,
    uint64_t context);

// The original 21H461 decoder accepts a SET of UTF8String/INTEGER pairs.
// ss=4: unlocked since boot; sls=3: no passcode. Remaining state is absent.
// These are device state facts for the gated unprotected RAM Data environment,
// never authentication results for an imported keybag or another user.
static size_t encodeState(uint8_t *out, size_t capacity) {
    struct Entry { const char *key; uint8_t value; };
    const Entry entries[] = {{"ss",4},{"sls",3},{"sb",0},{"sfa",0},{"sgs",0},
        {"sas",0},{"sgpe",0},{"srcd",0},{"sms",0},{"bh",0}};
    size_t body=0;
    for(const auto &entry:entries) body+=7+strlen(entry.key);
    if(!out || body>=128 || capacity<body+2)return 0;
    size_t n=0;out[n++]=0x31;out[n++]=(uint8_t)body;
    for(const auto &entry:entries) {
        size_t length=strlen(entry.key);
        out[n++]=0x30;out[n++]=(uint8_t)(5+length);
        out[n++]=0x0c;out[n++]=(uint8_t)length;
        for(size_t i=0;i<length;++i)out[n++]=(uint8_t)entry.key[i];
        out[n++]=0x02;out[n++]=1;out[n++]=entry.value;
    }
    return n;
}

class AppleKeyStoreUserClient final : public IOUserClient {
    OSDeclareDefaultStructors(AppleKeyStoreUserClient)
    unsigned handshaken=0;
public:
    IOReturn clientClose() override {
        __atomic_store_n(&handshaken,0,__ATOMIC_RELEASE);
        terminate();return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *args,
        IOExternalMethodDispatch *,OSObject *,void *) override {
        IOReturn rc=query(selector,args);
        const unsigned inputCount=args?args->scalarInputCount:0;
        const unsigned outputCount=args?args->scalarOutputCount:0;
        const unsigned inputSize=args?args->structureInputSize:0;
        const unsigned outputSize=args?args->structureOutputSize:0;
        const uint64_t context=args && inputCount && args->scalarInput ?
            args->scalarInput[0] : 0;
        picasso_aks_observed(selector,rc,inputCount,outputCount,inputSize,
            outputSize,context);
        return rc;
    }
private:
    IOReturn query(uint32_t selector,IOExternalMethodArguments *args) {
        if(!picasso_aks_state_enabled() || isInactive())return kIOReturnNotReady;
        if(!args)return kIOReturnBadArgument;
        // No async, structure-input, mutation or key-operation contracts.
        if(args->asyncWakePort || args->asyncReferenceCount || args->structureInputSize ||
            args->structureInputDescriptor)return kIOReturnBadArgument;
        if(selector==0) {
            if(args->scalarInputCount || args->scalarOutputCount || args->structureOutputSize ||
                args->structureOutputDescriptor)return kIOReturnBadArgument;
            // Establish this actual client session; idempotent, with no bag state.
            __atomic_store_n(&handshaken,1,__ATOMIC_RELEASE);return kIOReturnSuccess;
        }
        if(!__atomic_load_n(&handshaken,__ATOMIC_ACQUIRE))return kIOReturnNotOpen;
        switch(selector) {
        case 4: case 5: case 9: case 12: case 13: case 26: case 27:
        case 55: case 101: case 102:
            args->scalarOutputCount=0;
            return kIOReturnSuccess;
        default:
            break;
        }
        if(selector==14) {
            if(args->scalarOutputCount!=1 || !args->scalarOutput)return kIOReturnBadArgument;
            // keybagd accepts this upstream-compatible handle without systembag.kb.
            args->scalarOutput[0]=0;
            return kIOReturnSuccess;
        }
        const uint64_t supportedSelectors=(1ull<<7)|(1ull<<17)|
            (1ull<<31)|(1ull<<35);
        if(selector>=64 || ((supportedSelectors>>selector)&1)==0)
            return kIOReturnUnsupported;
        if(args->scalarInputCount!=1 || !args->scalarInput)return kIOReturnBadArgument;
        if(selector==7) {
            if(args->scalarOutputCount!=1 || !args->scalarOutput || args->structureOutputSize ||
                args->structureOutputDescriptor)return kIOReturnBadArgument;
            args->scalarOutput[0]=4;return kIOReturnSuccess;
        }
        if(args->scalarOutputCount)return kIOReturnBadArgument;
        // Selector 31 is a configuration query, not a device-state query.
        // With no persistent keybag policy, its truthful result is an empty DER SET.
        static const uint8_t emptyConfiguration[]={0x31,0x00};
        uint8_t stateDer[128];
        const uint8_t *der=emptyConfiguration;
        size_t length=sizeof(emptyConfiguration);
        if(selector!=31) {
            der=stateDer;
            length=encodeState(stateDer,sizeof(stateDer));
            if(!length)return kIOReturnError;
        }
        if(args->structureOutputDescriptor) {
            IOMemoryDescriptor *memory=args->structureOutputDescriptor;
            if(length>args->structureOutputDescriptorSize || length>memory->getLength())return kIOReturnNoSpace;
            // Out-of-line userclient arguments arrive as an address-range
            // descriptor that is not wired. writeBytes() walks physical
            // segments and panics unless this descriptor is prepared first.
            IOReturn rc=memory->prepare(kIODirectionIn);
            if(rc!=kIOReturnSuccess)return rc;
            const bool copied=memory->writeBytes(0,der,length)==length;
            memory->complete(kIODirectionIn);
            if(!copied)return kIOReturnUnderrun;
            args->structureOutputDescriptorSize=(uint32_t)length;
        } else {
            if(!args->structureOutput || length>args->structureOutputSize)return kIOReturnNoSpace;
            memcpy(args->structureOutput,der,length);args->structureOutputSize=(uint32_t)length;
        }
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(AppleKeyStoreUserClient,IOUserClient)

class AppleKeyStore final : public IOService {
    OSDeclareDefaultStructors(AppleKeyStore)
    IOReturn openState(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **out) {
        if(out)*out=nullptr;
        if(!out || !task)return kIOReturnBadArgument;
        if(!picasso_aks_state_enabled() || isInactive())return kIOReturnNotReady;
        if(type!=0)return kIOReturnUnsupported;
        AppleKeyStoreUserClient *client=new AppleKeyStoreUserClient;
        if(!client)return kIOReturnNoMemory;
        if(!client->initWithTask(task,security,type,properties)){client->release();return kIOReturnError;}
        if(!client->attach(this)){client->release();return kIOReturnError;}
        if(!client->start(this)){client->detach(this);client->release();return kIOReturnError;}
        *out=client;return kIOReturnSuccess;
    }
public:
    bool start(IOService *provider) override {
        if(!picasso_aks_state_enabled() || !IOService::start(provider))return false;
        setName("Picasso ephemeral keystore state");
        if(!setProperty("NativeKeyStoreEvents",true) || !setProperty("KeyOperationsAvailable",false) ||
            !setProperty("EphemeralStorageOnly",true) || !setProperty("StateQueriesOnly",true)) {
            IOService::stop(provider);return false;
        }
        registerService();return true;
    }
    IOReturn newUserClient(task_t t,void *s,UInt32 type,OSDictionary *p,IOUserClient **out) override {
        IOReturn rc=openState(t,s,type,p,out);
        picasso_aks_observed(256,rc,0,0,0,0,type);
        return rc;
    }
    IOReturn newUserClient(task_t t,void *s,UInt32 type,IOUserClient **out) override {
        return newUserClient(t,s,type,nullptr,out);
    }
};
OSDefineMetaClassAndStructors(AppleKeyStore,IOService)
extern "C" IOService *native_keyevents_allocate(void) {return new AppleKeyStore;}
