/*
 * Contiguous Memory Allocator framework
 * Copyright (c) 2010 by Samsung Electronics.
 * Written by Michal Nazarewicz (m.nazarewicz <at> samsung.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License or (at your optional) any later version of the license.
 */

/*
 * See Documentation/cma.txt for documentation.
 */

#define pr_fmt(fmt) "cma: " fmt

#ifdef CONFIG_CMA_DEBUG
#  define DEBUG
#endif

#ifndef CONFIG_NO_BOOTMEM
#  include <linux/bootmem.h>   /* alloc_bootmem_pages_nopanic() */
#endif
#ifdef CONFIG_HAVE_MEMBLOCK
#  include <linux/memblock.h>  /* memblock*() */
#endif
#include <linux/device.h>      /* struct device, dev_name() */
#include <linux/errno.h>       /* Error numbers */
#include <linux/err.h>         /* IS_ERR, PTR_ERR, etc. */
#include <linux/mm.h>          /* PAGE_ALIGN() */
#include <linux/module.h>      /* EXPORT_SYMBOL_GPL() */
#include <linux/slab.h>        /* kmalloc() */
#include <linux/string.h>      /* str*() */

#include <linux/cma-int.h>     /* CMA structures */
#include <linux/cma.h>         /* CMA Device API */


#define CMA_MAX_REGIONS      16
#define CMA_MAX_MAPPINGS     64
#define CMA_MAX_PARAM_LEN   512


/************************* Parse region list *************************/

struct cma_region cma_regions[CMA_MAX_REGIONS + 1 /* 1 for zero-sized */];

static char *__must_check __init
cma_param_parse_entry(char *param, struct cma_region *reg)
{
	const char *name, *alloc = NULL, *alloc_params = NULL, *ch;
	unsigned long long size, start = 0, alignment = 0;

	/* Parse */
	name = param;
	param = strchr(param, '=');
	if (!param) {
		pr_err("param: expecting '=' near %s\n", name);
		return NULL;
	} else if (param == name) {
		pr_err("param: empty region name near %s\n", name);
		return NULL;
	}
	*param = '\0';
	++param;

	ch = param;
	size = memparse(param, &param);
	if (unlikely(!size || size > ULONG_MAX)) {
		pr_err("param: invalid size near %s\n", ch);
		return NULL;
	}


	if (*param == ' <at> ') {
		ch = param;
		start = memparse(param + 1, &param);
		if (unlikely(start > ULONG_MAX)) {
			pr_err("param: invalid start near %s\n", ch);
			return NULL;
		}
	}

	if (*param == '/') {
		ch = param;
		alignment = memparse(param + 1, &param);
		if (unlikely(alignment > ULONG_MAX ||
			     (alignment & (alignment - 1)))) {
			pr_err("param: invalid alignment near %s\n", ch);
			return NULL;
		}
	}

	if (*param == ':') {
		alloc = ++param;
		while (*param && *param != '(' && *param != ';')
			++param;

		if (*param == '(') {
			*param = '\0';
			alloc_params = ++param;
			param = strchr(param, ')');
			if (!param) {
				pr_err("param: expecting ')' near %s\n", param);
				return NULL;
			}
			*param++ = '\0';
		}
	}

	if (*param == ';') {
		*param = '\0';
		++param;
	} else if (*param) {
		pr_err("param: expecting ';' or end of parameter near %s\n",
		       param);
		return NULL;
	}

	/* Save */
	alignment         = alignment ? PAGE_ALIGN(alignment) : PAGE_SIZE;
	start             = ALIGN(start, alignment);
	size              = PAGE_ALIGN(size);
	reg->name         = name;
	reg->start        = start;
	reg->size         = size;
	reg->free_space   = size;
	reg->alignment    = alignment;
	reg->alloc_name   = alloc && *alloc ? alloc : NULL;
	reg->alloc_params = alloc_params;

	return param;
}

/*
 * cma          ::=  "cma=" regions [ ';' ]
 * regions      ::= region [ ';' regions ]
 *
 * region       ::= reg-name
 *                    '=' size
 *                  [ ' <at> ' start ]
 *                  [ '/' alignment ]
 *                  [ ':' [ alloc-name ] [ '(' alloc-params ')' ] ]
 *
 * See Documentation/cma.txt for details.
 *
 * Example:
 * cma=reg1=64M:bf;reg2=32M <at> 0x100000:bf;reg3=64M/1M:bf
 *
 * If allocator is ommited the first available allocater will be used.
 */

