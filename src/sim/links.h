#ifndef SOS_SIM_LINKS_H
#define SOS_SIM_LINKS_H

#include <stdbool.h>
#include <stdint.h>

/* Ordered sets of owned items.
 *
 * Many parts of the world keep a per-owner collection: a being's partners,
 * the members of a group, the relations one being holds, the rumors a being
 * knows, the skills it has learnt. They all want the same thing: a set of
 * (owner, item) pairs, keyed like everything else by two 64-bit values, that
 * can be enumerated one owner at a time in the order the items were added,
 * without a buffer and without scanning the whole world.
 *
 * The set keeps one node per pair in an SosTable keyed (owner, item), holding
 * the previous and next item of the owner's list, plus one head per owner in
 * an SosTable keyed (owner, 0), holding the first and last item and the count.
 * The doubly linked list makes Add, Remove, Has and Next constant time; the
 * tail pointer makes appending, and so insertion order, constant time too.
 * Only what callers add is stored, so the millions of owners with no items
 * cost nothing. Items and owners are never zero, so the (owner, 0) heads can
 * never collide with a node, exactly as a world id of zero means nobody. */

typedef struct SosLinks SosLinks;

/* An empty set, or NULL when memory ran out. */
SosLinks *SosLinksCreate(void);
void SosLinksDestroy(SosLinks *links);

/* Adds the pair (owner, item), which must both be non-zero, after any item the
 * owner already has. Idempotent: adding a pair that is present succeeds and
 * changes nothing. False when owner or item is zero or memory ran out, leaving
 * the set unchanged. */
bool SosLinksAdd(SosLinks *links, uint64_t owner, uint64_t item);

/* Reserves room so that a following run of adds cannot run out of memory:
 * `extraItems` more pairs and `extraOwners` more owners than the set already
 * has. False for a NULL set, when the totals would exceed the table limit, or
 * when memory ran out, in which case the set is unchanged but may keep any
 * room already won. Never changes which pairs are present. */
bool SosLinksReserve(SosLinks *links, uint32_t extraOwners, uint32_t extraItems);

/* Removes the pair (owner, item). False when it is absent. Never allocates. */
bool SosLinksRemove(SosLinks *links, uint64_t owner, uint64_t item);

/* True when the pair (owner, item) is present. */
bool SosLinksHas(const SosLinks *links, uint64_t owner, uint64_t item);

/* How many items the owner has, or zero for a NULL set or a zero owner. */
uint32_t SosLinksCount(const SosLinks *links, uint64_t owner);

/* The owner's oldest surviving item, or 0 when it has none. */
uint64_t SosLinksFirst(const SosLinks *links, uint64_t owner);

/* The item added after `item` for the owner, or 0 at the end of the list or
 * when `item` is not the owner's. Iterating First then Next visits the owner's
 * items oldest first without a buffer. */
uint64_t SosLinksNext(const SosLinks *links, uint64_t owner, uint64_t item);

/* Writes up to `capacity` of the owner's items into `out`, oldest first, and
 * returns how many there are, which may exceed `capacity`. Zero for a NULL set
 * or a zero owner, and zero for a NULL `out` with a non-zero `capacity`. */
uint32_t SosLinksList(const SosLinks *links, uint64_t owner, uint64_t *out, uint32_t capacity);

/* Removes every item of the owner and returns how many were removed, or zero
 * for a NULL set or a zero owner. */
uint32_t SosLinksRemoveOwner(SosLinks *links, uint64_t owner);

/* Folds the set into one value over its (owner, item) pairs, so equal sets
 * hash equally however they were built: neither the insertion order nor where
 * the nodes sit in the tables can change it. Zero for a NULL set. */
uint64_t SosLinksHash(const SosLinks *links);

#endif
