/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <infiltratr/filesystem_module_v2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/random.h>
/* The fault harness records original and unflushed sectors separately.
 * Restoring only the latter simulates a loss of volatile writes, rather
 * than simply reporting an I/O error while leaving all bytes durable. */
enum { HFS_UNDO_MAX=256u };
typedef struct {uint64_t at;uint8_t data[1024];} UndoSector;
typedef struct {
    uint32_t events,fail_at,original_count,pending_count;
    UndoSector original[HFS_UNDO_MAX],pending[HFS_UNDO_MAX];
} FaultTrace;
typedef struct {int fd;uint32_t sector;uint64_t bytes;unsigned writes;
    FaultTrace *trace;
    const IfsFsVolumeOpsV2 *ops;
    const IfsFsHostRuntimeV2 *writer;
    int attempted,format_result;
} Media;
static int rd(void *ctx,uint64_t at,void *p,uint32_t n)
{
    Media *m=ctx;
    if(m->writer && !m->attempted) {
        m->attempted=1;
        m->format_result=m->ops->format(m->writer,1u,"Reentrant",0u);
    }
    if(at%m->sector || n%m->sector || n>m->sector || at>m->bytes || n>m->bytes-at)return -1;
    return pread(m->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int note_sector(Media *m,UndoSector *undo,uint32_t *count,uint64_t at)
{
    for(uint32_t i=0;i<*count;++i)if(undo[i].at==at)return 0;
    if(*count>=HFS_UNDO_MAX || m->sector>sizeof(undo[0].data))return -1;
    UndoSector *u=&undo[*count];
    u->at=at;
    if(pread(m->fd,u->data,m->sector,(off_t)at)!=(ssize_t)m->sector)return -1;
    ++*count;return 0;
}
static int restore_sectors(Media *m,UndoSector *undo,uint32_t *count)
{
    for(uint32_t i=0;i<*count;++i)
        if(pwrite(m->fd,undo[i].data,m->sector,(off_t)undo[i].at)!=(ssize_t)m->sector)
            return -1;
    *count=0u;return fsync(m->fd);
}
static int wr(void *ctx,uint64_t at,const void *p,uint32_t n)
{
    Media *m=ctx;
    if(at%m->sector || n%m->sector || n>m->sector || at>m->bytes || n>m->bytes-at)return -1;
    if(m->trace) {
        FaultTrace *f=m->trace;
        if(++f->events==f->fail_at)return -1;
        if(n!=m->sector || note_sector(m,f->original,&f->original_count,at) ||
           note_sector(m,f->pending,&f->pending_count,at))return -1;
    }
    ++m->writes;
    return pwrite(m->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int flush(void *ctx)
{
    Media *m=ctx;
    if(m->trace && ++m->trace->events==m->trace->fail_at)return -1;
    int rc=fsync(m->fd);
    if(!rc && m->trace)m->trace->pending_count=0u;
    return rc;
}
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
static int entropy(void *ctx,void *p,uint32_t n)
{(void)ctx;return getrandom(p,n,0)==(ssize_t)n?0:-1;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"HFS format line %d: %s\n",__LINE__,#x);goto done;}}while(0)
static int check_relocation_faults(Media *m,const IfsFsHostRuntimeV2 *host,
    const IfsFsCoreOpsV2 *core,const IfsFsDefragOpsV2 *map,
    uint64_t free_bytes,const unsigned char payload[8192])
{
    FaultTrace *f=calloc(1,sizeof *f);
    if(!f)return -1;
    int result=-1;
    void *mount=NULL,*plan=NULL;
    m->trace=f;
    for(uint32_t policy=0u;policy<=10u;policy+=10u) {
        uint32_t events=0u;
        for(uint32_t trial=0u;trial<=events;++trial) {
            f->events=0u;f->fail_at=trial?trial:UINT32_MAX;
            IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
            uint64_t moved=0u,repaired=0u;
            CHECK(core->mount(host,&mount,&info)==IFS_FS_OK);
            CHECK(map->plan(mount,"/F000000.BIN",policy,&plan)==IFS_FS_OK && plan);
            int rc=map->execute(mount,plan,&moved);
            if(!trial) {
                CHECK(rc==IFS_FS_OK && moved==16384u);
                CHECK(map->verify(mount,plan)==IFS_FS_OK);
                events=f->events;
                CHECK(events && events<512u);
            }else CHECK(rc!=IFS_FS_OK);
            map->release_plan(mount,plan);plan=NULL;
            CHECK(core->unmount(mount)==IFS_FS_OK);mount=NULL;
            CHECK(!restore_sectors(m,f->pending,&f->pending_count));
            f->fail_at=UINT32_MAX;
            if(trial) {
                CHECK(core->mount(host,&mount,&info)==IFS_FS_OK &&
                      info.free_bytes==free_bytes);
                CHECK(map->recover(mount,0u,&repaired)==IFS_FS_OK);
                CHECK(map->recover(mount,0u,&repaired)==IFS_FS_OK && !repaired);
                for(unsigned file=0;file<16u;++file) {
                    char path[24];unsigned char readback[8192];
                    snprintf(path,sizeof path,"/F%06u.BIN",file);
                    for(unsigned phase=0;phase<2u;++phase)
                        CHECK(core->read(mount,path,phase*sizeof readback,
                              readback,sizeof readback)==IFS_FS_OK &&
                              !memcmp(readback,payload,sizeof readback));
                }
                CHECK(core->unmount(mount)==IFS_FS_OK);mount=NULL;
            }
            CHECK(!restore_sectors(m,f->original,&f->original_count));
            f->pending_count=0u;
        }
    }
    puts("Classic HFS relocation write/flush interruption recovery PASS");
    result=0;
done:
    if(plan && mount)map->release_plan(mount,plan);
    if(mount)core->unmount(mount);
    m->trace=NULL;free(f);return result;
}
int main(int argc,char **argv)
{
    if(argc!=5)return 2;
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib){fprintf(stderr,"%s\n",dlerror());return 2;}
    IfsFsModuleApiEntryV2 get=NULL;
    void *symbol=dlsym(lib,IFS_FS_COMMON_EXPORT_V2);
    if(!symbol || sizeof(get)!=sizeof(symbol))return 2;
    memcpy(&get,&symbol,sizeof(get));
    const IfsFsModuleApiV2 *api=get();
    if(!ifs_fs_api_v2_valid(api,"hfs"))return 2;
    const IfsFsCoreOpsV2 *core=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(!core || !vol || !vol->format)return 2;
    Media m={.sector=(uint32_t)strtoul(argv[4],NULL,10),.bytes=128ull*1024u*1024u};
    uint32_t variant=(uint32_t)strtoul(argv[3],NULL,10);
    if(variant!=1u)return 2;
    m.fd=open(argv[2],O_CREAT|O_EXCL|O_NOFOLLOW|O_RDWR,0600);
    if(m.fd<0)return 2;
    int result=1;
    CHECK(ftruncate(m.fd,(off_t)m.bytes)==0);
    IfsFsHostRuntimeV2 h={
        .abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(h),
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
        .volume_bytes=m.bytes,.logical_sector_bytes=m.sector,
        .physical_sector_bytes=m.sector,.maximum_transfer_bytes=m.sector,
        .context=&m,.read_at=rd,.write_at=wr,.flush=flush,
        .allocate=allocate,.release=release,.random_bytes=entropy
    },ro=h;
    ro.capabilities=IFS_FS_HOST_READ_ONLY;ro.write_at=NULL;ro.flush=NULL;
    CHECK(vol->format(&ro,variant,"NativeHFS",0u)==IFS_FS_ACCESS);
    CHECK(vol->format(&h,3u,"NativeHFS",0u)==IFS_FS_UNSUPPORTED);
    CHECK(vol->format(&h,variant,"Invalid:Name",0u)==IFS_FS_INVALID);
    CHECK(!m.writes);
    CHECK(vol->format(&h,variant,"NativeHFS",0u)==IFS_FS_OK);
    uint32_t detected=0;void *mount=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    char name[128];
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,.name_capacity=sizeof(name)};
    CHECK(core->probe(&ro,&detected)==IFS_FS_OK && detected==variant);
    const unsigned before_mount=m.writes;
    m.ops=vol;m.writer=&h;
    CHECK(core->mount(&ro,&mount,&info)==IFS_FS_OK && mount);
    m.writer=NULL;
    CHECK(m.attempted && m.format_result==IFS_FS_BUSY && m.writes==before_mount);
    CHECK(info.variant==variant && info.total_bytes==m.bytes-20480u &&
          info.free_bytes==info.total_bytes-2u*4096u);
    char label[28],tiny[2]={'X','X'};
    uint32_t label_bytes=UINT32_MAX;
    CHECK(vol->label && !vol->set_label &&
          vol->label(mount,label,sizeof(label),&label_bytes)==IFS_FS_OK &&
          label_bytes==9u && !strcmp(label,"NativeHFS"));
    CHECK(vol->label(mount,tiny,sizeof(tiny),&label_bytes)==IFS_FS_NO_SPACE &&
          !label_bytes && tiny[0]=='X' && tiny[1]=='X');
    CHECK(core->stat(mount,"/",&entry)==IFS_FS_OK && entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->directory_entry(mount,"/",0u,&entry)==IFS_FS_END);
    CHECK(vol->format(&h,variant,"NativeHFS",0u)==IFS_FS_BUSY);
    CHECK(core->unmount(mount)==IFS_FS_OK);
    CHECK(core->mount(&h,&mount,&info)==IFS_FS_OK && mount);
    CHECK(core->create && core->write);
    unsigned char payload[8192],readback[8192];
    for(unsigned i=0;i<sizeof(payload);++i)payload[i]=(unsigned char)(i*31u+7u);
    for(unsigned file=0;file<16u;++file) {
        char path[24];snprintf(path,sizeof(path),"/F%06u.BIN",file);
        const int created=core->create(mount,path,IFS_FS_ENTRY_FILE);
        if(created!=IFS_FS_OK)fprintf(stderr,"HFS CORE.CREATE %s returned %d\n",path,created);
        CHECK(created==IFS_FS_OK);
        const int inspected=core->stat(mount,path,&entry);
        if(inspected!=IFS_FS_OK)
            fprintf(stderr,"HFS CORE.STAT %s returned %d after creation\n",path,inspected);
        CHECK(inspected==IFS_FS_OK && entry.size_bytes==0u);
        /* Cross-leaf traversal must preserve every older record, as well as
         * the newly inserted record, when the catalogue grows an index. */
        for(unsigned prior=0;prior<=file;++prior) {
            char expected_name[16];
            snprintf(expected_name,sizeof(expected_name),"F%06u.BIN",prior);
            CHECK(core->directory_entry(mount,"/",prior,&entry)==IFS_FS_OK);
            CHECK(!strcmp(entry.name,expected_name) && entry.size_bytes==0u);
        }
        CHECK(core->directory_entry(mount,"/",file+1u,&entry)==IFS_FS_END);
    }
    CHECK(core->create(mount,"/F000000.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_EXISTS);
    for(unsigned phase=0;phase<2u;++phase)
        for(unsigned file=0;file<16u;++file) {
            char path[24];snprintf(path,sizeof(path),"/F%06u.BIN",file);
            const int written=core->write(mount,path,phase*sizeof(payload),payload,sizeof(payload));
            if(written!=IFS_FS_OK)fprintf(stderr,"HFS CORE.WRITE %s phase %u returned %d\n",path,phase,written);
            CHECK(written==IFS_FS_OK);
        }
    CHECK(core->unmount(mount)==IFS_FS_OK);
    CHECK(core->mount(&ro,&mount,&info)==IFS_FS_OK && mount);
    CHECK(core->create(mount,"/REJECT.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_ACCESS);
    const IfsFsDefragOpsV2 *map=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    CHECK(map && map->map && map->header.capabilities==IFS_FS_DEFRAG_ALL &&
          map->plan && map->execute && map->verify && map->recover && map->release_plan);
    for(unsigned file=0;file<16u;++file) {
        char path[24];snprintf(path,sizeof(path),"/F%06u.BIN",file);
        CHECK(core->stat(mount,path,&entry)==IFS_FS_OK && entry.size_bytes==2u*sizeof(payload));
        CHECK(core->read(mount,path,0u,readback,sizeof(readback))==IFS_FS_OK &&
              !memcmp(payload,readback,sizeof(payload)));
        CHECK(core->read(mount,path,sizeof(payload),readback,sizeof(readback))==IFS_FS_OK &&
              !memcmp(payload,readback,sizeof(payload)));
        IfsFsExtentV2 extents[3]={{0}};uint32_t count=0u;
        CHECK(map->map(mount,path,0u,extents,3u,&count)==IFS_FS_OK && count==2u);
        CHECK(extents[0].length==sizeof(payload) && extents[1].length==sizeof(payload) &&
              extents[1].physical_offset>extents[0].physical_offset+extents[0].length);
        CHECK(map->map(mount,path,2u,extents,3u,&count)==IFS_FS_END && count==0u);
    }
    CHECK(core->unmount(mount)==IFS_FS_OK);
    if(m.sector<=1024u) {
        const uint64_t expected_free=info.free_bytes;
        CHECK(check_relocation_faults(&m,&h,core,map,expected_free,payload)==0);
        for(uint32_t policy=0u;policy<=10u;policy+=10u) {
            CHECK(core->mount(&h,&mount,&info)==IFS_FS_OK);
            void *plan=NULL;uint64_t moved=0u,repaired=UINT64_MAX;
            CHECK(map->plan(mount,"/",policy,&plan)==IFS_FS_OK && plan);
            CHECK(map->execute(mount,plan,&moved)==IFS_FS_OK && moved);
            CHECK(map->verify(mount,plan)==IFS_FS_OK);
            map->release_plan(mount,plan);
            CHECK(map->recover(mount,0u,&repaired)==IFS_FS_OK && repaired==0u);
            CHECK(core->unmount(mount)==IFS_FS_OK);
            CHECK(core->mount(&ro,&mount,&info)==IFS_FS_OK && info.free_bytes==expected_free);
            const IfsFsAllocationOpsV2 *allocation=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
            CHECK(allocation && allocation->map);
            uint64_t cursor=0u,unknown=0u,free_bytes=0u;
            for(unsigned page=0u;;++page){
                IfsFsAllocationRangeV2 ranges[8];uint32_t count=0u;uint64_t next=0u;
                CHECK(page<65536u);
                int status=allocation->map(mount,cursor,ranges,8u,&count);
                CHECK(ifs_fs_allocation_page_v2_valid(h.volume_bytes,cursor,status,ranges,8u,count,&next,&unknown));
                for(uint32_t k=0u;k<count;++k)
                    if(ranges[k].state==IFS_FS_SPACE_FREE)free_bytes+=ranges[k].length;
                cursor=next;
                if(status==IFS_FS_END)break;
            }
            CHECK(cursor==h.volume_bytes && !unknown && free_bytes==expected_free);

            for(unsigned file=0;file<16u;++file) {
                char path[24];snprintf(path,sizeof(path),"/F%06u.BIN",file);
                CHECK(core->read(mount,path,0u,readback,sizeof(readback))==IFS_FS_OK &&
                      !memcmp(payload,readback,sizeof(payload)));
                CHECK(core->read(mount,path,sizeof(payload),readback,sizeof(readback))==IFS_FS_OK &&
                      !memcmp(payload,readback,sizeof(payload)));
                IfsFsExtentV2 ext[3]={{0}};uint32_t count=0;
                CHECK(map->map(mount,path,0u,ext,3u,&count)==IFS_FS_OK && count==1u &&
                      ext[0].length==2u*sizeof(payload));
            }
            CHECK(core->unmount(mount)==IFS_FS_OK);
        }
    }
    unsigned char primary[512],alternate[512];
    CHECK(pread(m.fd,primary,sizeof(primary),1024)==(ssize_t)sizeof(primary));
    CHECK(pread(m.fd,alternate,sizeof(alternate),(off_t)(m.bytes-1024u))==(ssize_t)sizeof(alternate));
    CHECK(!memcmp(primary,alternate,sizeof(primary)));
    puts("Classic HFS native format, allocation, catalogue and alternate MDB PASS");
    result=0;
done:
    close(m.fd);dlclose(lib);
    if(result)unlink(argv[2]);
    return result;
}
