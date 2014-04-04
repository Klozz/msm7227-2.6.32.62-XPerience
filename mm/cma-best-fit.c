/*
 * Contiguous Memory Allocator framework: Best Fit allocator
 * Copyright (c) 2010 by Samsung Electronics.
 * Written by Michal Nazarewicz (m.nazarewicz <at> samsung.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License or (at your optional) any later version of the license.
 */

#define pr_fmt(fmt) "cma: bf: " fmt

#ifdef CONFIG_CMA_DEBUG
#  define DEBUG
#endif

#include <linux/errno.h>       /* Error numbers */
#include <linux/slab.h>        /* kmalloc() */

#include <linux/cma-int.h>     /* CMA structures */

#include "cma-allocators.h"    /* Prototypes */


/************************* Data Types *************************/

struct cma_bf_item {
	struct cma_chunk ch;
	struct rb_node by_size;
};

struct cma_bf_private {
	struct rb_root by_start_root;
	struct rb_root by_size_root;
};


/************************* Prototypes *************************/

/*
 * Those are only for holes.  They must be called whenever hole's
 * properties change but also whenever chunk becomes a hole or hole
 * becames a chunk.
 */
static void __cma_bf_hole_insert_by_size(struct cma_bf_item *item);
static void __cma_bf_hole_erase_by_size(struct cma_bf_item *item);
static void __cma_bf_hole_insert_by_start(struct cma_bf_item *item);
static void __cma_bf_hole_erase_by_start(struct cma_bf_item *item);

/**
 * __cma_bf_hole_take() - takes a chunk of memory out of a hole.
 *  <at> hole:	hole to take chunk from
 *  <at> size	chunk's size
 *  <at> alignment:	chunk's starting address alignment (must be power of wo)
 *
 * Takes a  <at> size bytes large chunk from hole  <at> hole which must be ble
 * to hold the chunk.  The "must be able" includes also alignment
 * constraint.
 *
 * Returns allocated item or NULL on error (if kmalloc() failed).
 */
static struct cma_bf_item *__must_check
__cma_bf_hole_take(struct cma_bf_item *hole, size_t size, size_t alignment);

/**
 * __cma_bf_hole_merge_maybe() - tries to merge hole with neighbours.
 *
 *  <at> item hole to try and merge
 *
 * Which items are preserved is undefined so you may not rely on it.
 */
static void __cma_bf_hole_merge_maybe(struct cma_bf_item *item);


/************************* Device API *************************/

int cma_bf_init(struct cma_region *reg)
{
	struct cma_bf_private *prv;
	struct cma_bf_item *item;

	prv = kzalloc(sizeof *prv, GFP_NOWAIT);
	if (unlikely(!prv))
		return -ENOMEM;

	item = kzalloc(sizeof *item, GFP_NOWAIT);
	if (unlikely(!item)) {
		kfree(prv);
		return -ENOMEM;
	}

	item->ch.start = reg->start;
	item->ch.size  = reg->size;
	item->ch.reg   = reg;

	rb_root_init(&prv->by_start_root, &item->ch.by_start);
	rb_root_init(&prv->by_size_root, &item->by_size);

	reg->private_data = prv;
	return 0;
}

void cma_bf_cleanup(struct cma_region *reg)
{
	struct cma_bf_private *prv = reg->private_data;
	struct cma_bf_item *item =
		rb_entry(prv->by_size_root.rb_node,
			 struct cma_bf_item, by_size);

	/* We can assume there is only a single hole in the tree. */
	WARN_ON(item->by_size.rb_left || item->by_size.rb_right ||
		item->ch.by_start.rb_left || item->ch.by_start.rb_right);

	kfree(item);
	kfree(prv);
}

struct cma_chunk *cma_bf_alloc(struct cma_region *reg,
			       unsigned long size, unsigned long alignment)
{
	struct cma_bf_private *prv = reg->private_data;
	struct rb_node *node = prv->by_size_root.rb_node;
	struct cma_bf_item *item = NULL;
	unsigned long start, end;

	/* First first item that is large enough */
	while (node) {
		struct cma_bf_item *i =
			rb_entry(node, struct cma_bf_item, by_size);

		if (i->ch.size < size) {
			node = node->rb_right;
		} else if (i->ch.size >= size) {
			node = node->rb_left;
			item = i;
		}
	}
	if (unlikely(!item))
		return NULL;

	/* Now look for items which can satisfy alignment requirements */
	for (;;) {
		start = ALIGN(item->ch.start, alignment);
		end   = item->ch.start + item->ch.size;
		if (start < end && end - start >= size) {
			item = __cma_bf_hole_take(item, size, alignment);
			return likely(item) ? &item->ch : NULL;
		}

		node = rb_next(node);
		if (!node)
			return NULL;

		item  = rb_entry(node, struct cma_bf_item, by_size);
	}
}

void cma_bf_free(struct cma_chunk *chunk)
{
	struct cma_bf_item *item = container_of(chunk, struct cma_bf_item, ch);

	/* Add new hole */
	__cma_bf_hole_insert_by_size(item);
	__cma_bf_hole_insert_by_start(item);

	/* Merge with prev and next sibling */
	__cma_bf_hole_merge_maybe(item);
}


/************************* Basic Tree Manipulation *************************/

