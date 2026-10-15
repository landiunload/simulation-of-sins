#include "sim/links.h"

#include "sim/random.h"
#include "sim/table.h"

#include <stdlib.h>

/* One pair in its owner's list: the items before and after it, zero at either
 * end. The pair itself is the node's key, so it is not stored twice. */
typedef struct LinkNode
{
    uint64_t previous;
    uint64_t next;
} LinkNode;

/* An owner's list: the two ends and the size. Keeping both ends is what makes
 * appending at the tail, and so insertion order, constant time. */
typedef struct LinkHead
{
    uint64_t first;
    uint64_t last;
    uint32_t count;
} LinkHead;

struct SosLinks
{
    /* Keyed (owner, item) with a non-zero item. */
    SosTable *nodes;
    /* Keyed (owner, 0), which a node key can never be. */
    SosTable *heads;
};

static SosTableKey NodeKey(uint64_t owner, uint64_t item)
{
    return (SosTableKey){.first = owner, .second = item};
}

static SosTableKey HeadKey(uint64_t owner)
{
    return (SosTableKey){.first = owner, .second = 0u};
}

static LinkNode *EditNode(SosLinks *links, uint64_t owner, uint64_t item)
{
    return (LinkNode *)SosTableEdit(links->nodes, NodeKey(owner, item));
}

static LinkHead *EditHead(SosLinks *links, uint64_t owner)
{
    return (LinkHead *)SosTableEdit(links->heads, HeadKey(owner));
}

SosLinks *SosLinksCreate(void)
{
    SosLinks *links = calloc(1u, sizeof(*links));
    if (links == NULL)
    {
        return NULL;
    }
    links->nodes = SosTableCreate(sizeof(LinkNode));
    links->heads = SosTableCreate(sizeof(LinkHead));
    if (links->nodes == NULL || links->heads == NULL)
    {
        SosLinksDestroy(links);
        return NULL;
    }
    return links;
}

void SosLinksDestroy(SosLinks *links)
{
    if (links != NULL)
    {
        SosTableDestroy(links->nodes);
        SosTableDestroy(links->heads);
        free(links);
    }
}

bool SosLinksReserve(SosLinks *links, uint32_t extraOwners, uint32_t extraItems)
{
    if (links == NULL)
    {
        return false;
    }
    /* Nodes hold one pair each; heads hold one per owner. Reject a request
     * that could not be met even with every byte available before touching
     * either table, so a partial reservation is never attempted. */
    const uint32_t nodeCount = SosTableCount(links->nodes);
    const uint32_t headCount = SosTableCount(links->heads);
    if (extraItems > SOS_TABLE_MAX_COUNT - nodeCount ||
        extraOwners > SOS_TABLE_MAX_COUNT - headCount)
    {
        return false;
    }
    return SosTableReserve(links->nodes, nodeCount + extraItems) &&
           SosTableReserve(links->heads, headCount + extraOwners);
}

bool SosLinksAdd(SosLinks *links, uint64_t owner, uint64_t item)
{
    if (links == NULL || owner == 0u || item == 0u)
    {
        return false;
    }
    const SosTableKey nodeKey = NodeKey(owner, item);
    if (SosTableFind(links->nodes, nodeKey) != NULL)
    {
        return true;
    }
    const SosTableKey headKey = HeadKey(owner);
    bool createdHead = false;
    if (SosTableInsert(links->heads, headKey, &createdHead) == NULL)
    {
        return false;
    }
    bool createdNode = false;
    if (SosTableInsert(links->nodes, nodeKey, &createdNode) == NULL)
    {
        if (createdHead)
        {
            (void)SosTableRemove(links->heads, headKey);
        }
        return false;
    }
    if (!createdNode)
    {
        return true;
    }
    LinkHead *head = EditHead(links, owner);
    LinkNode *node = EditNode(links, owner, item);
    if (head == NULL || node == NULL)
    {
        /* Unreachable while the two tables stay in step, but dropping the new
         * node keeps the set consistent rather than imagining it exists. */
        (void)SosTableRemove(links->nodes, nodeKey);
        if (createdHead)
        {
            (void)SosTableRemove(links->heads, headKey);
        }
        return false;
    }
    node->previous = head->last;
    node->next = 0u;
    if (head->last != 0u)
    {
        LinkNode *tail = EditNode(links, owner, head->last);
        if (tail != NULL)
        {
            tail->next = item;
        }
    }
    else
    {
        head->first = item;
    }
    head->last = item;
    head->count += 1u;
    return true;
}

