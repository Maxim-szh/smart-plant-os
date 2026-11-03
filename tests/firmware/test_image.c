#include "sp_image.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void valid(uint8_t *p,unsigned profile){memset(p,0,128);p[0]=0xe9;p[3]=(profile==0?2:profile==1?3:4)<<4;p[12]=profile==0?5:9;p[32]=0x32;p[33]=0x54;p[34]=0xcd;p[35]=0xab;strcpy((char *)p+48,"0.3.0-abi1");strcpy((char *)p+80,profile==0?"smart_plant_node":"smart_plant_hub");}
int main(void){uint8_t p[128];char version[32];for(unsigned profile=0;profile<3;profile++){
 valid(p,profile);assert(sp_image_header(p,profile,version)&&!strcmp(version,"0.3.0-abi1"));assert(!sp_image_header(p,(profile+1)%3,version));
 p[0]^=1;assert(!sp_image_header(p,profile,version));valid(p,profile);p[12]^=1;assert(!sp_image_header(p,profile,version));
 valid(p,profile);strcpy((char *)p+48,"0.1.0-abi1");assert(!sp_image_header(p,profile,version));strcpy((char *)p+48,"0.2.0-abi2");assert(!sp_image_header(p,profile,version));strcpy((char *)p+48,"0.3.0-abi1");assert(sp_image_header(p,profile,version));
 valid(p,profile);memset(p+48,'A',32);assert(!sp_image_header(p,profile,version));valid(p,profile);strcpy((char *)p+80,"some_other_project");assert(!sp_image_header(p,profile,version));
 }puts("PASS: image chip/flash/project/ABI/version validation and unterminated header rejection");}
