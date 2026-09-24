#ifndef IDM_H
#define IDM_H

/**
 * @file idm.h
 * @brief ID and index management utilities for Corm.
 *
 * Provides linked-list–based tracking of reusable integer
 * IDs and index allocation routines used internally by Corm.
 *
 * License: BSD-2-Clause
 */

#include "./queue.h"
#include <stddef.h>
#include <stdint.h>

/** @defgroup idm_constants IDM constants
 *  @brief Basic constants and return codes.
 *  @{
 */

/**
 * @brief Value representing an invalid or missing ID.
 */
#define IDM_MISS ((uint32_t)-1)

/** @} */

/** @defgroup idm_structs IDM structures
 *  @brief Data types used by the ID management system.
 *  @{
 */

/**
 * @brief Node representing a stored integer ID.
 */
typedef struct ids_item {
	uint32_t value;                 /* Stored ID value. */
	SLIST_ENTRY(ids_item) entry;    /* List */
} idsi_t;

SLIST_HEAD(ids, ids_item);
typedef struct ids ids_t;

/**
 * @brief Index/ID manager with a free-list and counter.
 */
typedef struct {
	ids_t free;     /* Free list of reusable IDs. */
	uint32_t last;  /* Last issued ID + 1. */
} idm_t;

/** @} */

/** @defgroup idm_init IDM initialization
 *  @brief Initialization helpers for lists and managers.
 *  @{
 */

/**
 * @brief Initialize an empty ID list.
 *
 * @return Newly initialized empty list.
 */
static inline ids_t ids_init(void) {
	struct ids list;
	SLIST_INIT(&list);
	return list;
}

/**
 * @brief Initialize an ID manager.
 *
 * Creates a new manager with no allocated IDs.
 *
 * @return Initialized `idm_t` structure.
 */
static inline idm_t idm_init(void) {
	idm_t idm;
	idm.free = ids_init();
	idm.last = 0;
	return idm;
}

/** @} */

/** @defgroup idm_iter IDM iteration helpers
 *  @brief Functions for traversing ID lists.
 *  @{
 */

/**
 * @brief Peek at the top ID without removing it.
 *
 * @param[in] list ID list.
 * @return Top ID or `IDM_MISS` if empty.
 */
static inline uint32_t ids_peek(ids_t *list) {
	idsi_t *top = SLIST_FIRST(list);
	return top ? top->value : IDM_MISS;
}

/**
 * @brief Get the first iterator node.
 *
 * @param[in] list ID list.
 * @return Pointer to the first node.
 */
static inline idsi_t *ids_iter(ids_t *list) {
	return SLIST_FIRST(list);
}

/**
 * @brief Advance iterator to the next node.
 *
 * @param[out] id  Retrieved ID value.
 * @param[in,out] cur Current iterator pointer.
 * @return 1 if valid, 0 if end of list.
 */
static inline int ids_next(uint32_t *id, idsi_t **cur) {
	idsi_t *prev = *cur;
	if (!prev)
		return 0;
	*id = prev->value;
	*cur = SLIST_NEXT(prev, entry);
	return 1;
}

/** @} */

/** @defgroup idm_ops IDM operations
 *  @brief Allocation and management routines.
 *  @{
 */

/**
 * @brief Push an ID into the free list.
 *
 * @param[in,out] list List to append to.
 * @param[in] id       ID value to add.
 */
void ids_push(ids_t *list, uint32_t id);

/**
 * @brief Pop an ID from the free list.
 *
 * @param[in,out] list List to remove from.
 * @return Removed ID, or `IDM_MISS` if empty.
 */
uint32_t ids_pop(ids_t *list);

/**
 * @brief Free all IDs in the list.
 *
 * @param[in,out] list List to clear.
 */
void ids_drop(ids_t *list);

/**
 * @brief Remove a specific ID from the list.
 *
 * @param[in,out] list List to modify.
 * @param[in] id       ID to remove.
 */
void ids_remove(ids_t *list, uint32_t id);

/**
 * @brief Free a specific ID node.
 *
 * Alias for `ids_remove()`.
 *
 * @param[in,out] list List to modify.
 * @param[in] id       ID to remove.
 */
void ids_free(ids_t *list, uint32_t id);

/**
 * @brief Drop all IDs managed by an ID manager.
 *
 * @param[in,out] idm ID manager to clear.
 */
void idm_drop(idm_t *idm);

/**
 * @brief Push IDs up to a given value.
 *
 * @param[in,out] idm ID manager.
 * @param[in] n       Highest value to reach.
 * @return The ID pushed, or `IDM_MISS` if out of range.
 */
uint32_t idm_push(idm_t *idm, uint32_t n);

/**
 * @brief Delete a specific ID from manager.
 *
 * Returns 1 if the deletion updated `last`, else 0.
 *
 * @param[in,out] idm ID manager.
 * @param[in] id      ID to remove.
 * @return 1 if success, 0 otherwise.
 */
int idm_del(idm_t *idm, uint32_t id);

/**
 * @brief Allocate a new unique ID.
 *
 * Returns a previously freed ID if available,
 * otherwise increments `last`.
 *
 * @param[in,out] idm ID manager.
 * @return New ID value.
 */
uint32_t idm_new(idm_t *idm);

/** @} */

#endif /* IDM_H */
