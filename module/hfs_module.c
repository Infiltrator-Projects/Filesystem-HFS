/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_hfs_module_v1.h"
#include "ifs_hfs_fixture_v1.h"
int ifs_hfs_worker_main(int,char **);
int ifs_hfs_fixture_run(const void *,const char *,int,const void *);
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
static int run(int argc,char **argv,const IfsDefragServicesV1 *s) {
 if(ifs_bind_services(s)!=0)return 1;
 const int result=ifs_hfs_worker_main(argc,argv);
 ifs_unbind_services();return result;
}
static int fixture(const void *spec,const char *path,int create,
                   const IfsDefragServicesV1 *s,const IfsHfsFixtureHostV1 *host) {
 if(ifs_bind_services(s)!=0)return -1;
 const int result=ifs_hfs_fixture_run(spec,path,create,host);
 ifs_unbind_services();return result;
}
static const IfsHfsModuleV1 api={IFS_HFS_MODULE_V1,sizeof(IfsHfsModuleV1),
 "hfs",IFS_HFS_CAP_RUN | IFS_HFS_CAP_FIXTURE,run,fixture};
const IfsHfsModuleV1 *infiltrator_filesystem_hfs_v1(void){return &api;}
