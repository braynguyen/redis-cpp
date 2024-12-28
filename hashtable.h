#pragma once

#include <stddef.h>
#include <stdint.h>

// hasthable node, embedded into payload
struct HNode {
    HNode *next = NULL;
    uint64_t hcode = 0; // cached hash value
};

struct HTab {
    HNode **tab = NULL; // array of 'Hnode *'
    size_t mask = 0;
    size_t size = 0;
};

struct HMap {
    HTab ht1; // newer
    HTab ht2; // older
    size_t resizing_pos = 0;
};

HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *));
void hm_insert(HMap *hmap, HNode *node);
HNode *hm_pop(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *));
size_t hm_size(HMap *hmap);
void hm_destroy(HMap *hmap);