// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PicassoFastrpc.h"
#include "PicassoRpcFs.h"
#include "WlanSmem.h"
namespace {
struct RpcStock { const char *path;uint32_t mode,size;const uint8_t *data; };
#include "PicassoRpcStock.inc"
enum { StockCount=sizeof(rpcStock)/sizeof(rpcStock[0]),MutableCount=128,FileBytes=8192,HandleCount=16 };
struct Node { char path[256];uint32_t mode,size;bool used,removed;uint8_t data[FileBytes]; };
struct Handle { uint32_t token,node,pos;bool used,directory,read,write,append,eof;int error; };
struct Session { Handle handles[HandleCount];uint32_t sequence; };
static Node nodes[MutableCount];
static Session sessions[2];
static bool equal(const char *a,const char *b) { while(*a && *a==*b) { ++a;++b; }return *a==*b; }
static unsigned length(const char *s) { unsigned n=0;while(s[n])++n;return n; }
static bool prefix(const char *s,const char *p) { while(*p)if(*s++!=*p++)return false;return true; }
static void copy(void *d,const void *s,unsigned n) { auto *a=(uint8_t *)d;auto *b=(const uint8_t *)s;while(n--)*a++=*b++; }
static bool string(const PicassoRpcArg &a,unsigned n,char *out,unsigned cap=256) {
    if(!n || n>cap || n>a.bytes || !a.data || a.data[n-1])return false;
    for(unsigned i=0;i<n-1;++i)if(a.data[i]<32 || a.data[i]>126)return false;
    copy(out,a.data,n);return true;
}
static void record(char *name,const char *s) { unsigned n=length(s);if(n>127)n=127;copy(name,s,n);name[n]=0; }
static bool canonical(const char *in,char *out) {
    const char *replacement=nullptr;unsigned skip=0;
    if(prefix(in,"/persist/")) { replacement="/mnt/vendor/persist/";skip=9; }
    else if(prefix(in,"/vendor/persist/")) { replacement="/mnt/vendor/persist/";skip=16; }
    unsigned n=0;if(replacement)while(*replacement)out[n++]=*replacement++;
    in+=skip;if(!n && *in!='/')return false;
    while(*in) { if(n>=255)return false;out[n++]=*in++; }out[n]=0;
    while(n>1 && out[n-1]=='/')out[--n]=0;
    for(unsigned i=0;i<n;++i) {
        if(out[i]=='\\')return false;
        if(out[i]=='/' && (out[i+1]=='/' || (out[i+1]=='.' && (!out[i+2] || out[i+2]=='/' ||
            (out[i+2]=='.' && (!out[i+3] || out[i+3]=='/'))))))return false;
    }return true;
}
static bool writable(const char *p) { return prefix(p,"/mnt/vendor/persist/sensors/"); }
static bool isDir(uint32_t m) { return (m&0170000)==0040000; }
static const char *path(unsigned i) { return i<StockCount?rpcStock[i].path:nodes[i-StockCount].path; }
static uint32_t mode(unsigned i) { return i<StockCount?rpcStock[i].mode:nodes[i-StockCount].mode; }
static uint32_t size(unsigned i) { return i<StockCount?rpcStock[i].size:nodes[i-StockCount].size; }
static const uint8_t *data(unsigned i) { return i<StockCount?rpcStock[i].data:nodes[i-StockCount].data; }
static int lookup(const char *p) {
    for(unsigned i=0;i<MutableCount;++i)if(nodes[i].used && equal(nodes[i].path,p))return nodes[i].removed?-1:int(StockCount+i);
    for(unsigned i=0;i<StockCount;++i)if(equal(rpcStock[i].path,p))return int(i);
    return -1;
}
static bool virtualParent(const char *p) {
    return equal(p,"/") || equal(p,"/vendor") || equal(p,"/vendor/etc") || equal(p,"/vendor/etc/sensors") ||
        equal(p,"/mnt") || equal(p,"/mnt/vendor") || equal(p,"/mnt/vendor/persist");
}
static bool parentExists(const char *p) {
    char parent[256];copy(parent,p,length(p)+1);unsigned n=length(parent);while(n && parent[n-1]!='/')--n;
    if(n>1)parent[n-1]=0;else parent[1]=0;
    int i=lookup(parent);return virtualParent(parent) || (i>=0 && isDir(mode(i)));
}
static int mutableNode(const char *p,int original,bool truncate) {
    if(!writable(p))return -13;
    if(original>=int(StockCount)) { if(truncate)nodes[original-StockCount].size=0;return original; }
    if(original>=0 && size(original)>FileBytes)return -28;
    for(unsigned i=0;i<MutableCount;++i)if(!nodes[i].used || (nodes[i].removed && equal(nodes[i].path,p))) {
        auto &n=nodes[i];n.used=true;n.removed=false;copy(n.path,p,length(p)+1);
        n.mode=original>=0?mode(original):0100600;n.size=original>=0&&!truncate?size(original):0;
        if(n.size)copy(n.data,data(original),n.size);return StockCount+i;
    }
    return -28;
}
static Handle *handle(Session &s,uint64_t token) {
    if(token>0xffffffffU)return nullptr;
    for(auto &h:s.handles)if(h.used && h.token==token)return &h;return nullptr;
}
static int open(Session &s,const char *p,const char *m,bool directory,uint32_t *token) {
    bool rd=false,wr=false,append=false,trunc=false;
    if(!directory) {
        if(!*m || (*m!='r' && *m!='w' && *m!='a'))return 22;
        rd=*m=='r';wr=*m!='r';trunc=*m=='w';append=*m=='a';
        bool binary=false,plus=false;for(unsigned i=1;m[i];++i) {
            if(m[i]=='b' && !binary)binary=true;else if(m[i]=='+' && !plus)plus=true;else return 22;
        }if(plus)rd=wr=true;
    }
    int i=lookup(p);if(i<0 && (directory || *m=='r'))return 2;
    if(i>=0 && isDir(mode(i))!=directory)return directory?20:21;
    if(!directory && wr && !writable(p))return 13;
    if(i<0 && !parentExists(p))return 2;
    Handle *free=nullptr;for(auto &h:s.handles)if(!h.used) { free=&h;break; }if(!free)return 24;
    if(wr) { i=mutableNode(p,i,trunc);if(i<0)return -i; }
    // Stock node identity remains stable across a later copy-on-write by another
    // open. Resolve its current path for each operation, never silently stale.
    if(s.sequence>=0x00ffffff)return 24;
    *free={};free->used=true;free->node=i;free->directory=directory;free->read=rd;free->write=wr;free->append=append;
    free->pos=append?size(i):0;free->token=(++s.sequence<<8)|(unsigned(free-s.handles)+1)|(unsigned(&s-sessions)<<7);
    *token=free->token;return 0;
}
static int current(Handle &h) { int i=lookup(path(h.node));if(i>=0)h.node=i;return i; }
static int readFile(Handle &h,uint8_t *out,unsigned bytes,bool line,unsigned *done) {
    if(h.directory || !h.read)return 9;int i=current(h);if(i<0)return 2;
    unsigned limit=size(i),n=0,room=line?(bytes?bytes-1:0):bytes;
    if(line && !bytes)return 22;
    while(n<room && h.pos<limit) { uint8_t c=data(i)[h.pos++];out[n++]=c;if(line && c=='\n')break; }
    if(line)out[n]=0;
    if(n<room && h.pos>=limit)h.eof=true;
    *done=n;return 0;
}
static const char *searchPath="/vendor/lib/rfsa/adsp;/vendor/dsp;/vendor/dsp/adsp;/dsp";
static int resolve(const char *name,char *out) {
    if(*name=='/')return canonical(name,out)?0:22;
    // Relative library lookup uses the same bounded, stock-backed namespace.
    const char *dirs[]={"/vendor/lib/rfsa/adsp/","/vendor/dsp/","/vendor/dsp/adsp/","/dsp/"};
    for(auto d:dirs) { unsigned n=length(d),l=length(name);if(n+l>=256)return 36;copy(out,d,n);copy(out+n,name,l+1);
        char normalized[256];if(!canonical(out,normalized))return 22;if(lookup(out)>=0)return 0; }
    return 2;
}
}
void picasso_rpc_fs_reset() { for(auto &n:nodes)n.used=false;for(auto &s:sessions)s={}; }
// Stock socinfo_v0_6 prefix, item 137 in the v12 global partition.
// Keep a stable bounded snapshot; no dummy platform values on parse failures.
int picasso_rpc_fs_socinfo(const wlan_smem_io *io,uint32_t *values) {
    struct wlan_smem_partition part={};wlan_smem_item item={};uint8_t raw[68],again[68];
    int error=wlan_smem_partition(io,0xfffe,0xfffe,&part);if(error)return error;
    error=wlan_smem_find(io,&part,137,&item);if(error)return error;
    if(item.size<68 || wlan_smread(io,item.offset,raw,68) || wlan_smread(io,item.offset,again,68))return -22;
    for(unsigned i=0;i<68;++i)if(raw[i]!=again[i])return -11;
    unsigned format=rpc32(raw),id=rpc32(raw+4),revision=rpc32(raw+8),platform=rpc32(raw+52),version=rpc32(raw+56),sub=rpc32(raw+64);
    if((format>>16) || format<6 || format>16 || !id)return -95;
    const char *platformName=nullptr;
    switch(platform) {
        case 1:platformName="Surf";break;case 8:platformName="MTP";break;case 10:platformName="Dragon";break;
        case 11:platformName="QRD";break;case 31:platformName="HDK";break;case 36:platformName="PICASSO";break;
        default:return -95;
    }
    const char *normal[]={"Unknown","charm","strange","strange_2a","Invalid"};
    const char *qrd[]={"QRD","SKUAA","SKUF","SKUAB",nullptr,"SKUG","INVALID"};
    const char *subName=platform==11?qrd[sub<6?sub:6]:normal[sub<4?sub:4];if(!subName)return -95;
    const char *keys[]={"hw_platform","platform_subtype","platform_subtype_id","platform_version","soc_id","revision"};
    char contents[6][40]={};
    auto decimal=[](char *out,unsigned value) { char rev[10];unsigned n=0;do { rev[n++]=char('0'+value%10);value/=10; }while(value);
        for(unsigned i=0;i<n;++i)out[i]=rev[n-1-i];out[n]=0;return n; };
    copy(contents[0],platformName,length(platformName)+1);copy(contents[1],subName,length(subName)+1);
    decimal(contents[2],sub);decimal(contents[3],version);decimal(contents[4],id);
    unsigned n=decimal(contents[5],revision>>16);contents[5][n++]='.';decimal(contents[5]+n,revision&65535);
    unsigned freeCount=0;for(auto &node:nodes)freeCount+=!node.used;if(freeCount<6)return -28;
    for(unsigned i=0;i<6;++i) {
        char full[256]="/sys/devices/soc0/";unsigned at=length(full);copy(full+at,keys[i],length(keys[i])+1);
        if(lookup(full)>=0)return -17; // startup-only publication
    }
    for(unsigned i=0;i<6;++i)for(auto &node:nodes)if(!node.used) {
        node.used=true;node.removed=false;node.mode=0100444;
        const char *base="/sys/devices/soc0/";copy(node.path,base,length(base));copy(node.path+length(base),keys[i],length(keys[i])+1);
        node.size=length(contents[i]);copy(node.data,contents[i],node.size);node.data[node.size++]='\n';break;
    }
    values[0]=format;values[1]=id;values[2]=revision;values[3]=platform;values[4]=version;values[5]=sub;return 0;
}
int picasso_rpc_fs_invoke(unsigned pd,uint32_t sc,PicassoRpcArg *a,char *name) {
    if(pd!=0 && pd!=2)return 22;
    Session &s=sessions[pd==2];char p[256],raw[256],modeText[16];
    unsigned method=(sc>>24)&31;unsigned ni=(sc>>16)&255,no=(sc>>8)&255;
    auto shape=[&](unsigned in,unsigned out,unsigned prim,unsigned output) {
        return !(sc&255) && ni==in && no==out && (!prim || (a[0].data && a[0].bytes>=prim)) &&
            (!output || (a[in].data && a[in].bytes>=output)); };
    auto getPath=[&](unsigned arg,unsigned len) { if(!string(a[arg],len,raw))return false;record(name,raw);return canonical(raw,p); };
    if(method==0 || method==19) {
        bool env=method==19;if(!shape(env?5:3,1,env?16:8,4))return 14;
        unsigned first=env?3:1,off=env?8:0;
        if(env) { char key[128],delim[16];if(!string(a[1],rpc32(a[0].data),key,128) ||
            !string(a[2],rpc32(a[0].data+4),delim,16))return 14;
            record(name,key);
            if(!equal(key,"ADSP_LIBRARY_PATH") && !equal(key,"DSP_LIBRARY_PATH"))return 99;
            if(!equal(delim,";"))return 22; }
        if(!string(a[first],rpc32(a[0].data+off),raw) || !string(a[first+1],rpc32(a[0].data+off+4),modeText,16))return 14;
        record(name,raw);int err=resolve(raw,p);if(err)return err;
        uint32_t token=0;err=open(s,p,modeText,false,&token);if(!err)rpcPut32(a[ni].data,token);return err;
    }
    if(method==16) {
        if(!shape(2,2,8,4) || !string(a[1],rpc32(a[0].data),raw))return 14;record(name,raw);
        if(!equal(raw,"ADSP_LIBRARY_PATH") && !equal(raw,"DSP_LIBRARY_PATH"))return 99;
        unsigned bytes=rpc32(a[0].data+4),needed=length(searchPath)+1;if(bytes>a[3].bytes)return 14;
        rpcPut32(a[2].data,needed);if(bytes<needed)return 77;copy(a[3].data,searchPath,needed);return 0;
    }
    if(method==31 && ni==3 && no==0) { // extension 33: frename
        char dest[256];if(!shape(3,0,12,0) || rpc32(a[0].data)!=33 || !getPath(1,rpc32(a[0].data+4)) ||
            !string(a[2],rpc32(a[0].data+8),raw) || !canonical(raw,dest))return 14;
        int i=lookup(p);if(i<0)return 2;if(!writable(p) || !writable(dest))return 13;
        if(isDir(mode(i)))return 21;if(equal(p,dest))return 0;if(!parentExists(dest))return 2;
        // This bounded store refuses rename of open files and replacement of
        // existing entries; it must not lose descriptors or silently truncate.
        if(lookup(dest)>=0)return 17;
        for(auto &session:sessions)for(auto &h:session.handles)if(h.used && equal(path(h.node),p))return 16;
        int source=mutableNode(p,i,false);if(source<0)return -source;
        int target=mutableNode(dest,-1,true);if(target<0)return -target;
        auto &from=nodes[source-StockCount];auto &to=nodes[target-StockCount];
        to.mode=from.mode;to.size=from.size;copy(to.data,from.data,from.size);from.removed=true;return 0;
    }
    if(method==31 && ni==1 && no==0) { // extension 32: ftrunc
        if(!shape(1,0,12,0) || rpc32(a[0].data)!=32)return 14;
        auto *h=handle(s,rpc32(a[0].data+4));if(!h || !h->write || h->directory)return 9;
        int i=current(*h);if(i<int(StockCount))return 9;record(name,path(i));
        unsigned bytes=rpc32(a[0].data+8);if(bytes>FileBytes)return 28;
        auto &n=nodes[i-StockCount];if(bytes>n.size)rpcZero(n.data+n.size,bytes-n.size);n.size=bytes;return 0;
    }
    if(method==31) {
        if(!shape(2,1,8,96) || rpc32(a[0].data)!=31 || !getPath(1,rpc32(a[0].data+4)))return 14;
        int i=lookup(p);if(i<0)return 2;auto *b=a[2].data;rpcZero(b,96);rpcPut64(b,96);
        rpcPut64(b+16,i+1);rpcPut32(b+24,mode(i));rpcPut32(b+28,1);rpcPut64(b+40,size(i));return 0;
    }
    if(method==22) {
        if(!shape(2,1,4,1) || !getPath(1,rpc32(a[0].data)))return 14;
        a[2].data[0]=lookup(p)>=0;return 0;
    }
    if(method==26) {
        if(!shape(2,1,4,8) || !getPath(1,rpc32(a[0].data)))return 14;
        uint32_t token=0;int err=open(s,p,"",true,&token);if(!err)rpcPut64(a[2].data,token);return err;
    }
    if(method==29) {
        if(!shape(2,0,8,0) || !getPath(1,rpc32(a[0].data)))return 14;
        if(lookup(p)>=0)return 17;if(!writable(p))return 13;if(!parentExists(p))return 2;
        int i=mutableNode(p,-1,true);if(i<0)return -i;nodes[i-StockCount].mode=0040000|(rpc32(a[0].data+4)&0777);return 0;
    }
    if(method==24 || method==30) {
        if(!shape(2,0,4,0) || !getPath(1,rpc32(a[0].data)))return 14;
        int i=lookup(p);if(i<0)return 2;if(!writable(p))return 13;
        if(isDir(mode(i))!=(method==30))return method==30?20:21;
        for(auto &session:sessions)for(auto &h:session.handles)if(h.used && equal(path(h.node),p))return 16;
        for(unsigned j=0;j<StockCount+MutableCount;++j) {
            if(j>=StockCount && !nodes[j-StockCount].used)continue;
            if(lookup(path(j))!=int(j))continue;
            if(prefix(path(j),p) && path(j)[length(p)]=='/')return 39;
        }
        i=mutableNode(p,i,false);if(i<0)return -i;nodes[i-StockCount].removed=true;return 0;
    }
    if(method==27 || method==28) {
        if(!shape(1,method==28?1:0,8,method==28?264:0))return 14;
        auto *h=handle(s,rpc64(a[0].data));if(!h || !h->directory)return 9;
        record(name,path(h->node));if(method==27) { h->used=false;return 0; }
        auto *b=a[1].data;rpcZero(b,264);const char *parent=path(h->node);unsigned len=length(parent);
        while(h->pos<StockCount+MutableCount) { unsigned i=h->pos++;
            if(i>=StockCount && !nodes[i-StockCount].used)continue;
            const char *child=path(i);if(lookup(child)!=int(i) || !prefix(child,parent) || child[len]!='/')continue;
            const char *base=child+len+1;bool nested=false;for(unsigned j=0;base[j];++j)if(base[j]=='/')nested=true;
            if(nested)continue;rpcPut32(b,i+1);copy(b+4,base,length(base)+1);return 0;
        }rpcPut32(b+260,1);return 0;
    }
    if(method==2 || method==3 || method==4 || method==5 || method==6 || method==7 || method==8 || method==9 || method==10 ||
        method==11 || method==12 || method==13 || method==14 || method==20 || method==23) {
        unsigned in=(method==5 || method==7)?2:1,out=(method==4 || method==6 || method==20)?2:(method==5 || method==8 || method==10 || method==12 || method==13)?1:0;
        unsigned prim=(method==4 || method==5 || method==6 || method==7 || method==20)?8:method==9?12:4;
        unsigned os=(method==4 || method==5 || method==10)?8:out?4:0;
        if(!shape(in,out,prim,os))return 14;auto *h=handle(s,rpc32(a[0].data));if(!h || h->directory)return 9;
        int i=current(*h);if(i<0)return 2;record(name,path(i));
        if(method==3) { h->used=false;return 0; }
        if(method==23)return 95; // RAM is not persistent storage
        if(method==2)return 0; // writes below are committed directly to RAM
        if(method==11) { h->pos=0;h->eof=false;h->error=0;return 0; }
        if(method==14) { h->eof=false;h->error=0;return 0; }
        if(method==8 || method==12 || method==13) { rpcPut32(a[1].data,method==8?h->pos:method==12?h->eof:h->error);return 0; }
        if(method==10) { rpcPut64(a[1].data,size(i));return 0; }
        if(method==9) { unsigned whence=rpc32(a[0].data+8);if(whence>2)return 22;
            int64_t pos=int32_t(rpc32(a[0].data+4));pos+=whence==1?h->pos:whence==2?size(i):0;
            if(pos<0 || pos>0x7fffffff)return 22;h->pos=uint32_t(pos);h->eof=false;return 0; }
        if(method==6 || method==7) {
            unsigned bytes=rpc32(a[0].data+4);
            if(method==6) { if(bytes>a[2].bytes)return 14;rpcPut32(a[1].data,8);if(bytes<8)return 77;
                rpcPut64(a[2].data,h->pos);return 0; }
            if(bytes!=8 || bytes>a[1].bytes)return 14;uint64_t pos=rpc64(a[1].data);
            if(pos>0x7fffffff)return 22;h->pos=uint32_t(pos);h->eof=false;return 0;
        }
        unsigned bytes=rpc32(a[0].data+4),done=0;
        if(method==4 || method==20) {
            if(bytes>a[2].bytes)return 14;int err=readFile(*h,a[2].data,bytes,method==20,&done);
            if(err)h->error=err;else { rpcPut32(a[1].data,method==20?(!done && h->eof):done);if(method==4)rpcPut32(a[1].data+4,h->eof); }return err;
        }
        if(bytes>a[1].bytes)return 14;if(!h->write)return 9;
        auto &n=nodes[i-StockCount];if(h->append)h->pos=n.size;
        if(h->pos>FileBytes || bytes>FileBytes-h->pos) { h->error=28;return 28; }
        if(h->pos>n.size)rpcZero(n.data+n.size,h->pos-n.size);
        copy(n.data+h->pos,a[1].data,bytes);h->pos+=bytes;if(h->pos>n.size)n.size=h->pos;
        rpcPut32(a[2].data,bytes);rpcPut32(a[2].data+4,0);return 0;
    }
    return 20;
}