static int __init cma_param_parse(char *param)
{
	static char buffer[CMA_MAX_PARAM_LEN];

	unsigned left = ARRAY_SIZE(cma_regions);
	struct cma_region *reg = cma_regions;

	pr_debug("param: %s\n", param);

	strlcpy(buffer, param, sizeof buffer);
	for (param = buffer; *param; ++reg) {
		if (unlikely(!--left)) {
			pr_err("param: too many regions\n");
			return -ENOSPC;
		}

		param = cma_param_parse_entry(param, reg);
		if (unlikely(!param))
			return -EINVAL;

		pr_debug("param: adding region %s (%p <at> %p)\n",
			 reg->name, (void *)reg->size, (void *)reg->start);
	}
	return 0;
}
early_param("cma", cma_param_parse);


/************************* Parse dev->regions map *************************/

static const char *cma_map[CMA_MAX_MAPPINGS + 1 /* 1 for NULL */];

/*
 * cma-map      ::=  "cma_map=" rules [ ';' ]
 * rules        ::= rule [ ';' rules ]
 * rule         ::= patterns '=' regions
 * patterns     ::= pattern [ ',' patterns ]
 *
 * regions      ::= reg-name [ ',' regions ]
 *              // list of regions to try to allocate memory
 *              // from for devices that match pattern
 *
 * pattern      ::= dev-pattern [ '/' kind-pattern ]
 *                | '/' kind-pattern
 *              // pattern request must match for this rule to
 *              // apply to it; the first rule that matches is
 *              // applied; if dev-pattern part is omitted
 *              // value identical to the one used in previous
 *              // rule is assumed
 *
 * See Documentation/cma.txt for details.
 *
 * Example (white space added for convenience, forbidden in real string):
 * cma_map = foo-dev = reg1;             -- foo-dev with no kind
 *           bar-dev / firmware = reg3;  -- bar-dev's firmware
 *           / * = reg2;                 -- bar-dev's all other kinds
 *           baz-dev / * = reg1,reg2;    -- any kind of baz-dev
 *           * / * = reg2,reg1;          -- any other allocations
 */
static int __init cma_map_param_parse(char *param)
{
	static char buffer[CMA_MAX_PARAM_LEN];

	unsigned left = ARRAY_SIZE(cma_map) - 1;
	const char **spec = cma_map;

	pr_debug("map: %s\n", param);

	strlcpy(buffer, param, sizeof buffer);
	for (param = buffer; *param; ++spec) {
		char *eq, *e;

		if (!left--) {
			pr_err("map: too many mappings\n");
			return -ENOSPC;
		}

		e = strchr(param, ';');
		if (e)
			*e = '\0';

		eq = strchr(param, '=');
		if (unlikely(!eq)) {
			pr_err("map: expecting '='\n");
			cma_map[0] = NULL;
			return -EINVAL;
	}

		*eq = '\0';
		*spec = param;

		pr_debug("map: adding: '%s' -> '%s'\n", param, eq + 1);

		if (!e)
			break;
		param = e + 1;
	}

	return 0;
}
early_param("cma_map", cma_map_param_parse);


/************************* Initialise CMA *************************/

#define CMA_ALLOCATORS_LIST
#include "cma-allocators.h"

static struct cma_allocator *__must_check __init
__cma_allocator_find(const char *name)
{
	size_t i = ARRAY_SIZE(cma_allocators);

	if (i) {
		struct cma_allocator *alloc = cma_allocators;

		if (!name)
			return alloc;

		do {
			if (!strcmp(alloc->name, name))
				return alloc;
			++alloc;
		} while (--i);
	}

	return NULL;
}


int __init cma_defaults(const char *cma_str, const char *cma_map_str)
{
	int ret;

	if (cma_str && !cma_regions->size) {
		ret = cma_param_parse((char *)cma_str);
		if (unlikely(ret))
			return ret;
	}

	if (cma_map_str && !*cma_map) {
		ret = cma_map_param_parse((char *)cma_map_str);
		if (unlikely(ret))
			return ret;
	}

	return 0;
}


