/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_hfs_module_v1.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"fail %s\n",#x);return 1;} } while(0)
int main(void){
 const IfsHfsModuleV1 *api=infiltrator_filesystem_hfs_v1();
 CHECK(api && api->abi_version==1u && api->structure_bytes>=sizeof(*api));
 CHECK(api->format_id && strcmp(api->format_id,"hfs")==0);
 CHECK(api->capabilities==(IFS_HFS_CAP_RUN|IFS_HFS_CAP_FIXTURE));
 CHECK(api->run && api->fixture);
 char *argv[]={"hfs_analyser","--version",NULL};
 CHECK(api->run(2,argv,NULL)!=0);
 return 0;
}
