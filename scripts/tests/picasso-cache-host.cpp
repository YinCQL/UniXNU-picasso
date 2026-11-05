// SPDX-License-Identifier: BSD-2-Clause-Patent
// Production cache stream writer with faulted block operations.
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <array>
#include <map>
#define Z_WAITOK 1
#define Z_ZERO 2
static uint8_t *kalloc_data(size_t n,int){return static_cast<uint8_t *>(calloc(1,n));}
static void kfree_data(void *p,size_t){free(p);}
static std::map<uint64_t,std::array<uint8_t,4096>> medium;
static unsigned operations=0,failAt=0,depth=0;
static bool open(uint64_t n,uint64_t *first,const char **why){assert(depth==1 && n==65536);*first=0;*why="";return true;}
static int write(uint64_t b,uint32_t n,const void *p){assert(depth==1 && n==1);if(++operations==failAt)return 0;memcpy(medium[b].data(),p,4096);return 1;}
static int sync(){assert(depth==1);return ++operations!=failAt;}
static auto native_ufs_aux_open=&open;
static auto native_ufs_aux_write=&write;
static auto native_ufs_aux_sync=&sync;
static int picasso_cache_lock;
static void lck_mtx_lock(int *){assert(!depth);++depth;}
static void lck_mtx_unlock(int *){assert(depth==1);--depth;}
#include "cache-writer.inc"
static uint32_t field(unsigned offset){uint32_t v;memcpy(&v,medium[PICASSO_PIPE_BLOCK].data()+offset,4);return v;}
namespace Console {
constexpr uint32_t PURing_SIZE=256*1024,PURing_MASK=PURing_SIZE-1;
static struct {uint64_t total;char buffer[PURing_SIZE];} pur_ring;
static unsigned pur_lock=0,pur_dropped=3,unlocks=0,overwriteAt=0;
#define MIN(a,b) ((a)<(b)?(a):(b))
#define os_atomic_cmpxchg(p,a,b,o) ((*(p)==(a))?(*(p)=(b),true):false)
#define os_atomic_load(p,o) (*(p))
static void unlock(unsigned *p,unsigned v){*p=v;if(++unlocks==overwriteAt)pur_ring.total+=PURing_SIZE+1;}
#define os_atomic_store(p,v,o) unlock(p,v)
#include "console-snapshot.inc"
static void run(){
    std::array<uint8_t,PURing_SIZE> out;uint64_t total=0,dropped=0;
    assert(!picasso_console_snapshot(out.data(),PURing_SIZE,&total,&dropped));
    pur_ring.total=PURing_SIZE+100;
    for(uint64_t i=pur_ring.total-PURing_SIZE;i<pur_ring.total;++i)pur_ring.buffer[i&PURing_MASK]=char(i%251);
    assert(picasso_console_snapshot(out.data(),PURing_SIZE,&total,&dropped)==PURing_SIZE && total==pur_ring.total && dropped==3 && !pur_lock);
    for(unsigned i=0;i<PURing_SIZE;++i)assert(out[i]==(100+i)%251);
    pur_lock=1;assert(!picasso_console_snapshot(out.data(),PURing_SIZE,&total,&dropped));pur_lock=0;
    unlocks=0;overwriteAt=2;assert(!picasso_console_snapshot(out.data(),PURing_SIZE,&total,&dropped) && !pur_lock);
}
}
int main(){
    Console::run();
    std::array<uint8_t,8192> body;body.fill(0x41);
    assert(!boot_log_export_put(1,8192,0,body.data(),1000));
    assert(boot_log_export_put(1,8192,999,body.data(),1000)==-3 && !picasso_pipe_active && !depth);
    assert(!boot_log_export_put(1,8192,0,body.data(),1000));
    assert(boot_log_export_put(2,8192,1000,body.data(),1000)==-3 && !picasso_pipe_active);
    assert(!boot_log_export_put(3,8192,0,body.data(),1000));
    assert(!boot_log_export_put(3,8192,1000,body.data()+1000,7192));
    assert(field(12)==8192 && field(24)==1 && field(16)==picasso_lsp_hash_step(2166136261u,body.data(),body.size()));
    for(unsigned fail=1;fail<=7;++fail){
        operations=0;failAt=fail;
        assert(boot_log_export_put(4,8192,0,body.data(),8192)==-6 && !picasso_pipe_active && !depth);
    }
    failAt=0;
    assert(!boot_log_export_put(5,19,0,body.data(),19) && field(24)==1);
    for(unsigned i=19;i<4096;++i)assert(!medium[PICASSO_PIPE_BLOCK+1][i]);
    puts("PASS cache stream: ordered offsets/generation, real flush failures, lock balance and tail padding");
}
