#include "sp_update_wire.h"
#include "sp_crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 uint8_t key[16]={1,2,3},data[192],wire[248],plain[192];for(unsigned i=0;i<192;i++)data[i]=(uint8_t)i;
 sp_update_header_t h={.role=1,.type=SP_U_DATA,.boot=7,.sequence=9,.target_boot=4,.offset=192,.session={3,4,5}},out;
 size_t size,len;assert(sp_update_seal(key,&h,data,192,wire,&size)&&size==248);
 assert(sp_update_open(key,wire,size,&out,plain,&len)&&len==192&&!memcmp(data,plain,192));assert(out.boot==7&&out.sequence==9&&out.offset==192&&!memcmp(out.session,h.session,16));
 for(unsigned i=0;i<size;i++){wire[i]^=1;assert(!sp_update_open(key,wire,size,&out,plain,&len));wire[i]^=1;}
 for(unsigned i=0;i<56;i++)assert(!sp_update_open(key,wire,i,&out,plain,&len));
 uint8_t wrong[16]={7};assert(!sp_update_open(wrong,wire,size,&out,plain,&len));
 assert(!sp_update_seal(key,&h,data,193,wire,&size));h.sequence=0;assert(!sp_update_seal(key,&h,data,192,wire,&size));h.sequence=9;
 sp_header_t spp={.role=1,.boot=7,.sequence=9,.target_boot=4};sp_message_t msg;sp_message_init(&msg,SP_QUERY);uint8_t regular[SP_WIRE_MAX];
 assert(sp_frame_seal(key,&spp,&msg,regular,&size));assert(!sp_update_open(key,regular,size,&out,plain,&len));
 assert(sp_update_seal(key,&h,data,192,wire,&size));assert(!sp_frame_open(key,wire,size,&spp,&msg));
 /* Role, sequence, boot, target, session, offset and payload are all authenticated. */
 puts("PASS: SPU1 full-size roundtrip, bytewise tampering, truncation, wrong key and SPP domain isolation");
}
