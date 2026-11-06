/*
 * SAB - O(1) bookkeeping of the script heap (engine 8025_webm, -DSAB).
 *
 * Stock 8025 keeps every script allocation (array(), allocscript(), ...) in
 * the list "scriptheap" under a name ("openbor_array"), so free() finds it
 * by a linear scan of the list and removes it from the list's string-hash
 * bucket, where every array shares one name: both are O(number of live
 * allocations). A script-driven mod with a few thousand live arrays spent
 * most of its frame time in free() (100+ us per call).
 *
 * With SAB the arrays are kept unnamed (no string-hash bucket) and a pointer
 * hash table maps each allocation to its list node, so free() is O(1).
 * Behaviour is unchanged: same allocations, same order in the list, same
 * shutdown dump (unnamed array nodes print as "openbor_array").
 */
#ifndef SAB_HEAP_H
#define SAB_HEAP_H

#include <stdint.h>
#include <stdlib.h>

typedef struct
{
    void *key;              /* NULL: empty, (void *)1: deleted */
    Node *node;
} sab_heap_slot;

static sab_heap_slot *sab_heap_tab = NULL;
static size_t sab_heap_cap = 0, sab_heap_used = 0, sab_heap_fill = 0;

#define SAB_HEAP_DELETED ((void *)1)

static size_t sab_heap_hash(void *p)
{
    uint64_t x = (uint64_t)(uintptr_t)p;

    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)x;
}

static void sab_heap_put_raw(void *key, Node *node)
{
    size_t i = sab_heap_hash(key) & (sab_heap_cap - 1);

    while(sab_heap_tab[i].key && sab_heap_tab[i].key != SAB_HEAP_DELETED)
    {
        i = (i + 1) & (sab_heap_cap - 1);
    }
    if(!sab_heap_tab[i].key)
    {
        sab_heap_fill++;
    }
    sab_heap_tab[i].key = key;
    sab_heap_tab[i].node = node;
    sab_heap_used++;
}

static void sab_heap_grow(void)
{
    sab_heap_slot *old = sab_heap_tab;
    size_t n = sab_heap_cap, i;

    /* sized from the live entries (tombstones are dropped by the rehash) */
    sab_heap_cap = 4096;
    while(sab_heap_used * 4 >= sab_heap_cap)
    {
        sab_heap_cap *= 2;
    }
    sab_heap_tab = calloc(sab_heap_cap, sizeof(sab_heap_slot));
    sab_heap_used = sab_heap_fill = 0;
    for(i = 0; i < n; i++)
    {
        if(old[i].key && old[i].key != SAB_HEAP_DELETED)
        {
            sab_heap_put_raw(old[i].key, old[i].node);
        }
    }
    free(old);
}

/* Records allocation `key` stored in list node `node`. */
static void sab_heap_add(void *key, Node *node)
{
    if(!key || !node)
    {
        return;
    }
    if((sab_heap_fill + 1) * 2 >= sab_heap_cap)
    {
        sab_heap_grow();
    }
    sab_heap_put_raw(key, node);
}

static Node *sab_heap_find(void *key)
{
    size_t i;

    if(!sab_heap_cap || !key)
    {
        return NULL;
    }
    i = sab_heap_hash(key) & (sab_heap_cap - 1);
    while(sab_heap_tab[i].key)
    {
        if(sab_heap_tab[i].key == key)
        {
            return sab_heap_tab[i].node;
        }
        i = (i + 1) & (sab_heap_cap - 1);
    }
    return NULL;
}

static void sab_heap_del(void *key)
{
    size_t i;

    if(!sab_heap_cap || !key)
    {
        return;
    }
    i = sab_heap_hash(key) & (sab_heap_cap - 1);
    while(sab_heap_tab[i].key)
    {
        if(sab_heap_tab[i].key == key)
        {
            sab_heap_tab[i].key = SAB_HEAP_DELETED;
            sab_heap_tab[i].node = NULL;
            sab_heap_used--;
            return;
        }
        i = (i + 1) & (sab_heap_cap - 1);
    }
}

static void sab_heap_clear(void)
{
    free(sab_heap_tab);
    sab_heap_tab = NULL;
    sab_heap_cap = sab_heap_used = sab_heap_fill = 0;
}

#endif
