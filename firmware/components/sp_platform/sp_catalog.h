#ifndef SP_CATALOG_H
#define SP_CATALOG_H
#include "sp_internal.h"
#include "sp_api.h"
#define SP_PLANT_MAX 20u
bool sp_catalog_load(sp_identity_t *, sp_api_t *);
bool sp_catalog_save(const uint8_t device[6], const sp_plant_meta_t *);
bool sp_catalog_claim(sp_api_node_t *, const uint8_t plant[16]);
bool sp_catalog_archive(const uint8_t device[6]);
bool sp_catalog_contains(const uint8_t plant[16]);
bool sp_catalog_handle(sp_api_t *, const sp_api_request_t *);
#endif
