#ifndef __LINUX_CMA_H
#define __LINUX_CMA_H

/*
 * Contiguous Memory Allocator framework
 * Copyright (c) 2010 by Samsung Electronics.
 * Written by Michal Nazarewicz (m.nazarewicz <at> samsung.com)
 */

/*
 * See Documentation/cma.txt for documentation.
 */

#ifdef __KERNEL__

struct device;

/**
 * cma_alloc() - allocates contiguous chunk of memory.
 *  <at> dev:	The device to perform allocation for.
 *  <at> kind:	A kind of memory to allocate.  A device may use several
 * 		different kinds of memory which are configured
 * 		separately.  Usually it's safe to pass NULL here.
 *  <at> size:	Size of the memory to allocate in bytes.
 *  <at> alignment:	Desired alignment.  Must be a power of two or zero.  If
 * 		alignment is less then a page size it will be set to
 * 		page size. If unsure, pass zero here.
 *
 * On error returns a negative error cast to unsigned long.  Use
 * IS_ERR_VALUE() to check if returned value is indeed an error.
 * Otherwise physical address of the chunk is returned.
 */
unsigned long __must_check
cma_alloc(const struct device *dev, const char *kind,
	  unsigned long size, unsigned long alignment);


/**
 * struct cma_info - information about regions returned by cma_info().
 *  <at> lower_bound:	The smallest address that is possible to be
 * 			allocated for given (dev, kind) pair.
 *  <at> upper_bound:	The one byte after the biggest address that is
 * 			possible to be allocated for given (dev, kind)
 * 			pair.
 *  <at> total_size:	Total size of regions mapped to (dev, kind) pair.
 *  <at> count:	Number of regions mapped to (dev, kind) pair.
 */
struct cma_info {
	unsigned long lower_bound, upper_bound;
	unsigned long total_size;
	unsigned count;
};

/**
 * cma_info() - queries information about regions.
 *  <at> info:	Pointer to a structure where to save the information.
 *  <at> dev:	The device to query information for.
 *  <at> kind:	A kind of memory to query information for.
 * 		If unsure, pass NULL here.
 *
 * On error returns a negative error, zero otherwise.
 */
int __must_check
cma_info(struct cma_info *info, const struct device *dev, const char *kind);


/**
 * cma_get() - increases reference counter of a chunk.
 *  <at> addr:	Beginning of the chunk.
 *
 * Returns zero on success or -ENOENT if there is no chunk at given
 * location.  In the latter case issues a warning and a stacktrace.
 */
int cma_get(unsigned long addr);

/**
 * cma_put() - decreases reference counter of a chunk.
 *  <at> addr:	Beginning of the chunk.
 *
 * Returns one if the chunk has been freed, zero if it hasn't, and
 * -ENOENT if there is no chunk at given location.  In the latter case
 * issues a warning and a stacktrace.
 *
 * If this function returns zero, you still can not count on the area
 * remaining in memory.  Only use the return value if you want to see
 * if the area is now gone, not present.
 */
int cma_put(unsigned long addr);

#endif

#endif
