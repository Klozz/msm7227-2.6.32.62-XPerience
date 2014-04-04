#ifdef __CMA_ALLOCATORS_H

/* List all existing allocators here using CMA_ALLOCATOR macro. */

#ifdef CONFIG_CMA_BEST_FIT
CMA_ALLOCATOR("bf", bf)
#endif


#  undef CMA_ALLOCATOR
#else
#  define __CMA_ALLOCATORS_H

/* Function prototypes */
#  ifndef __LINUX_CMA_ALLOCATORS_H
#    define __LINUX_CMA_ALLOCATORS_H
#    define CMA_ALLOCATOR(name, infix)					\
	extern int cma_ ## infix ## _init(struct cma_region *);		\
	extern void cma_ ## infix ## _cleanup(struct cma_region *);	\
	extern struct cma_chunk *					\
	cma_ ## infix ## _alloc(struct cma_region *,			\
			      unsigned long, unsigned long);		\
	extern void cma_ ## infix ## _free(struct cma_chunk *);
#    include "cma-allocators.h"
#  endif

/* The cma_allocators array */
#  ifdef CMA_ALLOCATORS_LIST
#    define CMA_ALLOCATOR(_name, infix) {		\
		.name    = _name,			\
		.init    = cma_ ## infix ## _init,	\
		.cleanup = cma_ ## infix ## _cleanup,	\
		.alloc   = cma_ ## infix ## _alloc,	\
		.free    = cma_ ## infix ## _free,	\
	},
static struct cma_allocator cma_allocators[] = {
#    include "cma-allocators.h"
};
#    undef CMA_ALLOCATOR_LIST
#  endif
#  undef __CMA_ALLOCATORS_H
#endif
