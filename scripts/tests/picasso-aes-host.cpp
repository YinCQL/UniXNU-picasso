// Production AES service with host IOKit descriptors and Windows CNG CBC.
// CNG performs real cryptography; this does not execute Apple corecrypto.
#include <windows.h>
#include <bcrypt.h>
#undef interface
#pragma comment(lib, "bcrypt.lib")
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using IOReturn = int;
using task_t = void *;
using mach_vm_address_t = uintptr_t;
static task_t kernel_task = reinterpret_cast<void *>(1);
constexpr int kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnUnsupported=2,
              kIOReturnNoMemory=3, kIOReturnIOError=4;
constexpr unsigned kIODirectionIn=1, kIODirectionOut=2, kIODirectionInOut=3;
static int allocations, published, callbacks, callbackStatus, failure;
static bool corrupt, cryptoFailure;
#define OSDeclareDefaultStructors(C)
#define OSDefineMetaClassAndStructors(C, B)
#define __ATOMIC_RELAXED 0
#define __atomic_fetch_add host_add
static uint32_t host_add(volatile uint32_t *p, uint32_t n, int) { auto old=*p; *p+=n; return old; }
static void IOLog(const char *, ...) {}
static void *IOMalloc(size_t n) { if(failure==1) return nullptr; ++allocations; return malloc(n); }
static void IOFree(void *p,size_t) { if(p) --allocations; free(p); }
static void cc_clear(size_t n, void *p) { SecureZeroMemory(p,n); }
struct aes_encrypt_ctx { unsigned char key[32]; unsigned bytes; };
using aes_decrypt_ctx = aes_encrypt_ctx;
static int aes_encrypt_key(const unsigned char *key,unsigned size,aes_encrypt_ctx *ctx) {
  memcpy(ctx->key,key,size); ctx->bytes=size; return cryptoFailure?1:0;
}
static int aes_decrypt_key(const unsigned char *key,unsigned size,aes_decrypt_ctx *ctx) { return aes_encrypt_key(key,size,ctx); }
static int transform(const unsigned char *in,const unsigned char *iv,unsigned blocks,unsigned char *out,const aes_encrypt_ctx *ctx,bool encrypt) {
  BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_KEY_HANDLE key=nullptr;
  assert(BCryptOpenAlgorithmProvider(&alg,BCRYPT_AES_ALGORITHM,nullptr,0)>=0);
  assert(BCryptSetProperty(alg,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_CBC)),sizeof(BCRYPT_CHAIN_MODE_CBC),0)>=0);
  assert(BCryptGenerateSymmetricKey(alg,&key,nullptr,0,const_cast<PUCHAR>(ctx->key),ctx->bytes,0)>=0);
  unsigned char chaining[16];memcpy(chaining,iv,16);ULONG written=0;
  auto status=encrypt?BCryptEncrypt(key,const_cast<PUCHAR>(in),blocks*16,nullptr,chaining,16,out,blocks*16,&written,0):
                      BCryptDecrypt(key,const_cast<PUCHAR>(in),blocks*16,nullptr,chaining,16,out,blocks*16,&written,0);
  BCryptDestroyKey(key);BCryptCloseAlgorithmProvider(alg,0);
  if(status<0 || written!=blocks*16) return 1;
  if(corrupt) out[0]^=1;
  return 0;
}
static int aes_encrypt_cbc(const unsigned char *in,const unsigned char *iv,unsigned blocks,unsigned char *out,const aes_encrypt_ctx *ctx) { return transform(in,iv,blocks,out,ctx,true); }
static int aes_decrypt_cbc(const unsigned char *in,const unsigned char *iv,unsigned blocks,unsigned char *out,const aes_decrypt_ctx *ctx) { return transform(in,iv,blocks,out,ctx,false); }
static struct Crypto { bool ccaes_cbc_encrypt=true,ccaes_cbc_decrypt=true; } crypto;
static Crypto *g_crypto_funcs=&crypto;
struct OSSymbol {};
struct IOWorkLoop {};
struct IOService {
  virtual ~IOService()=default;
  virtual bool start(IOService *) { return failure!=2; }
  virtual IOWorkLoop *getWorkLoop() const { return nullptr; }
  virtual IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) { return kIOReturnUnsupported; }
  void setProperty(const char *,bool) {}
  void registerService() { ++published; }
};
struct IOMemoryDescriptor {
  std::vector<unsigned char> owned;
  unsigned char *data; size_t size;
  int prepares=0,completes=0,prepareError=0;
  bool shortRead=false,shortWrite=false;
  explicit IOMemoryDescriptor(size_t n):owned(n),data(owned.data()),size(n) {}
  IOMemoryDescriptor(void *p,size_t n):data(static_cast<unsigned char *>(p)),size(n) {}
  virtual ~IOMemoryDescriptor()=default;
  size_t getLength() const { return size; }
  int prepare() { ++prepares;return prepareError; }
  int complete() { ++completes;return 0; }
  size_t readBytes(unsigned long long offset,void *to,size_t n) {
    assert(offset<=size && n<=size-offset); if(shortRead) return 0;memcpy(to,data+offset,n);return n;
  }
  size_t writeBytes(unsigned long long offset,const void *from,size_t n) {
    assert(offset<=size && n<=size-offset);if(shortWrite) return 0;memcpy(data+offset,from,n);return n;
  }
  void release() { delete this; }
  static IOMemoryDescriptor *withAddressRange(mach_vm_address_t p,unsigned long long n,unsigned,task_t) {
    return failure==3?nullptr:new IOMemoryDescriptor(reinterpret_cast<void *>(p),static_cast<size_t>(n));
  }
};
struct IOBufferMemoryDescriptor: IOMemoryDescriptor {
  explicit IOBufferMemoryDescriptor(size_t n):IOMemoryDescriptor(n) {}
  static IOBufferMemoryDescriptor *withCapacity(size_t n,unsigned) { return new IOBufferMemoryDescriptor(n); }
};
struct IOAESIV { unsigned char bytes[16]; };
struct IOAESKeyData { uint32_t bits; unsigned char bytes[32]; uint32_t selector; };
enum IOAESOperation:unsigned { IOAESEncrypt=0,IOAESDecrypt=1 };
using IOAESCompletion=void (*)(void *,int);
struct IOAESAccelerator:IOService {
  virtual const char *getAlgorithmName()=0;
  virtual bool isKeySupported(unsigned)=0;
  virtual unsigned getSupportedOptions()=0;
  virtual IOReturn performAES(void *,void *,task_t,unsigned long long,IOAESIV *,IOAESOperation,IOAESKeyData *,IOAESCompletion,void *)=0;
  virtual IOReturn performAES(IOMemoryDescriptor *,IOMemoryDescriptor *,unsigned long long,IOAESIV *,IOAESOperation,IOAESKeyData *,unsigned long long,unsigned long long,IOAESCompletion,void *)=0;
};
#include "aes-production.inc"
static void done(void *,int status) { ++callbacks;callbackStatus=status; }
int main(int argc,char **argv) {
  assert(argc==2);int test=atoi(argv[1]);NativeAES aes;
  IOAESKeyData key={128,{0},0};IOAESIV iv={};
  IOMemoryDescriptor input(8192),output(8192);
  for(size_t i=0;i<input.size;++i) input.data[i]=static_cast<unsigned char>(i);
  if(test==0) { assert(aes.start(nullptr));assert(published==1 && native_aes_kat_status==1 && native_aes_interface_status==1); }
  else if(test==1) { g_crypto_funcs=nullptr;assert(!aes.start(nullptr));assert(!published); }
  else if(test==2) { corrupt=true;assert(!aes.start(nullptr));assert(!published && native_aes_kat_status==2); }
  else if(test==3) { key.selector=2001;assert(aes.performAES(&input,&output,16,&iv,IOAESEncrypt,&key,0,0,done,nullptr)==kIOReturnUnsupported);assert(!callbacks && !aes.isKeySupported(2001));key.selector=0;assert(aes.performAES(&input,&output,16,&iv,static_cast<IOAESOperation>(99),&key,0,0,done,nullptr)==kIOReturnUnsupported); }
  else if(test==4) { for(auto n:{0ULL,17ULL,8193ULL}) assert(aes.performAES(&input,&output,n,&iv,IOAESEncrypt,&key,0,0,done,nullptr)==kIOReturnBadArgument);assert(aes.performAES(&input,&output,16,&iv,IOAESEncrypt,&key,~0ULL,0,done,nullptr)==kIOReturnBadArgument);assert(!callbacks); }
  else if(test==5) { unsigned char expected[8192];aes_encrypt_ctx ctx;assert(!aes_encrypt_key(key.bytes,16,&ctx));assert(!aes_encrypt_cbc(input.data,iv.bytes,512,expected,&ctx));assert(!aes.performAES(&input,&output,8192,&iv,IOAESEncrypt,&key,0,0,done,nullptr));assert(!memcmp(output.data,expected,8192) && !memcmp(iv.bytes,expected+8176,16));memset(iv.bytes,0,16);assert(!aes.performAES(&output,&output,8192,&iv,IOAESDecrypt,&key,0,0,nullptr,nullptr));assert(!memcmp(input.data,output.data,8192));assert(callbacks==1 && !callbackStatus); }
  else if(test==6 || test==7 || test==10) { input.shortRead=test==6;output.shortWrite=test==7;cryptoFailure=test==10;assert(!aes.performAES(&input,&output,16,&iv,IOAESEncrypt,&key,0,0,done,nullptr));assert(callbacks==1 && callbackStatus==kIOReturnIOError);assert(input.completes==1 && output.completes==1);for(auto b:iv.bytes) assert(!b); }
  else if(test==8) { input.prepareError=9;assert(aes.performAES(&input,&output,16,&iv,IOAESEncrypt,&key,0,0,done,nullptr)==9);assert(!callbacks && !input.completes && !output.prepares);input.prepareError=0;output.prepareError=8;assert(aes.performAES(&input,&output,16,&iv,IOAESEncrypt,&key,0,0,done,nullptr)==8);assert(input.completes==1 && !output.completes && !callbacks); }
  else if(test==9) { failure=1;assert(aes.performAES(&input,&output,16,&iv,IOAESEncrypt,&key,0,0,done,nullptr)==kIOReturnNoMemory);failure=3;assert(aes.performAES(input.data,output.data,kernel_task,16,&iv,IOAESEncrypt,&key,done,nullptr)==kIOReturnNoMemory);assert(!callbacks); }
  else if(test==11) { failure=2;assert(!aes.start(nullptr));assert(!published); }
  else assert(false);
  assert(!allocations);puts("PASS: production AES vectors/interfaces/refusal/cleanup");
}