int __init cma_region_alloc(struct cma_region *reg)
{

#ifndef CONFIG_NO_BOOTMEM

	void *ptr;

	ptr = __alloc_bootmem_nopanic(reg->size, reg->alignment, reg->start);
	if (likely(ptr)) {
		reg->start = virt_to_phys(ptr);
		return 0;
	}

#endif

#ifdef CONFIG_HAVE_MEMBLOCK

	if (reg->start) {
		if (memblock_is_region_reserved(reg->start, reg->size) < 0 &&
		    memblock_reserve(reg->start, reg->size) >= 0)
			return 0;
	} else {
		/*
		 * Use __memblock_alloc_base() since
		 * memblock_alloc_base() panic()s.
		 */
		u64 ret = __memblock_alloc_base(reg->size, reg->alignment, 0);
		if (ret && ret < ULONG_MAX && ret + reg->size < ULONG_MAX) {
			reg->start = ret;
			return 0;
		}

		if (ret)
			memblock_free(ret, reg->size);
	}

#endif

	return -ENOMEM;
}

int __init cma_regions_allocate(int (*alloc)(struct cma_region *reg))
{
	struct cma_region *reg = cma_regions, *out = cma_regions;

	pr_debug("allocating\n");

	if (!alloc)
		alloc = cma_region_alloc;

	for (; reg->size; ++reg) {
		if (likely(alloc(reg) >= 0)) {
			pr_debug("init: %s: allocated %p <at> %p\n",
				 reg->name,
				 (void *)reg->size, (void *)reg->start);
			if (out != reg)
				memcpy(out, reg, sizeof *out);
			++out;
		} else {
		printk("init: %s: unable to allocate %p <at> %p\n",
				reg->name,
				(void *)reg->size, (void *)reg->start);
		}
	}
	out->size = 0; /* Zero size termination */

	return cma_regions - out;
}

static int __init cma_init(void)
{
	struct cma_allocator *alloc;
	struct cma_region *reg;

	pr_debug("initialising\n");

	for (reg = cma_regions; reg->size; ++reg) {
		mutex_init(&reg->mutex);

		alloc = __cma_allocator_find(reg->alloc_name);
		if (unlikely(!alloc)) {
			printk("init: %s: %s: no such allocator\n",
				reg->name, reg->alloc_name ?: "(default)");
			continue;
		}

		if (unlikely(alloc->init(reg))) {
			pr_err("init: %s: %s: unable to initialise allocator\n",
			       reg->name, alloc->name);
			continue;
		}

		reg->alloc      = alloc;
		reg->alloc_name = alloc->name; /* it may have been NULL */
		pr_debug("init: %s: %s: initialised allocator\n",
			 reg->name, reg->alloc_name);
	}

	return 0;
}
subsys_initcall(cma_init);


/************************* Various prototypes *************************/

static struct cma_chunk *__must_check __cma_chunk_find(unsigned long addr);
static int __must_check __cma_chunk_insert(struct cma_chunk *chunk);
static void __cma_chunk_release(struct kref *ref);

static struct cma_region *__must_check
__cma_region_find(const char *name, unsigned n);

static const char *__must_check
__cma_where_from(const struct device *dev, const char *kind);
static struct cma_chunk *__must_check
__cma_alloc_do(const char *from, unsigned long size, unsigned long alignment);


/************************* The Device API *************************/

unsigned long __must_check
cma_alloc(const struct device *dev, const char *kind,
	  unsigned long size, unsigned long alignment)
{
	struct cma_chunk *chunk;
	const char *from;

	pr_debug("allocate %p/%p for %s/%s\n",
		 (void *)size, (void *)alignment, dev_name(dev), kind ?: "");

	if (unlikely(alignment & (alignment - 1) || !size))
		return -EINVAL;

	from = __cma_where_from(dev, kind);
	if (unlikely(IS_ERR(from)))
		return PTR_ERR(from);

	chunk = __cma_alloc_do(from, size, alignment ?: 1);
	if (chunk)
		pr_debug("allocated at %p\n", (void *)chunk->start);
	else
		pr_debug("not enough memory\n");

	return chunk ? chunk->start : -ENOMEM;
}
EXPORT_SYMBOL_GPL(cma_alloc);