bool SosLinksRemove(SosLinks *links, uint64_t owner, uint64_t item)
{
    if (links == NULL || owner == 0u || item == 0u)
    {
        return false;
    }
    LinkNode *node = EditNode(links, owner, item);
    LinkHead *head = EditHead(links, owner);
    if (node == NULL || head == NULL)
    {
        return false;
    }
    const uint64_t previous = node->previous;
    const uint64_t next = node->next;
    if (previous != 0u)
    {
        LinkNode *before = EditNode(links, owner, previous);
        if (before != NULL)
        {
            before->next = next;
        }
    }
    else
    {
        head->first = next;
    }
    if (next != 0u)
    {
        LinkNode *after = EditNode(links, owner, next);
        if (after != NULL)
        {
            after->previous = previous;
        }
    }
    else
    {
        head->last = previous;
    }
    (void)SosTableRemove(links->nodes, NodeKey(owner, item));
    if (head->count > 0u)
    {
        head->count -= 1u;
    }
    if (head->count == 0u)
    {
        (void)SosTableRemove(links->heads, HeadKey(owner));
    }
    return true;
}

bool SosLinksHas(const SosLinks *links, uint64_t owner, uint64_t item)
{
    if (links == NULL || owner == 0u || item == 0u)
    {
        return false;
    }
    return SosTableFind(links->nodes, NodeKey(owner, item)) != NULL;
}

uint32_t SosLinksCount(const SosLinks *links, uint64_t owner)
{
    if (links == NULL || owner == 0u)
    {
        return 0u;
    }
    const LinkHead *head = (const LinkHead *)SosTableFind(links->heads, HeadKey(owner));
    return head != NULL ? head->count : 0u;
}

uint64_t SosLinksFirst(const SosLinks *links, uint64_t owner)
{
    if (links == NULL || owner == 0u)
    {
        return 0u;
    }
    const LinkHead *head = (const LinkHead *)SosTableFind(links->heads, HeadKey(owner));
    return head != NULL ? head->first : 0u;
}

uint64_t SosLinksNext(const SosLinks *links, uint64_t owner, uint64_t item)
{
    if (links == NULL || owner == 0u || item == 0u)
    {
        return 0u;
    }
    const LinkNode *node = (const LinkNode *)SosTableFind(links->nodes, NodeKey(owner, item));
    return node != NULL ? node->next : 0u;
}

uint32_t SosLinksList(const SosLinks *links, uint64_t owner, uint64_t *out, uint32_t capacity)
{
    if (links == NULL || owner == 0u || (out == NULL && capacity != 0u))
    {
        return 0u;
    }
    uint32_t count = 0u;
    for (uint64_t item = SosLinksFirst(links, owner); item != 0u;
         item = SosLinksNext(links, owner, item))
    {
        if (count < capacity)
        {
            out[count] = item;
        }
        count += 1u;
    }
    return count;
}

uint32_t SosLinksRemoveOwner(SosLinks *links, uint64_t owner)
{
    if (links == NULL || owner == 0u)
    {
        return 0u;
    }
    uint32_t removed = 0u;
    uint64_t item = SosLinksFirst(links, owner);
    while (item != 0u)
    {
        /* Removing the current item only relinks the others, so its successor
         * is still the one to visit next. */
        const uint64_t next = SosLinksNext(links, owner, item);
        if (SosLinksRemove(links, owner, item))
        {
            removed += 1u;
        }
        item = next;
    }
    return removed;
}

static uint64_t HashPair(SosTableKey key, const void *record, void *context)
{
    (void)record;
    (void)context;
    /* Only the pair, never the previous/next links or the head's ends, so the
     * order pairs were added in cannot change the fold. */
    return SosHash64(SosHashCombine(SosHash64(key.first), key.second));
}

uint64_t SosLinksHash(const SosLinks *links)
{
    return links != NULL ? SosTableFold(links->nodes, HashPair, NULL) : 0u;
}