#define __CMA_BF_HOLE_INSERT(root, node, field) ({			\
	bool equal = false;						\
	struct rb_node **link = &(root).rb_node, *parent = NULL;	\
	const unsigned long value = item->field;			\
	while (*link) {							\
		struct cma_bf_item *i;					\
		parent = *link;						\
		i = rb_entry(parent, struct cma_bf_item, node);		\
		link = value <= i->field				\
			? &parent->rb_left				\
			: &parent->rb_right;				\
		equal = equal || value == i->field;			\
	}								\
	rb_link_node(&item->node, parent, link);			\
	rb_insert_color(&item->node, &root);				\
	equal;								\
})

static void __cma_bf_hole_insert_by_size(struct cma_bf_item *item)
{
	struct cma_bf_private *prv = item->ch.reg->private_data;
	(void)__CMA_BF_HOLE_INSERT(prv->by_size_root, by_size, ch.size);
}

static void __cma_bf_hole_erase_by_size(struct cma_bf_item *item)
{
	struct cma_bf_private *prv = item->ch.reg->private_data;
	rb_erase(&item->by_size, &prv->by_size_root);
}

static void __cma_bf_hole_insert_by_start(struct cma_bf_item *item)
{
	struct cma_bf_private *prv = item->ch.reg->private_data;
	/*
	 * __CMA_BF_HOLE_INSERT returns true if there was another node
	 * with the same value encountered.  This should never happen
	 * for start address and so we produce a warning.
	 *
	 * It's really some kind of bug if we got to such situation
	 * and things may start working incorrectly.  Nonetheless,
	 * there is not much we can do other then screaming as loud as
	 * we can hoping someone will notice the bug and fix it.
	 */
	WARN_ON(__CMA_BF_HOLE_INSERT(prv->by_start_root, ch.by_start,
				     ch.start));
}

static void __cma_bf_hole_erase_by_start(struct cma_bf_item *item)
{
	struct cma_bf_private *prv = item->ch.reg->private_data;
	rb_erase(&item->ch.by_start, &prv->by_start_root);
}


/************************* More Tree Manipulation *************************/

static struct cma_bf_item *__must_check
__cma_bf_hole_take(struct cma_bf_item *hole, size_t size, size_t alignment)
{
	struct cma_bf_item *item;

	/*
	 * There are three cases:
	 * 1. the chunk takes the whole hole,
	 * 2. the chunk is at the beginning or at the end of the hole, or
	 * 3. the chunk is in the middle of the hole.
	 */


	/* Case 1, the whole hole */
	if (size == hole->ch.size) {
		__cma_bf_hole_erase_by_size(hole);
		__cma_bf_hole_erase_by_start(hole);
		return hole;
	}


	/* Allocate */
	item = kmalloc(sizeof *item, GFP_KERNEL);
	if (unlikely(!item))
		return NULL;

	item->ch.start = ALIGN(hole->ch.start, alignment);
	item->ch.size  = size;

	/* Case 3, in the middle */
	if (item->ch.start != hole->ch.start
	 && item->ch.start + item->ch.size !=
	    hole->ch.start + hole->ch.size) {
		struct cma_bf_item *next;

		/*
		 * Space between the end of the chunk and the end of
		 * the region, ie. space left after the end of the
		 * chunk.  If this is dividable by alignment we can
		 * move the chunk to the end of the hole.
		 */
		unsigned long left =
			hole->ch.start + hole->ch.size -
			(item->ch.start + item->ch.size);
		if (left % alignment == 0) {
			item->ch.start += left;
			goto case_2;
		}

		/*
		 * We are going to add a hole at the end.  This way,
		 * we will reduce the problem to case 2 -- the chunk
		 * will be at the end of the hole.
		 */
		next = kmalloc(sizeof *next, GFP_KERNEL);

		if (unlikely(!next)) {
			kfree(item);
			return NULL;
		}

		next->ch.start = item->ch.start + item->ch.size;
		next->ch.size  =
			hole->ch.start + hole->ch.size - next->ch.start;
		next->ch.reg   = hole->ch.reg;

		__cma_bf_hole_insert_by_size(next);
		__cma_bf_hole_insert_by_start(next);

		hole->ch.size = next->ch.start - hole->ch.start;
		/* Go to case 2 */
	}


	/* Case 2, at the beginning or at the end */
case_2:
	/* No need to update the tree; order preserved. */
	if (item->ch.start == hole->ch.start)
		hole->ch.start += item->ch.size;

	/* Alter hole's size */
	hole->ch.size -= size;
	__cma_bf_hole_erase_by_size(hole);
	__cma_bf_hole_insert_by_size(hole);

	return item;
}


static void __cma_bf_hole_merge_maybe(struct cma_bf_item *item)
{
	struct cma_bf_item *prev;
	struct rb_node *node;
	int twice = 2;

	node = rb_prev(&item->ch.by_start);
	if (unlikely(!node))
		goto next;
	prev = rb_entry(node, struct cma_bf_item, ch.by_start);

	for (;;) {
		if (prev->ch.start + prev->ch.size == item->ch.start) {
			/* Remove previous hole from trees */
			__cma_bf_hole_erase_by_size(prev);
			__cma_bf_hole_erase_by_start(prev);

			/* Alter this hole */
			item->ch.size += prev->ch.size;
			item->ch.start = prev->ch.start;
			__cma_bf_hole_erase_by_size(item);
			__cma_bf_hole_insert_by_size(item);
			/*
			 * No need to update by start trees as we do
			 * not break sequence order
			 */

			/* Free prev hole */
			kfree(prev);
		}

next:
		if (!--twice)
			break;

		node = rb_next(&item->ch.by_start);
		if (unlikely(!node))
			break;
	prev = item;
		item = rb_entry(node, struct cma_bf_item, ch.by_start);
	}
}