int __must_check
cma_info(struct cma_info *info, const struct device *dev, const char *kind)
{
	struct cma_info ret = { ~0, 0, 0, 0 };
	const char *from;

	if (unlikely(!info))
		return -EINVAL;

	from = __cma_where_from(dev, kind);
	if (unlikely(IS_ERR(from)))
		return PTR_ERR(from);

	while (*from) {
		const char *end = strchr(from, ',');
		struct cma_region *reg =
			__cma_region_find(from, end ? end - from : strlen(from));
		if (reg) {
			ret.total_size += reg->size;
			if (ret.lower_bound > reg->start)
				ret.lower_bound = reg->start;
			if (ret.upper_bound < reg->start + reg->size)
				ret.upper_bound = reg->start + reg->size;
			++ret.count;
		}
		if (!end)
			break;
		from = end + 1;
	}

	memcpy(info, &ret, sizeof ret);
	return 0;
}
EXPORT_SYMBOL_GPL(cma_info);


int cma_get(unsigned long addr)
{
	struct cma_chunk *c = __cma_chunk_find(addr);

	pr_debug("get(%p): %sfound\n", (void *)addr, c ? "" : "not ");

	if (unlikely(!c))
		return -ENOENT;
	kref_get(&c->ref);
	return 0;
}
EXPORT_SYMBOL_GPL(cma_get);

int cma_put(unsigned long addr)
{
	struct cma_chunk *c = __cma_chunk_find(addr);
	int ret;

	pr_debug("put(%p): %sfound\n", (void *)addr, c ? "" : "not ");

	if (unlikely(!c))
		return -ENOENT;

	ret = kref_put(&c->ref, __cma_chunk_release);
	if (ret)
		pr_debug("put(%p): destroyed\n", (void *)addr);
	return ret;
}
EXPORT_SYMBOL_GPL(cma_put);


/************************* Implementation *************************/

static struct rb_root cma_chunks_by_start;;
static DEFINE_MUTEX(cma_chunks_mutex);

static struct cma_chunk *__must_check __cma_chunk_find(unsigned long addr)
{
	struct cma_chunk *chunk;
	struct rb_node *n;

	mutex_lock(&cma_chunks_mutex);

	for (n = cma_chunks_by_start.rb_node; n; ) {
		chunk = rb_entry(n, struct cma_chunk, by_start);
		if (addr < chunk->start)
			n = n->rb_left;
		else if (addr > chunk->start)
			n = n->rb_right;
		else
			goto found;
	}
	WARN("no chunk starting at %p\n", (void *)addr);
	chunk = NULL;

found:
	mutex_unlock(&cma_chunks_mutex);

	return chunk;
}

static int __must_check __cma_chunk_insert(struct cma_chunk *chunk)
{
	struct rb_node **new, *parent = NULL;
	unsigned long addr = chunk->start;

	mutex_lock(&cma_chunks_mutex);

	for (new = &cma_chunks_by_start.rb_node; *new; ) {
		struct cma_chunk *c =
			container_of(*new, struct cma_chunk, by_start);

		parent = *new;
		if (addr < c->start) {
			new = &(*new)->rb_left;
		} else if (addr > c->start) {
			new = &(*new)->rb_right;
		} else {
			/*
			 * We should never be here.  If we are it
			 * means allocator gave us an invalid chunk
			 * (one that has already been allocated) so we
			 * refuse to accept it.  Our caller will
			 * recover by freeing the chunk.
			 */
			WARN_ON(1);
			return -EBUSY;
		}
	}

	rb_link_node(&chunk->by_start, parent, new);
	rb_insert_color(&chunk->by_start, &cma_chunks_by_start);

	mutex_unlock(&cma_chunks_mutex);

	return 0;
}

static void __cma_chunk_release(struct kref *ref)
{
	struct cma_chunk *chunk = container_of(ref, struct cma_chunk, ref);

	mutex_lock(&cma_chunks_mutex);
	rb_erase(&chunk->by_start, &cma_chunks_by_start);
	mutex_unlock(&cma_chunks_mutex);

	mutex_lock(&chunk->reg->mutex);
	chunk->reg->alloc->free(chunk);
	--chunk->reg->users;
	chunk->reg->free_space += chunk->size;
	mutex_unlock(&chunk->reg->mutex);
}


static struct cma_region *__must_check
__cma_region_find(const char *name, unsigned n)
{
	struct cma_region *reg = cma_regions;

	for (; reg->start; ++reg) {
		if (!strncmp(name, reg->name, n) && !reg->name[n])
			return reg;
	}

	return NULL;
}


