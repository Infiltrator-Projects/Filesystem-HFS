/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent HFS-generated media through the production Common V2 ABI.
 * The image opens O_RDONLY; its SHA-256 is compared by the runner.
 */
#define _GNU_SOURCE
#include <infiltratr/filesystem_module_v2.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct { int fd; uint64_t bytes; } Device;
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
#define VERIFY(e) do {if(!(e)){ \
 fprintf(stderr,"HFS real-media Common V2 failure line %d: %s\n",__LINE__,#e); \
 return 1;}}while(0)
static int read_at(void *ctx,uint64_t offset,void *dst,uint32_t n)
{
    Device *d=ctx;
    if (!n || (offset%512u)||(n%512u)||n>4096u ||
        offset>d->bytes || n>d->bytes-offset) return -1;
    return pread(d->fd,dst,n,(off_t)offset)==(ssize_t)n?0:-1;
}
static void *allocate(void *ctx,size_t count)
{(void)ctx;return malloc(count);}
static void release(void *ctx,void *data)
{(void)ctx;free(data);}
static int compare_file(const IfsFsCoreOpsV2 *core,void *volume,
                        const char *hfs_path,const char *source_path)
{
    int fd=open(source_path,O_RDONLY|O_CLOEXEC);
    VERIFY(fd>=0);
    struct stat st;
    VERIFY(fstat(fd,&st)==0 && st.st_size>0);
    char name[128]={0};
    IfsFsEntryV2 info={.structure_bytes=sizeof(info),.name=name,
                        .name_capacity=sizeof(name)};
    VERIFY(core->stat(volume,hfs_path,&info)==IFS_FS_OK);
    VERIFY(info.type==IFS_FS_ENTRY_FILE);
    VERIFY(info.size_bytes==(uint64_t)st.st_size);
    unsigned char actual[16384],expected[16384];
    uint64_t off=0;
    while(off<(uint64_t)st.st_size) {
        uint32_t count=(uint64_t)st.st_size-off<sizeof(actual)?
                       (uint32_t)((uint64_t)st.st_size-off):
                       (uint32_t)sizeof(actual);
        VERIFY(pread(fd,expected,count,(off_t)off)==(ssize_t)count);
        VERIFY(core->read(volume,hfs_path,off,actual,count)==IFS_FS_OK);
        VERIFY(!memcmp(expected,actual,count));
        off+=count;
    }
    VERIFY(core->read(volume,hfs_path,(uint64_t)st.st_size,actual,1u)
           ==IFS_FS_INVALID);
    VERIFY(close(fd)==0);
    return 0;
}
int main(int argc,char **argv)
{
    if(argc!=4 && argc!=5)return 2;
    Device device={0};
    device.fd=open(argv[1],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    VERIFY(device.fd>=0);
    struct stat st;
    VERIFY(fstat(device.fd,&st)==0 && st.st_size>0);
    device.bytes=(uint64_t)st.st_size;
    IfsFsHostRuntimeV2 host={
       .abi_version=IFS_FS_HOST_ABI_V2,
       .structure_bytes=sizeof(IfsFsHostRuntimeV2),
       .capabilities=IFS_FS_HOST_READ_ONLY,
       .volume_bytes=device.bytes,.logical_sector_bytes=512u,
       .physical_sector_bytes=512u,.maximum_transfer_bytes=4096u,
       .context=&device,.read_at=read_at,
       .allocate=allocate,.release=release};
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    VERIFY(ifs_fs_api_v2_valid(api,"hfs"));
    const IfsFsCoreOpsV2 *core=(const void *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    VERIFY(core && core->probe && core->mount && core->unmount &&
           core->stat && core->directory_entry && core->read);
    void *volume=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    uint32_t variant=0;
    if(argc==5 && !strcmp(argv[4],"--reject")) {
        VERIFY(core->probe(&host,&variant)!=IFS_FS_OK ||
               core->mount(&host,&volume,&info)!=IFS_FS_OK);
        if(volume)VERIFY(core->unmount(volume)==IFS_FS_OK);
        VERIFY(close(device.fd)==0);
        return 0;
    }
    if(argc==5 && !strcmp(argv[4],"--corrupt-catalog")) {
        VERIFY(core->probe(&host,&variant)==IFS_FS_OK);
        VERIFY(core->mount(&host,&volume,&info)==IFS_FS_OK && volume);
        char filename[128]={0};
        IfsFsEntryV2 item={.structure_bytes=sizeof(item),.name=filename,
                            .name_capacity=sizeof(filename)};
        VERIFY(core->directory_entry(volume,"/",0u,&item)==IFS_FS_CORRUPT);
        VERIFY(core->unmount(volume)==IFS_FS_OK);
        VERIFY(close(device.fd)==0);
        return 0;
    }
    if(argc==5) return 2;
    VERIFY(core->probe(&host,&variant)==IFS_FS_OK);
    VERIFY(core->mount(&host,&volume,&info)==IFS_FS_OK && volume);
    char name[128]={0};
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,
                        .name_capacity=sizeof(name)};
    VERIFY(core->stat(volume,"/",&entry)==IFS_FS_OK &&
           entry.type==IFS_FS_ENTRY_DIRECTORY);
    int found_kernel=0,found_initrd=0,found_end=0;
    for(uint32_t i=0;i<1024u;++i) {
        int rc=core->directory_entry(volume,"/",i,&entry);
        if(rc==IFS_FS_END){found_end=1;break;}
        if(rc!=IFS_FS_OK) {
            fprintf(stderr,"HFS list index=%u rc=%d\n",i,rc);
            uint8_t mdb[512]={0},head[512]={0},node[512]={0};
            VERIFY(pread(device.fd,mdb,512,1024)==512);
            uint32_t physical=((uint32_t)mdb[28]<<8|mdb[29])*512u;
            uint32_t alloc=((uint32_t)mdb[20]<<24)|((uint32_t)mdb[21]<<16)|
                           ((uint32_t)mdb[22]<<8)|mdb[23];
            uint32_t first=((uint32_t)mdb[150]<<8)|mdb[151];
            uint64_t pos=physical+(uint64_t)first*alloc;
            VERIFY(pread(device.fd,head,512,(off_t)pos)==512);
            uint32_t start=((uint32_t)head[24]<<24)|((uint32_t)head[25]<<16)|
                           ((uint32_t)head[26]<<8)|head[27];
            uint32_t node_size=((uint32_t)head[32]<<8)|head[33];
            fprintf(stderr,"HFS tree catalog pos=%llu firstLeaf=%u nodeSize=%u headerKind=%u\n",
                (unsigned long long)pos,start,node_size,head[8]);
            if(node_size==512u) {
                VERIFY(pread(device.fd,node,512,(off_t)(pos+start*node_size))==512);
                unsigned count=(unsigned)node[10]*256u+node[11];
                fprintf(stderr,"leaf kind=%u count=%u prev=%u\n",node[8],count,
                    ((unsigned)node[4]<<24)|((unsigned)node[5]<<16)|
                    ((unsigned)node[6]<<8)|node[7]);
                for(unsigned k=0;k<count && k<6u;++k) {
                    unsigned begin=(unsigned)node[510u-2u*k]*256u+node[511u-2u*k];
                    unsigned finish=(unsigned)node[508u-2u*k]*256u+node[509u-2u*k];
                    fprintf(stderr,"record[%u] begin=%u finish=%u keylen=%u namelen=%u kind_guess=%u\n",
                        k,begin,finish,begin<512u?node[begin]:0,
                        begin+6u<512u?node[begin+6u]:0,
                        begin+8u<512u?node[begin+8u]:0);
                }
            }
        }
        VERIFY(rc==IFS_FS_OK);
        if(!strcmp(name,"kernel.img"))found_kernel=1;
        if(!strcmp(name,"initramfs"))found_initrd=1;
    }
    VERIFY(found_kernel && found_initrd && found_end);
    VERIFY(compare_file(core,volume,"/kernel.img",argv[2])==0);
    VERIFY(compare_file(core,volume,"/initramfs",argv[3])==0);
    VERIFY(core->stat(volume,"/not-present",&entry)==IFS_FS_NOT_FOUND);
    VERIFY(core->unmount(volume)==IFS_FS_OK);
    VERIFY(close(device.fd)==0);
    puts("HFS_INDEPENDENT_NATIVE_COMMON_V2_REAL_READS_PASS");
    return 0;
}
