#ifndef __LINUX_CMA_INT_H
#define __LINUX_CMA_INT_H

#ifdef CONFIG_CMA

/*
 * Contiguous Memory Allocator framework: internal header
 * Copyright (c) 2010 by Samsung Electronics.
 * Written by Michal Nazarewicz (m.nazarewicz <at> samsung.com)
 */

/*
 * See Documentation/cma.txt for documentation.
 */

#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>

struct cma_allocator;

/**
 * struct cma_region - a region reserved for CMA allocations.
 *  <at> name:	Unique name of the region.  Passed with cmdline.  Read only.
 *  <at> start:	Physical starting address of the region in bytes.  Always
 * 		aligned to the value specified by  <at> alignment.  Initialised
 * 		from value read from cmdline.  Read only for allocators.
 * 		Read write for platform-specific region allocation code.
 *  <at> size:	Physical size of the region in bytes.  At least page
 * 		aligned.  Initialised with value read from cmdline.  Read
 * 		only for allocators.  Read write for platform-specific region
 * 		allocation code.
 *  <at> alignment:	Desired alignment of the region.  A power of two, greater or\
 * 		equal PAGE_SIZE.  Initialised from value read from cmdline.
 * 		Read only.
 *  <at> alloc:	Allocator used with this region.  NULL means region is not
 * 		allocated.  Read only.
 *  <at> alloc_name:	Allocator name read from cmdline.  Private.
 *  <at> alloc_params:	Allocator-specific parameters read from cmdline.
 * 			Read only for allocator.
 *  <at> private_data:	Allocator's private data.
 *  <at> users:	Number of chunks allocated in this region.
 *  <at> mutex:	Guarantees that only one allocation/deallocation on given
 * 		region is performed.
 */
struct cma_region {
	const char *name;
	unsigned long start;
	unsigned long size, free_space;
	unsigned long alignment;

	struct cma_allocator *alloc;
	const char *alloc_name;
	const char *alloc_params;
	void *private_data;

	unsigned users;
	/*
	 * Protects the "users" and "free_space" fields and any calls
	 * to allocator on this region thus guarantees only one call
	 * to allocator will operate on this region..
	 */
	struct mutex mutex;
};

/**
 * struct cma_chunk - an allocated contiguous chunk of memory.
 *  <at> start:	Physical address in bytes.
 *  <at> size:	Size in bytes.
 *  <at> free_space:	Free space in region in bytes.  Read only.
 *  <at> reg:	Region this chunk belongs to.
 *  <at> kref:	Number of references.  Private.
 *  <at> by_start:	A node in an red-black tree with all chunks sorted by
 * 		start address.
 *
 * The cma_allocator::alloc() operation need to set only the  <at> start
 * and  <at> size fields.  The rest is handled by the caller (ie. CMA
 * glue).
 */
struct cma_chunk {
	unsigned long start;
	unsigned long size;

	struct cma_region *reg;
	struct kref ref;
	struct rb_node by_start;
};


/**
 * struct cma_allocator - a CMA allocator.
 *  <at> name:	Allocator's unique name
 *  <at> init:	Initialises a allocator on given region.  May not sleep.
 *  <at> cleanup:	Cleans up after init.  May assume that there are no chunks
 * 		allocated in given region.  May not sleep.
 *  <at> alloc:	Allocates a chunk of memory of given size in bytes and
 * 		with given alignment.  Alignment is a power of
 * 		two (thus non-zero) and callback does not need to check it.
 * 		May also assume that it is the only call that uses given
 * 		region (ie. access to the region is synchronised with
 * 		a mutex).  This has to allocate the chunk object (it may be
 * 		contained in a bigger structure with allocator-specific data.
 * 		May sleep.
 *  <at> free:	Frees allocated chunk.  May also assume that it is the only
 * 		call that uses given region.  This has to kfree() the chunk
 * 		object as well.  May sleep.
 */
struct cma_allocator {
	const char *name;
	int (*init)(struct cma_region *reg);
	void (*cleanup)(struct cma_region *reg);
	struct cma_chunk *(*alloc)(struct cma_region *reg, unsigned long size,
				   unsigned long alignment);
	void (*free)(struct cma_chunk *chunk);
};


/**
 * cma_region - a list of regions filled when parameters are parsed.
 *
 * This is terminated by an zero-sized entry (ie. an entry which size
 * field is zero).  Platform needs to allocate space for each of the
 * region before initcalls are executed.
 */
extern struct cma_region cma_regions[];


/**
 * cma_defaults() - specifies default command line parameters.
 *  <at> cma:	Default cma parameter if one was not specified via command
 * 		line.
 *  <at> cma_map:	Default cma_map parameter if one was not specified via
 * 		command line.
 *
 * This function should be called prior to cma_regions_allocate() and
 * after early parameters have been parsed.  The  <at> cma argument is only
 * used if there was no cma argument passed on command line.  The same
 * goes for  <at> cma_map which is used only if cma_map was not passed on
 * command line.
 *
 * Either of the argument may be NULL.
 *
 * Returns negative error code if there was an error parsing either of
 * the parameters or zero.
 */
int __init cma_defaults(const char *cma, const char *cma_map);


/**
 * cma_region_alloc() - allocates a physically contiguous memory region.
 *  <at> reg:	Region to allocate memory for.
 *
 * If platform supports bootmem this is the first allocator this
 * function tries to use.  If that failes (or bootmem is not
 * supported) function tries to use memblec if it is available.
 *
 * Returns zero or negative error.
 */
int __init cma_region_alloc(struct cma_region *reg);

/**
 * cma_regions_allocate() - helper function for allocating regions.
 *  <at> alloc:	Region allocator.  Needs to return non-negative if
 * 		allocation succeeded, negative error otherwise.  NULL
 * 		means cma_region_alloc() should be used.
 *
 * This function traverses the cma_regions array and tries to reserve
 * memory for each region.  It uses the  <at> alloc callback function for
 * that purpose.  If allocation failes for a given region, it is
 * removed from the array (by shifting all the elements after it).
 *
 * Returns number of reserved regions.
 */
int __init cma_regions_allocate(int (*alloc)(struct cma_region *reg));

#else

#define cma_regions_allocate(alloc) ((int)0)

#endif


#endif