static const char *__must_check
__cma_where_from(const struct device *dev, const char *kind)
{
	/*
	 * This function matches the pattern given at command line
	 * parameter agains given device name and kind.  Kind may be
	 * of course NULL or an emtpy string.
	 */

	const char **spec, *name;
	int name_matched = 0;

	/* Make sure dev was given and has name */
	if (unlikely(!dev))
		return ERR_PTR(-EINVAL);

	name = dev_name(dev);
	if (WARN_ON(!name || !*name))
		return ERR_PTR(-EINVAL);

	/* kind == NULL is just like an empty kind */
	if (!kind)
		kind = "";
	/*
	 * Now we go throught the cma_map array.  It is an array of
	 * pointers to chars (ie. array of strings) so in each
	 * iteration we take each of the string.  The strings is
	 * basically what user provided at the command line separated
	 * by semicolons.
	 */
	for (spec = cma_map; *spec; ++spec) {
		/*
		 * This macro tries to match pattern pointed by s to
		 *  <at> what.  If, while reading the spec, we ecnounter
		 * comma it means that the pattern does not match and
		 * we need to start over with another spec.  If there
		 * is a character that does not match, we neet to try
		 * again looking if there is another spec.
		 */
#define TRY_MATCH(what) do {				\
		const char *c = what;			\
		for (; *s != '*' && *c; ++c, ++s)	\
			if (*s == ',')			\
				goto again;		\
			else if (*s != '?' && *c != *s)	\
				goto again_maybe;	\
		if (*s == '*')				\
			++s;				\
	} while (0)

		const char *s = *spec - 1;
again:
		++s;

		/*
		 * If the pattern is spec starts with a slash, this
		 * means that the device part of the pattern matches
		 * if it matched previously.
		 */
		if (*s == '/') {
			if (!name_matched)
				goto again_maybe;
			goto kind;
		}

		/*
		 * We are now trying to match the device name.  This
		 * also updates the name_matched variable.  If the
		 * name does not match we will jump to again or
		 * again_maybe out of the TRY_MATCH() macro.
		 */
		name_matched = 0;
		TRY_MATCH(name);
		name_matched = 1;

		/*
		 * Now we need to match the kind part of the pattern.
		 * If the pattern is missing it we match only if kind
		 * points to an empty string.  Otherwise wy try to
		 * match it just like name.
		 */
		if (*s != '/') {
			if (*kind)
				goto again_maybe;
		} else {
kind:
			++s;
			TRY_MATCH(kind);
		}

		/*
		 * Patterns end either when the string ends or on
		 * a comma.  Returned value is the part of the rule
		 * with list of region names.  This works because when
		 * we parse the cma_map parameter the equel sign in
		 * rules is replaced by a NUL byte.
		 */
		if (!*s || *s == ',')
			return s + strlen(s) + 1;

again_maybe:
		s = strchr(s, ',');
		if (s)
			goto again;

#undef TRY_MATCH
	}

	return ERR_PTR(-ENOENT);
}


static struct cma_chunk *__must_check
__cma_alloc_do(const char *from, unsigned long size, unsigned long alignment)
{
	struct cma_chunk *chunk;
	struct cma_region *reg;

	pr_debug("alloc_do(%p/%p from %s)\n",
		 (void *)size, (void *)alignment, from);

	while (*from) {
		const char *end = strchr(from, ',');
		reg = __cma_region_find(from, end ? end - from : strlen(from));
		if (unlikely(!reg || !reg->alloc))
			goto skip;

		if (reg->free_space < size)
			goto skip;

		mutex_lock(&reg->mutex);
		chunk = reg->alloc->alloc(reg, size, alignment);
		if (chunk) {
			++reg->users;
			reg->free_space -= chunk->size;
		}
		mutex_unlock(&reg->mutex);
		if (chunk)
			goto got;

skip:
		if (!end)
			break;
		from = end + 1;
	}
	return NULL;

got:
	chunk->reg = reg;
	kref_init(&chunk->ref);

	if (likely(!__cma_chunk_insert(chunk)))
		return chunk;

	mutex_lock(&reg->mutex);
	--reg->users;
	reg->free_space += chunk->size;
	chunk->reg->alloc->free(chunk);
	mutex_unlock(&reg->mutex);
	return NULL;
}
