// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise production snapshot write failure recovery and policy gates.

#define _GNU_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define BOOL bool
#define NO false
#define YES true
#define white_log(...) ((void)0)
static int scenario, writes;
static int mock_fcntl(int fd,int command,...) {
    (void)fd;
    if(command==63)return scenario==4?2:0;
    if(scenario==5)return 0;
    errno=scenario==6?EACCES:EPERM;return -1;
}
static ssize_t fault_write(int fd,const void *p,size_t n,off_t off) {
    if(scenario==1 && writes++==0){errno=EINTR;return -1;}
    if(scenario==2 && off>=17){errno=EIO;return -1;}
    if(n>17)n=17;return pwrite(fd,p,n,off);
}
static ssize_t fault_read(int fd,void *p,size_t n,off_t off) {
    ssize_t r=pread(fd,p,n,off);if(scenario==3&&r>0)((unsigned char*)p)[0]^=1;return r;
}
static int fault_sync(int fd){if(scenario==7){errno=EIO;return -1;}return fsync(fd);}
#define fcntl mock_fcntl
#define pwrite fault_write
#define pread fault_read
#define fsync fault_sync
#include "picasso-snapshot-store-core.h"
static bool retry(const char *p,const unsigned char *b,size_t n,long type){(void)type;return picasso_snapshot_store_write(p,b,n);}

int main(void){

    for (unsigned mask=0;mask<32;mask++) {
        bool expected=mask==1;
        assert(picasso_snapshot_store_policy(mask&1,mask&2,(mask&4)?1:0,mask&8,mask&16)==expected);
    }
    assert(!picasso_snapshot_store_policy(true,false,2,false,false));
    assert(picasso_snapshot_store_beneath("/a/Snapshots/app","/a/Snapshots/app/scene"));
    assert(!picasso_snapshot_store_beneath("/a/Snapshots/app","/a/Snapshots/application/scene"));
    assert(!picasso_snapshot_store_beneath("/a/Snapshots/app","/a/Snapshots"));
    unsigned char png[33]={137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R',0,0,4,56,0,0,9,96,8,2,0,0,0};
    assert(picasso_snapshot_store_png(png,sizeof(png)));
    assert(!picasso_snapshot_store_png(NULL,sizeof(png)));
    assert(!picasso_snapshot_store_png(png,32));
    assert(!picasso_snapshot_store_png(png,16*1024*1024+1));
    png[25]=6;assert(picasso_snapshot_store_png(png,sizeof(png)));
    png[25]=3;assert(!picasso_snapshot_store_png(png,sizeof(png)));png[25]=2;
    png[16]=1;assert(!picasso_snapshot_store_png(png,sizeof(png)));png[16]=0;
    png[0]=0;assert(!picasso_snapshot_store_png(png,sizeof(png)));

    char dir[]="/tmp/snapshot-write-check-XXXXXX";assert(mkdtemp(dir));
    char path[256],other[256];snprintf(path,sizeof(path),"%s/image.png",dir);snprintf(other,sizeof(other),"%s/other.png",dir);
    unsigned char data[12013];for(size_t i=0;i<sizeof(data);i++)data[i]=(unsigned char)(i*17+3);
    for(scenario=0;scenario<=7;scenario++){
        int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);assert(fd>=0);assert(!close(fd));writes=0;
        bool result=retry(path,data,sizeof(data),2);assert(result==(scenario<2));
        struct stat st;assert(!stat(path,&st));assert(st.st_size==(scenario<2?(off_t)sizeof(data):0));
        if(result){unsigned char out[sizeof(data)];fd=open(path,O_RDONLY);assert(read(fd,out,sizeof(out))==sizeof(out));close(fd);assert(!memcmp(out,data,sizeof(out)));}
    }
    scenario=0;int fd=open(path,O_TRUNC|O_RDWR);assert(write(fd,"keep",4)==4);close(fd);
    assert(!retry(path,data,sizeof(data),2));struct stat st;assert(!stat(path,&st)&&st.st_size==4);
    assert(!symlink(path,other));assert(!retry(other,data,sizeof(data),2));assert(!unlink(other));
    fd=open(path,O_TRUNC|O_RDWR);close(fd);assert(!link(path,other));assert(!retry(path,data,sizeof(data),2));unlink(other);
    assert(retry(path,data,sizeof(data),0));unlink(path);rmdir(dir);
    puts("PASS actual writer: short writes/EINTR, write/read/fsync failure, class gates, existing content, symlink, hardlink");
}
