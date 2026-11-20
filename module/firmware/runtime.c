/* SPDX-License-Identifier: GPL-3.0-or-later
 * Freestanding byte primitives for the native HFS Common V2 engine.
 * All I/O and temporary allocations use authenticated Common V2 hosts.
 */
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *restrict destination,const void *restrict source,size_t n)
{
    unsigned char *d=destination;
    const unsigned char *s=source;
    for(size_t i=0;i<n;++i)d[i]=s[i];
    return destination;
}
void *memset(void *destination,int value,size_t n)
{
    unsigned char *d=destination;
    for(size_t i=0;i<n;++i)d[i]=(unsigned char)value;
    return destination;
}
int memcmp(const void *a,const void *b,size_t n)
{
    const unsigned char *left=a,*right=b;
    for(size_t i=0;i<n;++i)
        if(left[i]!=right[i])return (int)left[i]-(int)right[i];
    return 0;
}
size_t strlen(const char *s)
{
    size_t i=0u;
    while(s[i])++i;
    return i;
}
