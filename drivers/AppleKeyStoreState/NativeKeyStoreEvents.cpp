// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from AppleKeyStoreState 42ff83e08b2265fe741d4adaf084bebe003452ba.
// Compatibility stubs for Picasso's unencrypted developer Data only.
// No hardware keybag or password protection is implemented.
// RFC 3394 software wrapping uses the public, stable board identity only.
// The synthetic handle and no-op successes are explicitly development-only.
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
#include <libkern/crypto/aes.h>
#include <libkern/crypto/sha2.h>
extern "C" bool picasso_aks_device_id(uint8_t out[12]);

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


// The native 20H392 libaks ABI passes bag/class scalars and an n+8 byte blob.
// RFC 3394 authenticates the wrapped key; this public identity provides no
// hardware confidentiality. Reject protected bags and reference-key contracts.
static constexpr size_t maxKeyBytes=256;
static void clearBytes(void *memory,size_t n) {
    volatile uint8_t *p=(volatile uint8_t *)memory;while(n--)*p++=0;
}
static IOReturn writeOutput(IOExternalMethodArguments *args,const uint8_t *data,size_t n) {
    if(args->structureOutputDescriptor) {
        IOMemoryDescriptor *memory=args->structureOutputDescriptor;
        if(n>args->structureOutputDescriptorSize || n>memory->getLength())return kIOReturnNoSpace;
        IOReturn rc=memory->prepare(kIODirectionIn);if(rc)return rc;
        bool copied=memory->writeBytes(0,data,n)==n;
        IOReturn completed=memory->complete(kIODirectionIn);
        if(!copied)return kIOReturnUnderrun;
        if(completed)return completed;
        args->structureOutputDescriptorSize=(uint32_t)n;
    } else {
        if(!args->structureOutput || n>args->structureOutputSize)return kIOReturnNoSpace;
        memcpy(args->structureOutput,data,n);args->structureOutputSize=(uint32_t)n;
    }
    return kIOReturnSuccess;
}
static bool wrappingKey(uint64_t keyClass,uint8_t key[32]) {
    uint8_t id[12],kind[8];if(!picasso_aks_device_id(id))return false;
    for(unsigned i=0;i<8;++i)kind[i]=(uint8_t)(keyClass>>(8*i));
    static const char domain[]="Picasso unprotected RFC3394 v1";
    SHA256_CTX context;SHA256_Init(&context);
    SHA256_Update(&context,domain,sizeof(domain)-1);SHA256_Update(&context,id,sizeof(id));
    SHA256_Update(&context,kind,sizeof(kind));SHA256_Final(key,&context);
    clearBytes(&context,sizeof(context));clearBytes(id,sizeof(id));return true;
}
static IOReturn wrapKey(IOExternalMethodArguments *args,bool unwrap) {
    size_t input=args->structureInputSize;
    if(args->scalarInputCount!=2 || !args->scalarInput || args->scalarInput[0]!=0 ||
        args->scalarInput[1]<1 || args->scalarInput[1]>12)return kIOReturnUnsupported;
    if(args->structureInputDescriptor || !args->structureInput ||
        args->scalarOutputCount!=(unwrap?0u:1u) || (!unwrap && !args->scalarOutput))return kIOReturnBadArgument;
    if(input<(unwrap?24u:16u) || input>(unwrap?maxKeyBytes+8:maxKeyBytes) || input%8)return kIOReturnBadArgument;
    uint8_t key[32]={},blob[maxKeyBytes+8]={},block[16]={};
    union {aes_encrypt_ctx encrypt;aes_decrypt_ctx decrypt;} context;
    IOReturn result=kIOReturnError;
    if(!wrappingKey(args->scalarInput[1],key))return kIOReturnNotReady;
    size_t n=unwrap?input-8:input,words=n/8;
    memcpy(blob+8,((const uint8_t *)args->structureInput)+(unwrap?8:0),n);
    if(unwrap)memcpy(blob,args->structureInput,8);else memset(blob,0xa6,8);
    int error=unwrap?aes_decrypt_key(key,32,&context.decrypt):aes_encrypt_key(key,32,&context.encrypt);
    if(!error) {
        for(unsigned pass=0;pass<6 && !error;++pass) {
            for(size_t step=0;step<words && !error;++step) {
                size_t i=unwrap?words-step:step+1;
                uint64_t t=words*(unwrap?5-pass:pass)+i;
                memcpy(block,blob,8);memcpy(block+8,blob+8*i,8);
                if(unwrap)for(unsigned k=0;k<8;++k)block[7-k]^=(uint8_t)(t>>(8*k));
                error=unwrap?aes_decrypt(block,block,&context.decrypt):aes_encrypt(block,block,&context.encrypt);
                if(!unwrap)for(unsigned k=0;k<8;++k)block[7-k]^=(uint8_t)(t>>(8*k));
                memcpy(blob,block,8);memcpy(blob+8*i,block+8,8);
            }
        }
        uint8_t difference=0;if(unwrap)for(unsigned k=0;k<8;++k)difference|=blob[k]^0xa6;
        if(!error && !difference) {
            result=writeOutput(args,blob+(unwrap?8:0),n+(unwrap?0:8));
            if(!result && !unwrap)args->scalarOutput[0]=args->scalarInput[1];
        }
    }
    clearBytes(key,sizeof(key));clearBytes(blob,sizeof(blob));clearBytes(block,sizeof(block));clearBytes(&context,sizeof(context));
    return result;
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
        // Async and out-of-line input remain unsupported. Inline key input is
        // accepted only by the explicit wrap/unwrap selectors below.
        if(args->asyncWakePort || args->asyncReferenceCount ||
            args->structureInputDescriptor)return kIOReturnBadArgument;
        if(selector==0) {
            if(args->structureInputSize || args->scalarInputCount || args->scalarOutputCount || args->structureOutputSize ||
                args->structureOutputDescriptor)return kIOReturnBadArgument;
            // Establish this actual client session; idempotent, with no bag state.
            __atomic_store_n(&handshaken,1,__ATOMIC_RELEASE);return kIOReturnSuccess;
        }
        if(!__atomic_load_n(&handshaken,__ATOMIC_ACQUIRE))return kIOReturnNotOpen;
        if(selector==10 || selector==11)return wrapKey(args,selector==11);
        if(args->structureInputSize)return kIOReturnBadArgument;
        switch(selector) {
        case 4: case 5: case 9: case 12: case 13: case 26: case 27:
        case 55: case 101: case 102:
            args->scalarOutputCount=0;
            args->structureOutputSize=0;
            args->structureOutputDescriptorSize=0;
            return kIOReturnSuccess;
        default:
            break;
        }
        if(selector==14) {
            if(args->scalarOutputCount!=1 || !args->scalarOutput || args->structureOutputSize ||
                args->structureOutputDescriptor)return kIOReturnBadArgument;
            // Reference compatibility placeholder: no systembag.kb is created or loaded.
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
            IOReturn completed=memory->complete(kIODirectionIn);
            if(!copied)return kIOReturnUnderrun;
            if(completed!=kIOReturnSuccess)return completed;
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
        setName("Picasso unprotected keystore compatibility");
        if(!setProperty("SoftwareKeyWrapping",true) || !setProperty("HardwareKeyProtection",false) ||
            !setProperty("NativeKeyStoreEvents",true) || !setProperty("KeyOperationsAvailable",true) ||
            !setProperty("UnprotectedStorageOnly",true) || !setProperty("CompatibilityStubs",true)) {
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
