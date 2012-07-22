
#if 0
#define XCACHE_DEBUG
#endif

#if 0
#define SHOW_DPRINT
#endif

/* {{{ macros */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <signal.h>

#include "xcache.h"
#ifdef ZEND_ENGINE_2_1
#include "ext/date/php_date.h"
#endif

#ifdef HAVE_XCACHE_OPTIMIZER
#	include "mod_optimizer/xc_optimizer.h"
#endif
#ifdef HAVE_XCACHE_COVERAGER
#	include "mod_coverager/xc_coverager.h"
#endif
#ifdef HAVE_XCACHE_DISASSEMBLER
#	include "mod_disassembler/xc_disassembler.h"
#endif

#include "xcache_globals.h"
#include "xc_processor.h"
#include "xcache/xc_extension.h"
#include "xcache/xc_ini.h"
#include "xcache/xc_const_string.h"
#include "xcache/xc_opcode_spec.h"
#include "xcache/xc_utils.h"
#include "xcache/xc_sandbox.h"
#include "util/xc_align.h"
#include "util/xc_stack.h"
#include "util/xc_vector.h"
#include "util/xc_trace.h"

#include "php.h"
#include "ext/standard/info.h"
#include "ext/standard/md5.h"
#include "ext/standard/php_math.h"
#include "ext/standard/php_string.h"
#include "SAPI.h"

#define VAR_ENTRY_EXPIRED(pentry) ((pentry)->ttl && XG(request_time) > (pentry)->ctime + (time_t) (pentry)->ttl)
#define CHECK(x, e) do { if ((x) == NULL) { zend_error(E_ERROR, "XCache: " e); goto err; } } while (0)
#define LOCK(x) xc_lock((x)->lck)
#define UNLOCK(x) xc_unlock((x)->lck)

#define ENTER_LOCK_EX(x) \
	xc_lock((x)->lck); \
	zend_try { \
		do
#define LEAVE_LOCK_EX(x) \
		while (0); \
	} zend_catch { \
		catched = 1; \
	} zend_end_try(); \
	xc_unlock((x)->lck)

#define ENTER_LOCK(x) do { \
	int catched = 0; \
	ENTER_LOCK_EX(x)
#define LEAVE_LOCK(x) \
	LEAVE_LOCK_EX(x); \
	if (catched) { \
		zend_bailout(); \
	} \
} while(0)

/* }}} */

/* {{{ globals */
static char *xc_shm_scheme = NULL;
static char *xc_mmap_path = NULL;
static char *xc_coredump_dir = NULL;
static zend_bool xc_disable_on_crash = 0;

static xc_hash_t xc_php_hcache = { 0, 0, 0 };
static xc_hash_t xc_php_hentry = { 0, 0, 0 };
static xc_hash_t xc_var_hcache = { 0, 0, 0 };
static xc_hash_t xc_var_hentry = { 0, 0, 0 };

static zend_ulong xc_php_ttl    = 0;
static zend_ulong xc_var_maxttl = 0;

enum { xc_deletes_gc_interval = 120 };
static zend_ulong xc_php_gc_interval = 0;
static zend_ulong xc_var_gc_interval = 0;

/* total size */
static zend_ulong xc_php_size  = 0;
static zend_ulong xc_var_size  = 0;

static xc_shm_t *xc_shm = NULL;
static xc_cache_t **xc_php_caches = NULL;
static xc_cache_t **xc_var_caches = NULL;

static zend_bool xc_initized = 0;
static time_t xc_init_time = 0;
static long unsigned xc_init_instance_id = 0;
#ifdef ZTS
static long unsigned xc_init_instance_subid = 0;
#endif
static zend_compile_file_t *origin_compile_file = NULL;
static zend_compile_file_t *old_compile_file = NULL;
static zend_llist_element  *xc_llist_zend_extension = NULL;

static zend_bool xc_test = 0;
static zend_bool xc_readonly_protection = 0;

zend_bool xc_have_op_array_ctor = 0;

ZEND_DECLARE_MODULE_GLOBALS(xcache)

typedef enum { XC_TYPE_PHP, XC_TYPE_VAR } xc_entry_type_t;
/* }}} */

/* any function in *_unlocked is only safe be called within locked (single thread access) area */

static void xc_php_add_unlocked(xc_cache_t *cache, xc_entry_data_php_t *php) /* {{{ */
{
	xc_entry_data_php_t **head = &(cache->phps[php->hvalue]);
	php->next = *head;
	*head = php;
	cache->phps_count ++;
}
/* }}} */
static xc_entry_data_php_t *xc_php_store_unlocked(xc_cache_t *cache, xc_entry_data_php_t *php TSRMLS_DC) /* {{{ */
{
	xc_entry_data_php_t *stored_php;

	php->hits     = 0;
	php->refcount = 0;
	stored_php = xc_processor_store_xc_entry_data_php_t(cache, php TSRMLS_CC);
	if (stored_php) {
		xc_php_add_unlocked(cache, stored_php);
		return stored_php;
	}
	else {
		cache->ooms ++;
		return NULL;
	}
}
/* }}} */
static xc_entry_data_php_t *xc_php_find_unlocked(xc_cache_t *cache, xc_entry_data_php_t *php TSRMLS_DC) /* {{{ */
{
	xc_entry_data_php_t *p;
	for (p = cache->phps[php->hvalue]; p; p = (xc_entry_data_php_t *) p->next) {
		if (memcmp(&php->md5.digest, &p->md5.digest, sizeof(php->md5.digest)) == 0) {
			p->hits ++;
			return p;
		}
	}
	return NULL;
}
/* }}} */
static void xc_php_free_unlocked(xc_cache_t *cache, xc_entry_data_php_t *php) /* {{{ */
{
	cache->mem->handlers->free(cache->mem, (xc_entry_data_php_t *)php);
}
/* }}} */
static void xc_php_addref_unlocked(xc_entry_data_php_t *php) /* {{{ */
{
	php->refcount ++;
}
/* }}} */
static void xc_php_release_unlocked(xc_cache_t *cache, xc_entry_data_php_t *php) /* {{{ */
{
	if (-- php->refcount == 0) {
		xc_entry_data_php_t **pp = &(cache->phps[php->hvalue]);
		xc_entry_data_php_t *p;
		for (p = *pp; p; pp = &(p->next), p = p->next) {
			if (memcmp(&php->md5.digest, &p->md5.digest, sizeof(php->md5.digest)) == 0) {
				/* unlink */
				*pp = p->next;
				xc_php_free_unlocked(cache, php);
				return;
			}
		}
		assert(0);
	}
}
/* }}} */

static inline int xc_entry_equal_unlocked(xc_entry_type_t type, const xc_entry_t *entry1, const xc_entry_t *entry2 TSRMLS_DC) /* {{{ */
{
	/* this function isn't required but can be in unlocked */
	switch (type) {
		case XC_TYPE_PHP:
			{
				const xc_entry_php_t *php_entry1 = (const xc_entry_php_t *) entry1;
				const xc_entry_php_t *php_entry2 = (const xc_entry_php_t *) entry2;
				if (php_entry1->file_inode && php_entry2->file_inode) {
					zend_bool inodeIsSame = php_entry1->file_inode == php_entry2->file_inode
						                 && php_entry1->file_device == php_entry2->file_device;
					if (XG(experimental)) {
						/* new experimental behavior: quick check by inode, first */
						if (!inodeIsSame) {
							return 0;
						}

						/* and then opened_path compare */
					}
					else {
						/* old behavior: inode check only */
						return inodeIsSame;
					}
				}
			}

			assert(IS_ABSOLUTE_PATH(entry1->name.str.val, entry1->name.str.len));
			assert(IS_ABSOLUTE_PATH(entry2->name.str.val, entry2->name.str.len));

			return entry1->name.str.len == entry2->name.str.len
				&& memcmp(entry1->name.str.val, entry2->name.str.val, entry1->name.str.len + 1) == 0;

		case XC_TYPE_VAR:
#ifdef IS_UNICODE
			if (entry1->name_type != entry2->name_type) {
				return 0;
			}

			if (entry1->name_type == IS_UNICODE) {
				return entry1->name.ustr.len == entry2->name.ustr.len
				    && memcmp(entry1->name.ustr.val, entry2->name.ustr.val, (entry1->name.ustr.len + 1) * sizeof(UChar)) == 0;
			}
#endif
			return entry1->name.str.len == entry2->name.str.len
			    && memcmp(entry1->name.str.val, entry2->name.str.val, entry1->name.str.len + 1) == 0;
			break;

		default:
			assert(0);
	}
	return 0;
}
/* }}} */
static inline int xc_entry_has_prefix_unlocked(xc_entry_type_t type, xc_entry_t *entry, zval *prefix) /* {{{ */
{
	/* this function isn't required but can be in unlocked */

#ifdef IS_UNICODE
	if (entry->name_type != prefix->type) {
		return 0;
	}

	if (entry->name_type == IS_UNICODE) {
		if (entry->name.ustr.len < Z_USTRLEN_P(prefix)) {
			return 0;
		}
		return memcmp(entry->name.ustr.val, Z_USTRVAL_P(prefix), Z_USTRLEN_P(prefix) * sizeof(UChar)) == 0;
	}
#endif
	if (prefix->type != IS_STRING) {
		return 0;
	}

	if (entry->name.str.len < Z_STRLEN_P(prefix)) {
		return 0;
	}

	return memcmp(entry->name.str.val, Z_STRVAL_P(prefix), Z_STRLEN_P(prefix)) == 0;
}
/* }}} */
static void xc_entry_add_unlocked(xc_cache_t *cache, xc_hash_value_t entryslotid, xc_entry_t *entry) /* {{{ */
{
	xc_entry_t **head = &(cache->entries[entryslotid]);
	entry->next = *head;
	*head = entry;
	cache->entries_count ++;
}
/* }}} */
static xc_entry_t *xc_entry_store_unlocked(xc_entry_type_t type, xc_cache_t *cache, xc_hash_value_t entryslotid, xc_entry_t *entry TSRMLS_DC) /* {{{ */
{
	xc_entry_t *stored_entry;

	entry->hits  = 0;
	entry->ctime = XG(request_time);
	entry->atime = XG(request_time);
	stored_entry = type == XC_TYPE_PHP
		? (xc_entry_t *) xc_processor_store_xc_entry_php_t(cache, (xc_entry_php_t *) entry TSRMLS_CC)
		: (xc_entry_t *) xc_processor_store_xc_entry_var_t(cache, (xc_entry_var_t *) entry TSRMLS_CC);
	if (stored_entry) {
		xc_entry_add_unlocked(cache, entryslotid, stored_entry);
		++cache->updates;
		return stored_entry;
	}
	else {
		cache->ooms ++;
		return NULL;
	}
}
/* }}} */
static xc_entry_php_t *xc_entry_php_store_unlocked(xc_cache_t *cache, xc_hash_value_t entryslotid, xc_entry_php_t *entry_php TSRMLS_DC) /* {{{ */
{
	return (xc_entry_php_t *) xc_entry_store_unlocked(XC_TYPE_PHP, cache, entryslotid, (xc_entry_t *) entry_php TSRMLS_CC);
}
/* }}} */
static xc_entry_var_t *xc_entry_var_store_unlocked(xc_cache_t *cache, xc_hash_value_t entryslotid, xc_entry_var_t *entry_var TSRMLS_DC) /* {{{ */
{
	return (xc_entry_var_t *) xc_entry_store_unlocked(XC_TYPE_VAR, cache, entryslotid, (xc_entry_t *) entry_var TSRMLS_CC);
}
/* }}} */
static void xc_entry_free_real_unlocked(xc_entry_type_t type, xc_cache_t *cache, volatile xc_entry_t *entry) /* {{{ */
{
	if (type == XC_TYPE_PHP) {
		xc_php_release_unlocked(cache, ((xc_entry_php_t *) entry)->php);
	}
	cache->mem->handlers->free(cache->mem, (xc_entry_t *)entry);
}
/* }}} */
static void xc_entry_free_unlocked(xc_entry_type_t type, xc_cache_t *cache, xc_entry_t *entry TSRMLS_DC) /* {{{ */
{
	cache->entries_count --;
	if ((type == XC_TYPE_PHP ? ((xc_entry_php_t *) entry)->refcount : 0) == 0) {
		xc_entry_free_real_unlocked(type, cache, entry);
	}
	else {
		entry->next = cache->deletes;
		cache->deletes = entry;
		entry->dtime = XG(request_time);
		cache->deletes_count ++;
	}
	return;
}
/* }}} */
static void xc_entry_remove_unlocked(xc_entry_type_t type, xc_cache_t *cache, xc_hash_value_t entryslotid, xc_entry_t *entry TSRMLS_DC) /* {{{ */
{
	xc_entry_t **pp = &(cache->entries[entryslotid]);
	xc_entry_t *p;
	for (p = *pp; p; pp = &(p->next), p = p->next) {
		if (xc_entry_equal_unlocked(type, entry, p TSRMLS_CC)) {
			/* unlink */
			*pp = p->next;
			xc_entry_free_unlocked(type, cache, entry TSRMLS_CC);
			return;
		}
	}
	assert(0);
}
/* }}} */
static xc_entry_t *xc_entry_find_unlocked(xc_entry_type_t type, xc_cache_t *cache, xc_hash_value_t entryslotid, xc_entry_t *entry TSRMLS_DC) /* {{{ */
{
	xc_entry_t *p;
	for (p = cache->entries[entryslotid]; p; p = p->next) {
		if (xc_entry_equal_unlocked(type, entry, p TSRMLS_CC)) {
			zend_bool fresh;
			switch (type) {
			case XC_TYPE_PHP:
				{
					xc_entry_php_t *p_php = (xc_entry_php_t *) p;
					xc_entry_php_t *entry_php = (xc_entry_php_t *) entry;
					fresh = p_php->file_mtime == entry_php->file_mtime && p_php->file_size == entry_php->file_size;
				}
				break;

			case XC_TYPE_VAR:
				{
					fresh = !VAR_ENTRY_EXPIRED(p);
				}
				break;

			default:
				assert(0);
			}

			if (fresh) {
				p->hits ++;
				p->atime = XG(request_time);
				return p;
			}

			xc_entry_remove_unlocked(type, cache, entryslotid, p TSRMLS_CC);
			return NULL;
		}
	}
	return NULL;
}
/* }}} */
static void xc_entry_hold_php_unlocked(xc_cache_t *cache, xc_entry_php_t *entry TSRMLS_DC) /* {{{ */
{
	TRACE("hold %d:%s", entry->file_inode, entry->entry.name.str.val);
	entry->refcount ++;
	xc_stack_push(&XG(php_holds)[cache->cacheid], (void *)entry);
}
/* }}} */
static inline zend_uint advance_wrapped(zend_uint val, zend_uint count) /* {{{ */
{
	if (val + 1 >= count) {
		return 0;
	}
	return val + 1;
}
/* }}} */
static void xc_counters_inc(time_t *curtime, zend_uint *curslot, time_t period, zend_ulong *counters, zend_uint count TSRMLS_DC) /* {{{ */
{
	time_t n = XG(request_time) / period;
	if (*curtime != n) {
		zend_uint target_slot = n % count;
		if (n - *curtime > period) {
			memset(counters, 0, sizeof(counters[0]) * count);
		}
		else {
			zend_uint slot;
			for (slot = advance_wrapped(*curslot, count);
					slot != target_slot;
					slot = advance_wrapped(slot, count)) {
				counters[slot] = 0;
			}
			counters[target_slot] = 0;
		}
		*curtime = n;
		*curslot = target_slot;
	}
	counters[*curslot] ++;
}
/* }}} */
static void xc_cache_hit_unlocked(xc_cache_t *cache TSRMLS_DC) /* {{{ */
{
	cache->hits ++;

	xc_counters_inc(&cache->hits_by_hour_cur_time
			, &cache->hits_by_hour_cur_slot, 60 * 60
			, cache->hits_by_hour
			, sizeof(cache->hits_by_hour) / sizeof(cache->hits_by_hour[0])
			TSRMLS_CC);

	xc_counters_inc(&cache->hits_by_second_cur_time
			, &cache->hits_by_second_cur_slot
			, 1
			, cache->hits_by_second
			, sizeof(cache->hits_by_second) / sizeof(cache->hits_by_second[0])
			TSRMLS_CC);
}
/* }}} */

/* helper function that loop through each entry */
#define XC_ENTRY_APPLY_FUNC(name) zend_bool name(xc_entry_t *entry TSRMLS_DC)
typedef XC_ENTRY_APPLY_FUNC((*cache_apply_unlocked_func_t));
static void xc_entry_apply_unlocked(xc_entry_type_t type, xc_cache_t *cache, cache_apply_unlocked_func_t apply_func TSRMLS_DC) /* {{{ */
{
	xc_entry_t *p, **pp;
	int i, c;

	for (i = 0, c = cache->hentry->size; i < c; i ++) {
		pp = &(cache->entries[i]);
		for (p = *pp; p; p = *pp) {
			if (apply_func(p TSRMLS_CC)) {
				/* unlink */
				*pp = p->next;
				xc_entry_free_unlocked(type, cache, p TSRMLS_CC);
			}
			else {
				pp = &(p->next);
			}
		}
	}
}
/* }}} */

#define XC_CACHE_APPLY_FUNC(name) void name(xc_cache_t *cache TSRMLS_DC)
/* call graph:
 * xc_gc_expires_php -> xc_gc_expires_one -> xc_entry_apply_unlocked -> xc_gc_expires_php_entry_unlocked
 * xc_gc_expires_var -> xc_gc_expires_one -> xc_entry_apply_unlocked -> xc_gc_expires_var_entry_unlocked
 */
static XC_ENTRY_APPLY_FUNC(xc_gc_expires_php_entry_unlocked) /* {{{ */
{
	TRACE("ttl %lu, %lu %lu", (zend_ulong) XG(request_time), (zend_ulong) entry->atime, xc_php_ttl);
	if (XG(request_time) > entry->atime + (time_t) xc_php_ttl) {
		return 1;
	}
	return 0;
}
/* }}} */
static XC_ENTRY_APPLY_FUNC(xc_gc_expires_var_entry_unlocked) /* {{{ */
{
	if (VAR_ENTRY_EXPIRED(entry)) {
		return 1;
	}
	return 0;
}
/* }}} */
static void xc_gc_expires_one(xc_entry_type_t type, xc_cache_t *cache, zend_ulong gc_interval, cache_apply_unlocked_func_t apply_func TSRMLS_DC) /* {{{ */
{
	TRACE("interval %lu, %lu %lu", (zend_ulong) XG(request_time), (zend_ulong) cache->last_gc_expires, gc_interval);
	if (XG(request_time) >= cache->last_gc_expires + (time_t) gc_interval) {
		ENTER_LOCK(cache) {
			if (XG(request_time) >= cache->last_gc_expires + (time_t) gc_interval) {
				cache->last_gc_expires = XG(request_time);
				xc_entry_apply_unlocked(type, cache, apply_func TSRMLS_CC);
			}
		} LEAVE_LOCK(cache);
	}
}
/* }}} */
static void xc_gc_expires_php(TSRMLS_D) /* {{{ */
{
	int i, c;

	if (!xc_php_ttl || !xc_php_gc_interval || !xc_php_caches) {
		return;
	}

	for (i = 0, c = xc_php_hcache.size; i < c; i ++) {
		xc_gc_expires_one(XC_TYPE_PHP, xc_php_caches[i], xc_php_gc_interval, xc_gc_expires_php_entry_unlocked TSRMLS_CC);
	}
}
/* }}} */
static void xc_gc_expires_var(TSRMLS_D) /* {{{ */
{
	int i, c;

	if (!xc_var_gc_interval || !xc_var_caches) {
		return;
	}

	for (i = 0, c = xc_var_hcache.size; i < c; i ++) {
		xc_gc_expires_one(XC_TYPE_VAR, xc_var_caches[i], xc_var_gc_interval, xc_gc_expires_var_entry_unlocked TSRMLS_CC);
	}
}
/* }}} */

static XC_CACHE_APPLY_FUNC(xc_gc_delete_unlocked) /* {{{ */
{
	xc_entry_t *p, **pp;

	pp = &cache->deletes;
	for (p = *pp; p; p = *pp) {
		xc_entry_php_t *entry = (xc_entry_php_t *) p;
		if (XG(request_time) - p->dtime > 3600) {
			entry->refcount = 0;
			/* issue warning here */
		}
		if (entry->refcount == 0) {
			/* unlink */
			*pp = p->next;
			cache->deletes_count --;
			xc_entry_free_real_unlocked(XC_TYPE_PHP, cache, p);
		}
		else {
			pp = &(p->next);
		}
	}
}
/* }}} */
static XC_CACHE_APPLY_FUNC(xc_gc_deletes_one) /* {{{ */
{
	if (cache->deletes && XG(request_time) - cache->last_gc_deletes > xc_deletes_gc_interval) {
		ENTER_LOCK(cache) {
			if (cache->deletes && XG(request_time) - cache->last_gc_deletes > xc_deletes_gc_interval) {
				cache->last_gc_deletes = XG(request_time);
				xc_gc_delete_unlocked(cache TSRMLS_CC);
			}
		} LEAVE_LOCK(cache);
	}
}
/* }}} */
static void xc_gc_deletes(TSRMLS_D) /* {{{ */
{
	int i, c;

	if (xc_php_caches) {
		for (i = 0, c = xc_php_hcache.size; i < c; i ++) {
			xc_gc_deletes_one(xc_php_caches[i] TSRMLS_CC);
		}
	}

	if (xc_var_caches) {
		for (i = 0, c = xc_var_hcache.size; i < c; i ++) {
			xc_gc_deletes_one(xc_var_caches[i] TSRMLS_CC);
		}
	}
}
/* }}} */

/* helper functions for user functions */
static void xc_fillinfo_unlocked(int cachetype, xc_cache_t *cache, zval *return_value TSRMLS_DC) /* {{{ */
{
	zval *blocks, *hits;
	size_t i;
	const xc_block_t *b;
#ifndef NDEBUG
	xc_memsize_t avail = 0;
#endif
	xc_mem_t *mem = cache->mem;
	const xc_mem_handlers_t *handlers = mem->handlers;
	zend_ulong interval;
	if (cachetype == XC_TYPE_PHP) {
		interval = xc_php_ttl ? xc_php_gc_interval : 0;
	}
	else {
		interval = xc_var_gc_interval;
	}

	add_assoc_long_ex(return_value, ZEND_STRS("slots"),     cache->hentry->size);
	add_assoc_long_ex(return_value, ZEND_STRS("compiling"), cache->compiling);
	add_assoc_long_ex(return_value, ZEND_STRS("updates"),   cache->updates);
	add_assoc_long_ex(return_value, ZEND_STRS("misses"),    cache->updates); /* deprecated */
	add_assoc_long_ex(return_value, ZEND_STRS("hits"),      cache->hits);
	add_assoc_long_ex(return_value, ZEND_STRS("clogs"),     cache->clogs);
	add_assoc_long_ex(return_value, ZEND_STRS("ooms"),      cache->ooms);
	add_assoc_long_ex(return_value, ZEND_STRS("errors"),    cache->errors);

	add_assoc_long_ex(return_value, ZEND_STRS("cached"),    cache->entries_count);
	add_assoc_long_ex(return_value, ZEND_STRS("deleted"),   cache->deletes_count);
	if (interval) {
		time_t gc = (cache->last_gc_expires + interval) - XG(request_time);
		add_assoc_long_ex(return_value, ZEND_STRS("gc"),    gc > 0 ? gc : 0);
	}
	else {
		add_assoc_null_ex(return_value, ZEND_STRS("gc"));
	}
	MAKE_STD_ZVAL(hits);
	array_init(hits);
	for (i = 0; i < sizeof(cache->hits_by_hour) / sizeof(cache->hits_by_hour[0]); i ++) {
		add_next_index_long(hits, (long) cache->hits_by_hour[i]);
	}
	add_assoc_zval_ex(return_value, ZEND_STRS("hits_by_hour"), hits);

	MAKE_STD_ZVAL(hits);
	array_init(hits);
	for (i = 0; i < sizeof(cache->hits_by_second) / sizeof(cache->hits_by_second[0]); i ++) {
		add_next_index_long(hits, (long) cache->hits_by_second[i]);
	}
	add_assoc_zval_ex(return_value, ZEND_STRS("hits_by_second"), hits);

	MAKE_STD_ZVAL(blocks);
	array_init(blocks);

	add_assoc_long_ex(return_value, ZEND_STRS("size"),  handlers->size(mem));
	add_assoc_long_ex(return_value, ZEND_STRS("avail"), handlers->avail(mem));
	add_assoc_bool_ex(return_value, ZEND_STRS("can_readonly"), xc_readonly_protection);

	for (b = handlers->freeblock_first(mem); b; b = handlers->freeblock_next(b)) {
		zval *bi;

		MAKE_STD_ZVAL(bi);
		array_init(bi);

		add_assoc_long_ex(bi, ZEND_STRS("size"),   handlers->block_size(b));
		add_assoc_long_ex(bi, ZEND_STRS("offset"), handlers->block_offset(mem, b));
		add_next_index_zval(blocks, bi);
#ifndef NDEBUG
		avail += handlers->block_size(b);
#endif
	}
	add_assoc_zval_ex(return_value, ZEND_STRS("free_blocks"), blocks);
#ifndef NDEBUG
	assert(avail == handlers->avail(mem));
#endif
}
/* }}} */
static void xc_fillentry_unlocked(xc_entry_type_t type, const xc_entry_t *entry, xc_hash_value_t entryslotid, int del, zval *list TSRMLS_DC) /* {{{ */
{
	zval* ei;
	const xc_entry_data_php_t *php;

	ALLOC_INIT_ZVAL(ei);
	array_init(ei);

	add_assoc_long_ex(ei, ZEND_STRS("hits"),     entry->hits);
	add_assoc_long_ex(ei, ZEND_STRS("ctime"),    entry->ctime);
	add_assoc_long_ex(ei, ZEND_STRS("atime"),    entry->atime);
	add_assoc_long_ex(ei, ZEND_STRS("hvalue"),   entryslotid);
	if (del) {
		add_assoc_long_ex(ei, ZEND_STRS("dtime"), entry->dtime);
	}
#ifdef IS_UNICODE
	do {
		zval *zv;
		ALLOC_INIT_ZVAL(zv);
		switch (entry->name_type) {
			case IS_UNICODE:
				ZVAL_UNICODEL(zv, entry->name.ustr.val, entry->name.ustr.len, 1);
				break;
			case IS_STRING:
				ZVAL_STRINGL(zv, entry->name.str.val, entry->name.str.len, 1);
				break;
			default:
				assert(0);
		}
		zv->type = entry->name_type;
		add_assoc_zval_ex(ei, ZEND_STRS("name"), zv);
	} while (0);
#else
	add_assoc_stringl_ex(ei, ZEND_STRS("name"), entry->name.str.val, entry->name.str.len, 1);
#endif
	switch (type) {
		case XC_TYPE_PHP: {
			xc_entry_php_t *entry_php = (xc_entry_php_t *) entry;
			php = entry_php->php;
			add_assoc_long_ex(ei, ZEND_STRS("size"),          entry->size + php->size);
			add_assoc_long_ex(ei, ZEND_STRS("refcount"),      entry_php->refcount);
			add_assoc_long_ex(ei, ZEND_STRS("phprefcount"),   php->refcount);
			add_assoc_long_ex(ei, ZEND_STRS("file_mtime"),    entry_php->file_mtime);
			add_assoc_long_ex(ei, ZEND_STRS("file_size"),     entry_php->file_size);
			add_assoc_long_ex(ei, ZEND_STRS("file_device"),   entry_php->file_device);
			add_assoc_long_ex(ei, ZEND_STRS("file_inode"),    entry_php->file_inode);

#ifdef HAVE_XCACHE_CONSTANT
			add_assoc_long_ex(ei, ZEND_STRS("constinfo_cnt"), php->constinfo_cnt);
#endif
			add_assoc_long_ex(ei, ZEND_STRS("function_cnt"),  php->funcinfo_cnt);
			add_assoc_long_ex(ei, ZEND_STRS("class_cnt"),     php->classinfo_cnt);
#ifdef ZEND_ENGINE_2_1
			add_assoc_long_ex(ei, ZEND_STRS("autoglobal_cnt"),php->autoglobal_cnt);
#endif
			break;
		}

		case XC_TYPE_VAR:
			add_assoc_long_ex(ei, ZEND_STRS("refcount"),      0); /* for BC only */
			add_assoc_long_ex(ei, ZEND_STRS("size"),          entry->size);
			break;

		default:
			assert(0);
	}

	add_next_index_zval(list, ei);
}
/* }}} */
static void xc_filllist_unlocked(xc_entry_type_t type, xc_cache_t *cache, zval *return_value TSRMLS_DC) /* {{{ */
{
	zval* list;
	int i, c;
	xc_entry_t *e;

	ALLOC_INIT_ZVAL(list);
	array_init(list);

	for (i = 0, c = cache->hentry->size; i < c; i ++) {
		for (e = cache->entries[i]; e; e = e->next) {
			xc_fillentry_unlocked(type, e, i, 0, list TSRMLS_CC);
		}
	}
	add_assoc_zval(return_value, "cache_list", list);

	ALLOC_INIT_ZVAL(list);
	array_init(list);
	for (e = cache->deletes; e; e = e->next) {
		xc_fillentry_unlocked(XC_TYPE_PHP, e, 0, 1, list TSRMLS_CC);
	}
	add_assoc_zval(return_value, "deleted_list", list);
}
/* }}} */

static zend_op_array *xc_entry_install(xc_entry_php_t *entry_php TSRMLS_DC) /* {{{ */
{
	zend_uint i;
	xc_entry_data_php_t *p = entry_php->php;
	zend_op_array *old_active_op_array = CG(active_op_array);
#ifndef ZEND_ENGINE_2
	ALLOCA_FLAG(use_heap)
	/* new ptr which is stored inside CG(class_table) */
	xc_cest_t **new_cest_ptrs = (xc_cest_t **)my_do_alloca(sizeof(xc_cest_t*) * p->classinfo_cnt, use_heap);
#endif

	CG(active_op_array) = p->op_array;

#ifdef HAVE_XCACHE_CONSTANT
	/* install constant */
	for (i = 0; i < p->constinfo_cnt; i ++) {
		xc_constinfo_t *ci = &p->constinfos[i];
		xc_install_constant(entry_php->entry.name.str.val, &ci->constant,
				UNISW(0, ci->type), ci->key, ci->key_size, ci->h TSRMLS_CC);
	}
#endif

	/* install function */
	for (i = 0; i < p->funcinfo_cnt; i ++) {
		xc_funcinfo_t  *fi = &p->funcinfos[i];
		xc_install_function(entry_php->entry.name.str.val, &fi->func,
				UNISW(0, fi->type), fi->key, fi->key_size, fi->h TSRMLS_CC);
	}

	/* install class */
	for (i = 0; i < p->classinfo_cnt; i ++) {
		xc_classinfo_t *ci = &p->classinfos[i];
#ifndef ZEND_ENGINE_2
		zend_class_entry *ce = CestToCePtr(ci->cest);
		/* fix pointer to the be which inside class_table */
		if (ce->parent) {
			zend_uint class_idx = (/* class_num */ (int) (long) ce->parent) - 1;
			assert(class_idx < i);
			ci->cest.parent = new_cest_ptrs[class_idx];
		}
		new_cest_ptrs[i] =
#endif
#ifdef ZEND_COMPILE_DELAYED_BINDING
		xc_install_class(entry_php->entry.name.str.val, &ci->cest, -1,
				UNISW(0, ci->type), ci->key, ci->key_size, ci->h TSRMLS_CC);
#else
		xc_install_class(entry_php->entry.name.str.val, &ci->cest, ci->oplineno,
				UNISW(0, ci->type), ci->key, ci->key_size, ci->h TSRMLS_CC);
#endif
	}

#ifdef ZEND_ENGINE_2_1
	/* trigger auto_globals jit */
	for (i = 0; i < p->autoglobal_cnt; i ++) {
		xc_autoglobal_t *aginfo = &p->autoglobals[i];
		zend_u_is_auto_global(aginfo->type, aginfo->key, aginfo->key_len TSRMLS_CC);
	}
#endif
#ifdef XCACHE_ERROR_CACHING
	/* restore trigger errors */
	for (i = 0; i < p->compilererror_cnt; i ++) {
		xc_compilererror_t *error = &p->compilererrors[i];
		CG(zend_lineno) = error->lineno;
		zend_error(error->type, "%s", error->error);
	}
	CG(zend_lineno) = 0;
#endif

	i = 1;
#ifndef ZEND_ENGINE_2_2
	zend_hash_add(&EG(included_files), entry_php->entry.name.str.val, entry_php->entry.name.str.len+1, (void *)&i, sizeof(int), NULL);
#endif

#ifndef ZEND_ENGINE_2
	my_free_alloca(new_cest_ptrs, use_heap);
#endif
	CG(active_op_array) = old_active_op_array;
	return p->op_array;
}
/* }}} */

static inline void xc_entry_unholds_real(xc_stack_t *holds, xc_cache_t **caches, int cachecount TSRMLS_DC) /* {{{ */
{
	int i;
	xc_stack_t *s;
	xc_cache_t *cache;
	xc_entry_php_t *entry_php;

	for (i = 0; i < cachecount; i ++) {
		s = &holds[i];
		TRACE("holded %d items", xc_stack_count(s));
		if (xc_stack_count(s)) {
			cache = caches[i];
			ENTER_LOCK(cache) {
				while (xc_stack_count(s)) {
					entry_php = (xc_entry_php_t *) xc_stack_pop(s);
					TRACE("unhold %d:%s", entry_php->file_inode, entry_php->entry.name.str.val);
					assert(entry_php->refcount > 0);
					--entry_php->refcount;
				}
			} LEAVE_LOCK(cache);
		}
	}
}
/* }}} */
static void xc_entry_unholds(TSRMLS_D) /* {{{ */
{
	if (xc_php_caches) {
		xc_entry_unholds_real(XG(php_holds), xc_php_caches, xc_php_hcache.size TSRMLS_CC);
	}

	if (xc_var_caches) {
		xc_entry_unholds_real(XG(var_holds), xc_var_caches, xc_var_hcache.size TSRMLS_CC);
	}
}
/* }}} */

#define HASH(i) (i)
#define HASH_ZSTR_L(t, s, l) HASH(zend_u_inline_hash_func((t), (s), ((l) + 1) * sizeof(UChar)))
#define HASH_STR_S(s, l) HASH(zend_inline_hash_func((char *) (s), (l)))
#define HASH_STR_L(s, l) HASH_STR_S((s), (l) + 1)
#define HASH_STR(s) HASH_STR_L((s), strlen((s)) + 1)
#define HASH_NUM(n) HASH(n)
static inline xc_hash_value_t xc_hash_fold(xc_hash_value_t hvalue, const xc_hash_t *hasher) /* {{{ fold hash bits as needed */
{
	xc_hash_value_t folded = 0;
	while (hvalue) {
		folded ^= (hvalue & hasher->mask);
		hvalue >>= hasher->bits;
	}
	return folded;
}
/* }}} */
static inline xc_hash_value_t xc_entry_hash_name(xc_entry_t *entry TSRMLS_DC) /* {{{ */
{
	return UNISW(NOTHING, UG(unicode) ? HASH_ZSTR_L(entry->name_type, entry->name.uni.val, entry->name.uni.len) :)
		HASH_STR_L(entry->name.str.val, entry->name.str.len);
}
/* }}} */
#define xc_entry_hash_var xc_entry_hash_name
static void xc_entry_free_key_php(xc_entry_php_t *entry_php TSRMLS_DC) /* {{{ */
{
#define X_FREE(var) do {\
	if (entry_php->var) { \
		efree(entry_php->var); \
	} \
} while (0)
	X_FREE(dirpath);
#ifdef IS_UNICODE
	X_FREE(ufilepath);
	X_FREE(udirpath);
#endif

#undef X_FREE
}
/* }}} */
static char *xc_expand_url(const char *filepath, char *real_path TSRMLS_DC) /* {{{ */
{
	if (strstr(filepath, "://") != NULL) {
		size_t filepath_len = strlen(filepath);
		size_t copy_len = filepath_len > MAXPATHLEN - 1 ? MAXPATHLEN - 1 : filepath_len;
		memcpy(real_path, filepath, filepath_len);
		real_path[copy_len] = '\0';
		return real_path;
	}
	return expand_filepath(filepath, real_path TSRMLS_CC);
}
/* }}} */

#define XC_RESOLVE_PATH_CHECKER(name) zend_bool name(const char *filepath, size_t filepath_len, void *data TSRMLS_DC)
typedef XC_RESOLVE_PATH_CHECKER((*xc_resolve_path_checker_func_t));
static zend_bool xc_resolve_path(const char *filepath, char *path_buffer, xc_resolve_path_checker_func_t checker_func, void *data TSRMLS_DC) /* {{{ */
{
	char *paths, *path;
	char *tokbuf;
	size_t path_buffer_len;
	int size;
	char tokens[] = { DEFAULT_DIR_SEPARATOR, '\0' };
	int ret;
	ALLOCA_FLAG(use_heap)

#if 0
	if ((*filepath == '.' && 
	     (IS_SLASH(filepath[1]) || 
	      ((filepath[1] == '.') && IS_SLASH(filepath[2])))) ||
	    IS_ABSOLUTE_PATH(filepath, strlen(filepath)) ||
	    !path ||
	    !*path) {

		if (checker_func(path_buffer, path_buffer_len, data TSRMLS_CC)) {
			ret = 1;
		}
		else {
			ret = FAILURE;
		}
		goto finish;
	}
#endif

	size = strlen(PG(include_path)) + 1;
	paths = (char *)my_do_alloca(size, use_heap);
	memcpy(paths, PG(include_path), size);

	for (path = php_strtok_r(paths, tokens, &tokbuf); path; path = php_strtok_r(NULL, tokens, &tokbuf)) {
		path_buffer_len = snprintf(path_buffer, MAXPATHLEN, "%s/%s", path, filepath);
		if (path_buffer_len < MAXPATHLEN - 1) {
			if (checker_func(path_buffer, path_buffer_len, data TSRMLS_CC)) {
				ret = 1;
				goto finish;
			}
		}
	}

	/* fall back to current directory */
	if (zend_is_executing(TSRMLS_C)) {
		const char *executed_filename = zend_get_executed_filename(TSRMLS_C);
		if (executed_filename && executed_filename[0] && executed_filename[0] != '[') {
			size_t filename_len = strlen(filepath);
			size_t dirname_len;

			for (dirname_len = strlen(executed_filename) - 1; dirname_len > 0; --dirname_len) {
				if (IS_SLASH(executed_filename[dirname_len])) {
					if (dirname_len + filename_len < MAXPATHLEN - 1) {
						++dirname_len; /* include tailing slash */
						memcpy(path_buffer, executed_filename, dirname_len);
						memcpy(path_buffer + dirname_len, filepath, filename_len);
						path_buffer_len = dirname_len + filename_len;
						path_buffer[path_buffer_len] = '\0';
						if (checker_func(path_buffer, path_buffer_len, data TSRMLS_CC)) {
							ret = 1;
							goto finish;
						}
					}
					break;
				}
			}
		}
	}

	ret = 0;

finish:
	my_free_alloca(paths, use_heap);

	return ret;
}
/* }}} */
#ifndef ZEND_ENGINE_2_3
static XC_RESOLVE_PATH_CHECKER(xc_stat_file) /* {{{ */
{
	return VCWD_STAT(filepath, (struct stat *) data) == 0 ? 1 : 0;
}
/* }}} */
static int xc_resolve_path_stat(const char *filepath, char *path_buffer, struct stat *pbuf TSRMLS_DC) /* {{{ */
{
	return xc_resolve_path(filepath, path_buffer, xc_stat_file, (void *) pbuf TSRMLS_CC)
		? SUCCESS
		: FAILURE;
}
/* }}} */
#endif
typedef struct xc_compiler_t { /* {{{ */
	/* XCache cached compile state */
	const char *filename;
	size_t filename_len;
	const char *opened_path;
	char opened_path_buffer[MAXPATHLEN];

	xc_entry_hash_t entry_hash;
	xc_entry_php_t new_entry;
	xc_entry_data_php_t new_php;
} xc_compiler_t;
/* }}} */
typedef struct xc_entry_resolve_path_data_t { /* {{{ */
	xc_compiler_t *compiler;
	xc_entry_php_t **stored_entry;
} xc_entry_resolve_path_data_t;
/* }}} */
static XC_RESOLVE_PATH_CHECKER(xc_entry_resolve_path_func_unlocked) /* {{{ */
{
	xc_entry_resolve_path_data_t *entry_resolve_path_data = (xc_entry_resolve_path_data_t *) data;
	xc_compiler_t *compiler = entry_resolve_path_data->compiler;

	compiler->new_entry.entry.name.str.val = xc_expand_url(filepath, compiler->opened_path_buffer TSRMLS_CC);
	compiler->new_entry.entry.name.str.len = strlen(compiler->new_entry.entry.name.str.val);

	*entry_resolve_path_data->stored_entry = (xc_entry_php_t *) xc_entry_find_unlocked(
			XC_TYPE_PHP
			, xc_php_caches[compiler->entry_hash.cacheid]
			, compiler->entry_hash.entryslotid
			, (xc_entry_t *) &compiler->new_entry
			TSRMLS_CC);

	return *entry_resolve_path_data->stored_entry ? 1 : 0;
}
/* }}} */
static int xc_entry_resolve_path_unlocked(xc_compiler_t *compiler, const char *filepath, xc_entry_php_t **stored_entry TSRMLS_DC) /* {{{ */
{
	char path_buffer[MAXPATHLEN];
	xc_entry_resolve_path_data_t entry_resolve_path_data;
	entry_resolve_path_data.compiler = compiler;
	entry_resolve_path_data.stored_entry = stored_entry;

	return xc_resolve_path(filepath, path_buffer, xc_entry_resolve_path_func_unlocked, (void *) &entry_resolve_path_data TSRMLS_CC)
		? SUCCESS
		: FAILURE;
}
/* }}} */
static int xc_entry_php_quick_resolve_opened_path(xc_compiler_t *compiler, struct stat *statbuf TSRMLS_DC) /* {{{ */
{
	if (strcmp(SG(request_info).path_translated, compiler->filename) == 0) {
		/* sapi has already done this stat() for us */
		if (statbuf) {
			struct stat *sapi_stat = sapi_get_stat(TSRMLS_C);
			if (!sapi_stat) {
				goto giveupsapistat;
			}
			*statbuf = *sapi_stat;
		}

		compiler->opened_path = xc_expand_url(compiler->filename, compiler->opened_path_buffer TSRMLS_CC);
		return SUCCESS;
	}
giveupsapistat:

	/* absolute path */
	if (IS_ABSOLUTE_PATH(compiler->filename, strlen(compiler->filename))) {
		if (statbuf && VCWD_STAT(compiler->filename, statbuf) != 0) {
			return FAILURE;
		}
		compiler->opened_path = xc_expand_url(compiler->filename, compiler->opened_path_buffer TSRMLS_CC);
		return SUCCESS;
	}

	/* relative path */
	if (*compiler->filename == '.' && (IS_SLASH(compiler->filename[1]) || compiler->filename[1] == '.')) {
		const char *ptr = compiler->filename + 1;
		if (*ptr == '.') {
			while (*(++ptr) == '.');
			if (!IS_SLASH(*ptr)) {
				return FAILURE;
			}   
		}

		if (statbuf && VCWD_STAT(compiler->filename, statbuf) != 0) {
			return FAILURE;
		}

		compiler->opened_path = xc_expand_url(compiler->filename, compiler->opened_path_buffer TSRMLS_CC);
		return SUCCESS;
	}

	return FAILURE;
}
/* }}} */
static int xc_entry_php_resolve_opened_path(xc_compiler_t *compiler, struct stat *statbuf TSRMLS_DC) /* {{{ */
{
	if (xc_entry_php_quick_resolve_opened_path(compiler, statbuf TSRMLS_CC) == SUCCESS) {
		/* opened_path resolved */
		return SUCCESS;
	}
	/* fall back to real stat call */
	else {
#ifdef ZEND_ENGINE_2_3
		char *opened_path = php_resolve_path(compiler->filename, compiler->filename_len, PG(include_path) TSRMLS_CC);
		if (opened_path) {
			strcpy(compiler->opened_path_buffer, opened_path);
			efree(opened_path);
			compiler->opened_path = compiler->opened_path_buffer;
			if (!statbuf || VCWD_STAT(compiler->opened_path, statbuf) == 0) {
				return SUCCESS;
			}
		}
#else
		char path_buffer[MAXPATHLEN];
		if (xc_resolve_path_stat(compiler->filename, path_buffer, statbuf TSRMLS_CC) == SUCCESS) {
			compiler->opened_path = xc_expand_url(path_buffer, compiler->opened_path_buffer TSRMLS_CC);
			return SUCCESS;
		}
#endif
	}
	return FAILURE;
}
/* }}} */
static int xc_entry_php_init_key(xc_compiler_t *compiler TSRMLS_DC) /* {{{ */
{
	if (XG(stat)) {
		struct stat buf;
		time_t delta;

		if (compiler->opened_path) {
			if (VCWD_STAT(compiler->opened_path, &buf) != 0) {
				return FAILURE;
			}
		}
		else {
			if (xc_entry_php_resolve_opened_path(compiler, &buf TSRMLS_CC) != SUCCESS) {
				return FAILURE;
			}
		}

		delta = XG(request_time) - buf.st_mtime;
		if (abs(delta) < 2 && !xc_test) {
			return FAILURE;
		}

		compiler->new_entry.file_mtime   = buf.st_mtime;
		compiler->new_entry.file_size    = buf.st_size;
		compiler->new_entry.file_device  = buf.st_dev;
		compiler->new_entry.file_inode   = buf.st_ino;
	}
	else {
		xc_entry_php_quick_resolve_opened_path(compiler, NULL TSRMLS_CC);

		compiler->new_entry.file_mtime   = 0;
		compiler->new_entry.file_size    = 0;
		compiler->new_entry.file_device  = 0;
		compiler->new_entry.file_inode   = 0;
	}

	{
		xc_hash_value_t basename_hash_value;
		if (xc_php_hcache.size > 1
		 || !compiler->new_entry.file_inode) {
			const char *filename_end = compiler->filename + compiler->filename_len;
			const char *basename = filename_end - 1;

			/* scan till out of basename part */
			while (basename >= compiler->filename && !IS_SLASH(*basename)) {
				--basename;
			}
			/* get back to basename */
			++basename;

			basename_hash_value = HASH_STR_L(basename, filename_end - basename);
		}

		compiler->entry_hash.cacheid = xc_php_hcache.size > 1 ? xc_hash_fold(basename_hash_value, &xc_php_hcache) : 0;
		compiler->entry_hash.entryslotid = xc_hash_fold(
				compiler->new_entry.file_inode
				? (xc_hash_value_t) HASH(compiler->new_entry.file_device + compiler->new_entry.file_inode)
				: basename_hash_value
				, &xc_php_hentry);
	}

	compiler->new_entry.filepath  = NULL;
	compiler->new_entry.dirpath   = NULL;
#ifdef IS_UNICODE
	compiler->new_entry.ufilepath = NULL;
	compiler->new_entry.udirpath  = NULL;
#endif

	return SUCCESS;
}
/* }}} */
static inline xc_hash_value_t xc_php_hash_md5(xc_entry_data_php_t *php TSRMLS_DC) /* {{{ */
{
	return HASH_STR_S(php->md5.digest, sizeof(php->md5.digest));
}
/* }}} */
static int xc_entry_data_php_init_md5(xc_cache_t *cache, xc_compiler_t *compiler TSRMLS_DC) /* {{{ */
{
	unsigned char   buf[1024];
	PHP_MD5_CTX     context;
	int             n;
	php_stream     *stream;
	ulong           old_rsid = EG(regular_list).nNextFreeElement;

	stream = php_stream_open_wrapper((char *) compiler->filename, "rb", USE_PATH | REPORT_ERRORS | ENFORCE_SAFE_MODE | STREAM_DISABLE_OPEN_BASEDIR, NULL);
	if (!stream) {
		return FAILURE;
	}

	PHP_MD5Init(&context);
	while ((n = php_stream_read(stream, (char *) buf, sizeof(buf))) > 0) {
		PHP_MD5Update(&context, buf, n);
	}
	PHP_MD5Final((unsigned char *) compiler->new_php.md5.digest, &context);

	php_stream_close(stream);
	if (EG(regular_list).nNextFreeElement == old_rsid + 1) {
		EG(regular_list).nNextFreeElement = old_rsid;
	}

	if (n < 0) {
		return FAILURE;
	}

	compiler->new_php.hvalue = (xc_php_hash_md5(&compiler->new_php TSRMLS_CC) & cache->hphp->mask);
#ifdef XCACHE_DEBUG
	{
		char md5str[33];
		make_digest(md5str, (unsigned char *) compiler->new_php.md5.digest);
		TRACE("md5 %s", md5str);
	}
#endif

	return SUCCESS;
}
/* }}} */
static void xc_entry_php_init(xc_entry_php_t *entry_php, const char *filepath TSRMLS_DC) /* {{{*/
{
	entry_php->filepath     = ZEND_24((char *), NOTHING) filepath;
	entry_php->filepath_len = strlen(entry_php->filepath);
	entry_php->dirpath      = estrndup(entry_php->filepath, entry_php->filepath_len);
	entry_php->dirpath_len  = zend_dirname(entry_php->dirpath, entry_php->filepath_len);
#ifdef IS_UNICODE
	zend_string_to_unicode(ZEND_U_CONVERTER(UG(runtime_encoding_conv)), &entry_php->ufilepath, &entry_php->ufilepath_len, entry_php->filepath, entry_php->filepath_len TSRMLS_CC);
	zend_string_to_unicode(ZEND_U_CONVERTER(UG(runtime_encoding_conv)), &entry_php->udirpath,  &entry_php->udirpath_len,  entry_php->dirpath,  entry_php->dirpath_len TSRMLS_CC);
#endif
}
/* }}} */
#ifndef ZEND_COMPILE_DELAYED_BINDING
static void xc_cache_early_binding_class_cb(zend_op *opline, int oplineno, void *data TSRMLS_DC) /* {{{ */
{
	char *class_name;
	zend_uint i;
	int class_len;
	xc_cest_t cest;
	xc_entry_data_php_t *php = (xc_entry_data_php_t *) data;

	class_name = Z_OP_CONSTANT(opline->op1).value.str.val;
	class_len  = Z_OP_CONSTANT(opline->op1).value.str.len;
	if (zend_hash_find(CG(class_table), class_name, class_len, (void **) &cest) == FAILURE) {
		assert(0);
	}
	TRACE("got ZEND_DECLARE_INHERITED_CLASS: %s", class_name + 1);
	/* let's see which class */
	for (i = 0; i < php->classinfo_cnt; i ++) {
		if (memcmp(ZSTR_S(php->classinfos[i].key), class_name, class_len) == 0) {
			php->classinfos[i].oplineno = oplineno;
			php->have_early_binding = 1;
			break;
		}
	}

	if (i == php->classinfo_cnt) {
		assert(0);
	}
}
/* }}} */
#endif

/* {{{ Constant Usage */
#ifdef ZEND_ENGINE_2_4
#	define xcache_literal_is_dir  1
#	define xcache_literal_is_file 2
#else
#	define xcache_op1_is_file 1
#	define xcache_op1_is_dir  2
#	define xcache_op2_is_file 4
#	define xcache_op2_is_dir  8
#endif
typedef struct {
	zend_bool filepath_used;
	zend_bool dirpath_used;
	zend_bool ufilepath_used;
	zend_bool udirpath_used;
} xc_const_usage_t;
/* }}} */
static void xc_collect_op_array_info(xc_compiler_t *compiler, xc_const_usage_t *usage, xc_op_array_info_t *op_array_info, zend_op_array *op_array TSRMLS_DC) /* {{{ */
{
#ifdef ZEND_ENGINE_2_4
	int literalindex;
#else
	zend_uint oplinenum;
#endif
	xc_vector_t details;

	xc_vector_init(xc_op_array_info_detail_t, &details);

#define XCACHE_ANALYZE_LITERAL(type) \
	if (zend_binary_strcmp(Z_STRVAL(literal->constant), Z_STRLEN(literal->constant), compiler->new_entry.type##path, compiler->new_entry.type##path_len) == 0) { \
		usage->type##path_used = 1; \
		literalinfo |= xcache_##literal##_is_##type; \
	}

#define XCACHE_U_ANALYZE_LITERAL(type) \
	if (zend_u_##binary_strcmp(Z_USTRVAL(literal->constant), Z_USTRLEN(literal->constant), compiler->new_entry.u##type##path, compiler->new_entry.u##type##path_len) == 0) { \
		usage->u##type##path_used = 1; \
		literalinfo |= xcache_##literal##_is_##type; \
	}

#define XCACHE_ANALYZE_OP(type, op) \
	if (zend_binary_strcmp(Z_STRVAL(Z_OP_CONSTANT(opline->op)), Z_STRLEN(Z_OP_CONSTANT(opline->op)), compiler->new_entry.type##path, compiler->new_entry.type##path_len) == 0) { \
		usage->type##path_used = 1; \
		oplineinfo |= xcache_##op##_is_##type; \
	}

#define XCACHE_U_ANALYZE_OP(type, op) \
	if (zend_u_##binary_strcmp(Z_USTRVAL(Z_OP_CONSTANT(opline->op)), Z_USTRLEN(Z_OP_CONSTANT(opline->op)), compiler->new_entry.u##type##path, compiler->new_entry.u##type##path_len) == 0) { \
		usage->u##type##path_used = 1; \
		oplineinfo |= xcache_##op##_is_##type; \
	}

#ifdef ZEND_ENGINE_2_4
	for (literalindex = 0; literalindex < op_array->last_literal; literalindex++) {
		zend_literal *literal = &op_array->literals[literalindex];
		zend_uint literalinfo = 0;
		if (Z_TYPE(literal->constant) == IS_STRING) {
			XCACHE_ANALYZE_LITERAL(file)
			else XCACHE_ANALYZE_LITERAL(dir)
		}
#ifdef IS_UNICODE
		else if (Z_TYPE(literal->constant) == IS_UNICODE) {
			XCACHE_U_ANALYZE_LITERAL(file)
			else XCACHE_U_ANALYZE_LITERAL(dir)
		}
#endif
		if (literalinfo) {
			xc_op_array_info_detail_t detail;
			detail.index = literalindex;
			detail.info  = literalinfo;
			xc_vector_add(xc_op_array_info_detail_t, &details, detail);
		}
	}

	op_array_info->literalinfo_cnt = details.cnt;
	op_array_info->literalinfos    = xc_vector_detach(xc_op_array_info_detail_t, &details);
#else /* ZEND_ENGINE_2_4 */
	for (oplinenum = 0; oplinenum < op_array->last; oplinenum++) {
		zend_op *opline = &op_array->opcodes[oplinenum];
		zend_uint oplineinfo = 0;
		if (Z_OP_TYPE(opline->op1) == IS_CONST) {
			if (Z_TYPE(Z_OP_CONSTANT(opline->op1)) == IS_STRING) {
				XCACHE_ANALYZE_OP(file, op1)
				else XCACHE_ANALYZE_OP(dir, op1)
			}
#ifdef IS_UNICODE
			else if (Z_TYPE(Z_OP_CONSTANT(opline->op1)) == IS_UNICODE) {
				XCACHE_U_ANALYZE_OP(file, op1)
				else XCACHE_U_ANALYZE_OP(dir, op1)
			}
#endif
		}

		if (Z_OP_TYPE(opline->op2) == IS_CONST) {
			if (Z_TYPE(Z_OP_CONSTANT(opline->op2)) == IS_STRING) {
				XCACHE_ANALYZE_OP(file, op2)
				else XCACHE_ANALYZE_OP(dir, op2)
			}
#ifdef IS_UNICODE
			else if (Z_TYPE(Z_OP_CONSTANT(opline->op2)) == IS_UNICODE) {
				XCACHE_U_ANALYZE_OP(file, op2)
				else XCACHE_U_ANALYZE_OP(dir, op2)
			}
#endif
		}

		if (oplineinfo) {
			xc_op_array_info_detail_t detail;
			detail.index = oplinenum;
			detail.info  = oplineinfo;
			xc_vector_add(xc_op_array_info_detail_t, &details, detail);
		}
	}

	op_array_info->oplineinfo_cnt = details.cnt;
	op_array_info->oplineinfos    = xc_vector_detach(xc_op_array_info_detail_t, &details);
#endif /* ZEND_ENGINE_2_4 */
	xc_vector_free(xc_op_array_info_detail_t, &details);
}
/* }}} */
void xc_fix_op_array_info(const xc_entry_php_t *entry_php, const xc_entry_data_php_t *php, zend_op_array *op_array, int shallow_copy, const xc_op_array_info_t *op_array_info TSRMLS_DC) /* {{{ */
{
#ifdef ZEND_ENGINE_2_4
	zend_uint literalinfoindex;

	for (literalinfoindex = 0; literalinfoindex < op_array_info->literalinfo_cnt; ++literalinfoindex) {
		int literalindex = op_array_info->literalinfos[literalinfoindex].index;
		int literalinfo = op_array_info->literalinfos[literalinfoindex].info;
		zend_literal *literal = &op_array->literals[literalindex];
		if ((literalinfo & xcache_literal_is_file)) {
			if (!shallow_copy) {
				efree(Z_STRVAL(literal->constant));
			}
			if (Z_TYPE(literal->constant) == IS_STRING) {
				assert(entry_php->filepath);
				ZVAL_STRINGL(&literal->constant, entry_php->filepath, entry_php->filepath_len, !shallow_copy);
				TRACE("restored literal constant: %s", entry_php->filepath);
			}
#ifdef IS_UNICODE
			else if (Z_TYPE(literal->constant) == IS_UNICODE) {
				assert(entry_php->ufilepath);
				ZVAL_UNICODEL(&literal->constant, entry_php->ufilepath, entry_php->ufilepath_len, !shallow_copy);
			}
#endif
			else {
				assert(0);
			}
		}
		else if ((literalinfo & xcache_literal_is_dir)) {
			if (!shallow_copy) {
				efree(Z_STRVAL(literal->constant));
			}
			if (Z_TYPE(literal->constant) == IS_STRING) {
				assert(entry_php->dirpath);
				TRACE("restored literal constant: %s", entry_php->dirpath);
				ZVAL_STRINGL(&literal->constant, entry_php->dirpath, entry_php->dirpath_len, !shallow_copy);
			}
#ifdef IS_UNICODE
			else if (Z_TYPE(literal->constant) == IS_UNICODE) {
				assert(!entry_php->udirpath);
				ZVAL_UNICODEL(&literal->constant, entry_php->udirpath, entry_php->udirpath_len, !shallow_copy);
			}
#endif
			else {
				assert(0);
			}
		}
	}
#else /* ZEND_ENGINE_2_4 */
	zend_uint oplinenum;

	for (oplinenum = 0; oplinenum < op_array_info->oplineinfo_cnt; ++oplinenum) {
		int oplineindex = op_array_info->oplineinfos[oplinenum].index;
		int oplineinfo = op_array_info->oplineinfos[oplinenum].info;
		zend_op *opline = &op_array->opcodes[oplineindex];
		if ((oplineinfo & xcache_op1_is_file)) {
			assert(Z_OP_TYPE(opline->op1) == IS_CONST);
			if (!shallow_copy) {
				efree(Z_STRVAL(Z_OP_CONSTANT(opline->op1)));
			}
			if (Z_TYPE(Z_OP_CONSTANT(opline->op1)) == IS_STRING) {
				assert(entry_php->filepath);
				ZVAL_STRINGL(&Z_OP_CONSTANT(opline->op1), entry_php->filepath, entry_php->filepath_len, !shallow_copy);
				TRACE("restored op1 constant: %s", entry_php->filepath);
			}
#ifdef IS_UNICODE
			else if (Z_TYPE(Z_OP_CONSTANT(opline->op1)) == IS_UNICODE) {
				assert(entry_php->ufilepath);
				ZVAL_UNICODEL(&Z_OP_CONSTANT(opline->op1), entry_php->ufilepath, entry_php->ufilepath_len, !shallow_copy);
			}
#endif
			else {
				assert(0);
			}
		}
		else if ((oplineinfo & xcache_op1_is_dir)) {
			assert(Z_OP_TYPE(opline->op1) == IS_CONST);
			if (!shallow_copy) {
				efree(Z_STRVAL(Z_OP_CONSTANT(opline->op1)));
			}
			if (Z_TYPE(Z_OP_CONSTANT(opline->op1)) == IS_STRING) {
				assert(entry_php->dirpath);
				TRACE("restored op1 constant: %s", entry_php->dirpath);
				ZVAL_STRINGL(&Z_OP_CONSTANT(opline->op1), entry_php->dirpath, entry_php->dirpath_len, !shallow_copy);
			}
#ifdef IS_UNICODE
			else if (Z_TYPE(Z_OP_CONSTANT(opline->op1)) == IS_UNICODE) {
				assert(!entry_php->udirpath);
				ZVAL_UNICODEL(&Z_OP_CONSTANT(opline->op1), entry_php->udirpath, entry_php->udirpath_len, !shallow_copy);
			}
#endif
			else {
				assert(0);
			}
		}

		if ((oplineinfo & xcache_op2_is_file)) {
			assert(Z_OP_TYPE(opline->op2) == IS_CONST);
			if (!shallow_copy) {
				efree(Z_STRVAL(Z_OP_CONSTANT(opline->op2)));
			}
			if (Z_TYPE(Z_OP_CONSTANT(opline->op2)) == IS_STRING) {
				assert(entry_php->filepath);
				TRACE("restored op2 constant: %s", entry_php->filepath);
				ZVAL_STRINGL(&Z_OP_CONSTANT(opline->op2), entry_php->filepath, entry_php->filepath_len, !shallow_copy);
			}
#ifdef IS_UNICODE
			else if (Z_TYPE(Z_OP_CONSTANT(opline->op2)) == IS_UNICODE) {
				assert(entry_php->ufilepath);
				ZVAL_UNICODEL(&Z_OP_CONSTANT(opline->op2), entry_php->ufilepath, entry_php->ufilepath_len, !shallow_copy);
			}
#endif
			else {
				assert(0);
			}
		}
		else if ((oplineinfo & xcache_op2_is_dir)) {
			assert(Z_OP_TYPE(opline->op2) == IS_CONST);
			if (!shallow_copy) {
				efree(Z_STRVAL(Z_OP_CONSTANT(opline->op2)));
			}
			if (Z_TYPE(Z_OP_CONSTANT(opline->op2)) == IS_STRING) {
				assert(entry_php->dirpath);
				TRACE("restored op2 constant: %s", entry_php->dirpath);
				ZVAL_STRINGL(&Z_OP_CONSTANT(opline->op2), entry_php->dirpath, entry_php->dirpath_len, !shallow_copy);
			}
#ifdef IS_UNICODE
			else if (Z_TYPE(Z_OP_CONSTANT(opline->op2)) == IS_UNICODE) {
				assert(entry_php->udirpath);
				ZVAL_UNICODEL(&Z_OP_CONSTANT(opline->op2), entry_php->udirpath, entry_php->udirpath_len, !shallow_copy);
			}
#endif
			else {
				assert(0);
			}
		}
	}
#endif /* ZEND_ENGINE_2_4 */
}
/* }}} */
static void xc_free_op_array_info(xc_op_array_info_t *op_array_info TSRMLS_DC) /* {{{ */
{
#ifdef ZEND_ENGINE_2_4
	if (op_array_info->literalinfos) {
		efree(op_array_info->literalinfos);
	}
#else
	if (op_array_info->oplineinfos) {
		efree(op_array_info->oplineinfos);
	}
#endif
}
/* }}} */
static void xc_free_php(xc_entry_data_php_t *php TSRMLS_DC) /* {{{ */
{
	zend_uint i;
	if (php->classinfos) {
		for (i = 0; i < php->classinfo_cnt; i ++) {
			xc_classinfo_t *classinfo = &php->classinfos[i];
			zend_uint j;

			for (j = 0; j < classinfo->methodinfo_cnt; j ++) {
				xc_free_op_array_info(&classinfo->methodinfos[j] TSRMLS_CC);
			}

			if (classinfo->methodinfos) {
				efree(classinfo->methodinfos);
			}
		}
	}
	if (php->funcinfos) {
		for (i = 0; i < php->funcinfo_cnt; i ++) {
			xc_free_op_array_info(&php->funcinfos[i].op_array_info TSRMLS_CC);
		}
	}
	xc_free_op_array_info(&php->op_array_info TSRMLS_CC);

#define X_FREE(var) do {\
	if (php->var) { \
		efree(php->var); \
	} \
} while (0)

#ifdef ZEND_ENGINE_2_1
	X_FREE(autoglobals);
#endif
	X_FREE(classinfos);
	X_FREE(funcinfos);
#ifdef HAVE_XCACHE_CONSTANT
	X_FREE(constinfos);
#endif
#undef X_FREE
}
/* }}} */
static void xc_compile_php(xc_compiler_t *compiler, zend_file_handle *h, int type TSRMLS_DC) /* {{{ */
{
	zend_uint old_constinfo_cnt, old_funcinfo_cnt, old_classinfo_cnt;
	zend_bool catched = 0;

	/* {{{ compile */
	TRACE("compiling %s", h->opened_path ? h->opened_path : h->filename);

	old_classinfo_cnt = zend_hash_num_elements(CG(class_table));
	old_funcinfo_cnt  = zend_hash_num_elements(CG(function_table));
	old_constinfo_cnt = zend_hash_num_elements(EG(zend_constants));

	zend_try {
		compiler->new_php.op_array = old_compile_file(h, type TSRMLS_CC);
	} zend_catch {
		catched = 1;
	} zend_end_try();

	if (catched) {
		goto err_bailout;
	}

	if (compiler->new_php.op_array == NULL) {
		goto err_op_array;
	}

	if (!XG(initial_compile_file_called)) {
		return;
	}

	/* }}} */
	/* {{{ prepare */
	zend_restore_compiled_filename(h->opened_path ? h->opened_path : (char *) h->filename TSRMLS_CC);

#ifdef HAVE_XCACHE_CONSTANT
	compiler->new_php.constinfo_cnt  = zend_hash_num_elements(EG(zend_constants)) - old_constinfo_cnt;
#endif
	compiler->new_php.funcinfo_cnt   = zend_hash_num_elements(CG(function_table)) - old_funcinfo_cnt;
	compiler->new_php.classinfo_cnt  = zend_hash_num_elements(CG(class_table))    - old_classinfo_cnt;
#ifdef ZEND_ENGINE_2_1
	/* {{{ count new_php.autoglobal_cnt */ {
		Bucket *b;

		compiler->new_php.autoglobal_cnt = 0;
		for (b = CG(auto_globals)->pListHead; b != NULL; b = b->pListNext) {
			zend_auto_global *auto_global = (zend_auto_global *) b->pData;
			/* check if actived */
			if (auto_global->auto_global_callback && !auto_global->armed) {
				compiler->new_php.autoglobal_cnt ++;
			}
		}
	}
	/* }}} */
#endif

#define X_ALLOC_N(var, cnt) do {     \
	if (compiler->new_php.cnt) {                  \
		ECALLOC_N(compiler->new_php.var, compiler->new_php.cnt); \
		if (!compiler->new_php.var) {             \
			goto err_alloc;          \
		}                            \
	}                                \
	else {                           \
		compiler->new_php.var = NULL;             \
	}                                \
} while (0)

#ifdef HAVE_XCACHE_CONSTANT
	X_ALLOC_N(constinfos,  constinfo_cnt);
#endif
	X_ALLOC_N(funcinfos,   funcinfo_cnt);
	X_ALLOC_N(classinfos,  classinfo_cnt);
#ifdef ZEND_ENGINE_2_1
	X_ALLOC_N(autoglobals, autoglobal_cnt);
#endif
#undef X_ALLOC
	/* }}} */

	/* {{{ shallow copy, pointers only */ {
		Bucket *b;
		zend_uint i;
		zend_uint j;

#define COPY_H(vartype, var, cnt, name, datatype) do {        \
	for (i = 0, j = 0; b; i ++, b = b->pListNext) {           \
		vartype *data = &compiler->new_php.var[j];                         \
                                                              \
		if (i < old_##cnt) {                                  \
			continue;                                         \
		}                                                     \
		j ++;                                                 \
                                                              \
		assert(i < old_##cnt + compiler->new_php.cnt);                     \
		assert(b->pData);                                     \
		memcpy(&data->name, b->pData, sizeof(datatype));      \
		UNISW(NOTHING, data->type = b->key.type;)             \
		if (UNISW(1, b->key.type == IS_STRING)) {             \
			ZSTR_S(data->key)      = BUCKET_KEY_S(b);         \
		}                                                     \
		else {                                                \
			ZSTR_U(data->key)      = BUCKET_KEY_U(b);         \
		}                                                     \
		data->key_size   = b->nKeyLength;                     \
		data->h          = b->h;                              \
	}                                                         \
} while(0)

#ifdef HAVE_XCACHE_CONSTANT
		b = EG(zend_constants)->pListHead; COPY_H(xc_constinfo_t, constinfos, constinfo_cnt, constant, zend_constant);
#endif
		b = CG(function_table)->pListHead; COPY_H(xc_funcinfo_t,  funcinfos,  funcinfo_cnt,  func,     zend_function);
		b = CG(class_table)->pListHead;    COPY_H(xc_classinfo_t, classinfos, classinfo_cnt, cest,     xc_cest_t);

#undef COPY_H

		/* for ZE1, cest need to be fixed inside store */

#ifdef ZEND_ENGINE_2_1
		/* scan for acatived auto globals */
		i = 0;
		for (b = CG(auto_globals)->pListHead; b != NULL; b = b->pListNext) {
			zend_auto_global *auto_global = (zend_auto_global *) b->pData;
			/* check if actived */
			if (auto_global->auto_global_callback && !auto_global->armed) {
				xc_autoglobal_t *data = &compiler->new_php.autoglobals[i];

				assert(i < compiler->new_php.autoglobal_cnt);
				i ++;
				UNISW(NOTHING, data->type = b->key.type;)
				if (UNISW(1, b->key.type == IS_STRING)) {
					ZSTR_S(data->key)     = BUCKET_KEY_S(b);
				}
				else {
					ZSTR_U(data->key)     = BUCKET_KEY_U(b);
				}
				data->key_len = b->nKeyLength - 1;
				data->h       = b->h;
			}
		}
#endif
	}
	/* }}} */

	/* {{{ collect info for file/dir path */ {
		Bucket *b;
		xc_const_usage_t const_usage;
		unsigned int i;

		xc_entry_php_init(&compiler->new_entry, zend_get_compiled_filename(TSRMLS_C) TSRMLS_CC);
		memset(&const_usage, 0, sizeof(const_usage));

		for (i = 0; i < compiler->new_php.classinfo_cnt; i ++) {
			xc_classinfo_t *classinfo = &compiler->new_php.classinfos[i];
			zend_class_entry *ce = CestToCePtr(classinfo->cest);
			classinfo->methodinfo_cnt = ce->function_table.nTableSize;
			if (classinfo->methodinfo_cnt) {
				int j;

				ECALLOC_N(classinfo->methodinfos, classinfo->methodinfo_cnt);
				if (!classinfo->methodinfos) {
					goto err_alloc;
				}

				for (j = 0, b = ce->function_table.pListHead; b; j ++, b = b->pListNext) {
					xc_collect_op_array_info(compiler, &const_usage, &classinfo->methodinfos[j], (zend_op_array *) b->pData TSRMLS_CC);
				}
			}
			else {
				classinfo->methodinfos = NULL;
			}
		}

		for (i = 0; i < compiler->new_php.funcinfo_cnt; i ++) {
			xc_collect_op_array_info(compiler, &const_usage, &compiler->new_php.funcinfos[i].op_array_info, (zend_op_array *) &compiler->new_php.funcinfos[i].func TSRMLS_CC);
		}

		xc_collect_op_array_info(compiler, &const_usage, &compiler->new_php.op_array_info, compiler->new_php.op_array TSRMLS_CC);

		/* file/dir path free unused */
#define X_FREE_UNUSED(var) \
		if (!const_usage.var##path_used) { \
			efree(compiler->new_entry.var##path); \
			compiler->new_entry.var##path = NULL; \
			compiler->new_entry.var##path_len = 0; \
		}
		/* filepath is required to restore op_array->filename, so no free filepath here */
		X_FREE_UNUSED(dir)
#ifdef IS_UNICODE
		X_FREE_UNUSED(ufile)
		X_FREE_UNUSED(udir)
#endif
#undef X_FREE_UNUSED
	}
	/* }}} */
#ifdef XCACHE_ERROR_CACHING
	compiler->new_php.compilererrors = xc_sandbox_compilererrors(TSRMLS_C);
	compiler->new_php.compilererror_cnt = xc_sandbox_compilererror_cnt(TSRMLS_C);
#endif
#ifndef ZEND_COMPILE_DELAYED_BINDING
	/* {{{ find inherited classes that should be early-binding */
	compiler->new_php.have_early_binding = 0;
	{
		zend_uint i;
		for (i = 0; i < compiler->new_php.classinfo_cnt; i ++) {
			compiler->new_php.classinfos[i].oplineno = -1;
		}
	}

	xc_undo_pass_two(compiler->new_php.op_array TSRMLS_CC);
	xc_foreach_early_binding_class(compiler->new_php.op_array, xc_cache_early_binding_class_cb, (void *) &compiler->new_php TSRMLS_CC);
	xc_redo_pass_two(compiler->new_php.op_array TSRMLS_CC);
	/* }}} */
#endif

	return;

err_alloc:
	xc_free_php(&compiler->new_php TSRMLS_CC);

err_bailout:
err_op_array:

	if (catched) {
		zend_bailout();
	}
}
/* }}} */
static zend_op_array *xc_compile_restore(xc_entry_php_t *stored_entry, xc_entry_data_php_t *stored_php TSRMLS_DC) /* {{{ */
{
	zend_op_array *op_array;
	xc_entry_php_t restored_entry;
	xc_entry_data_php_t restored_php;
	zend_bool catched;
	zend_uint i;

	/* still needed because in zend_language_scanner.l, require()/include() check file_handle.handle.stream.handle */
	i = 1;
	zend_hash_add(&EG(included_files), stored_entry->entry.name.str.val, stored_entry->entry.name.str.len + 1, (void *)&i, sizeof(int), NULL);

	CG(in_compilation)    = 1;
	CG(compiled_filename) = stored_entry->entry.name.str.val;
	CG(zend_lineno)       = 0;
	TRACE("restoring %d:%s", stored_entry->file_inode, stored_entry->entry.name.str.val);
	xc_processor_restore_xc_entry_php_t(&restored_entry, stored_entry TSRMLS_CC);
	xc_processor_restore_xc_entry_data_php_t(stored_entry, &restored_php, stored_php, xc_readonly_protection TSRMLS_CC);
	restored_entry.php = &restored_php;
#ifdef SHOW_DPRINT
	xc_dprint(&restored_entry, 0 TSRMLS_CC);
#endif

	catched = 0;
	zend_try {
		op_array = xc_entry_install(&restored_entry TSRMLS_CC);
	} zend_catch {
		catched = 1;
	} zend_end_try();

#ifdef HAVE_XCACHE_CONSTANT
	if (restored_php.constinfos) {
		efree(restored_php.constinfos);
	}
#endif
	if (restored_php.funcinfos) {
		efree(restored_php.funcinfos);
	}
	if (restored_php.classinfos) {
		efree(restored_php.classinfos);
	}

	if (catched) {
		zend_bailout();
	}
	CG(in_compilation)    = 0;
	CG(compiled_filename) = NULL;
	TRACE("restored %d:%s", stored_entry->file_inode, stored_entry->entry.name.str.val);
	return op_array;
}
/* }}} */
static zend_op_array *xc_check_initial_compile_file(zend_file_handle *h, int type TSRMLS_DC) /* {{{ */
{
	XG(initial_compile_file_called) = 1;
	return origin_compile_file(h, type TSRMLS_CC);
}
/* }}} */
typedef struct xc_sandboxed_compiler_t { /* {{{ */
	xc_compiler_t *compiler;
	/* input */
	zend_file_handle *h;
	int type;

	/* sandbox output */
	xc_entry_php_t *stored_entry;
	xc_entry_data_php_t *stored_php;
} xc_sandboxed_compiler_t; /* {{{ */

static zend_op_array *xc_compile_file_sandboxed(void *data TSRMLS_DC) /* {{{ */
{
	xc_sandboxed_compiler_t *sandboxed_compiler = (xc_sandboxed_compiler_t *) data;
	xc_compiler_t *compiler = sandboxed_compiler->compiler;
	zend_bool catched = 0;
	xc_cache_t *cache = xc_php_caches[compiler->entry_hash.cacheid];
	xc_entry_php_t *stored_entry;
	xc_entry_data_php_t *stored_php;

	/* {{{ compile */
	/* make compile inside sandbox */
#ifdef HAVE_XCACHE_CONSTANT
	compiler->new_php.constinfos  = NULL;
#endif
	compiler->new_php.funcinfos   = NULL;
	compiler->new_php.classinfos  = NULL;
#ifdef ZEND_ENGINE_2_1
	compiler->new_php.autoglobals = NULL;
#endif
	memset(&compiler->new_php.op_array_info, 0, sizeof(compiler->new_php.op_array_info));

	XG(initial_compile_file_called) = 0;
	zend_try {
		compiler->new_php.op_array = NULL;
		xc_compile_php(compiler, sandboxed_compiler->h, sandboxed_compiler->type TSRMLS_CC);
	} zend_catch {
		catched = 1;
	} zend_end_try();

	if (catched
	 || !compiler->new_php.op_array /* possible ? */
	 || !XG(initial_compile_file_called)) {
		goto err_aftersandbox;
	}

	/* }}} */
#ifdef SHOW_DPRINT
	compiler->new_entry.php = &compiler->new_php;
	xc_dprint(&compiler->new_entry, 0 TSRMLS_CC);
#endif

	stored_entry = NULL;
	stored_php = NULL;
	ENTER_LOCK_EX(cache) { /* {{{ php_store/entry_store */
		/* php_store */
		stored_php = xc_php_store_unlocked(cache, &compiler->new_php TSRMLS_CC);
		if (!stored_php) {
			/* error */
			break;
		}
		/* entry_store */
		compiler->new_entry.php = stored_php;
		stored_entry = xc_entry_php_store_unlocked(cache, compiler->entry_hash.entryslotid, &compiler->new_entry TSRMLS_CC);
		if (stored_entry) {
			xc_php_addref_unlocked(stored_php);
			TRACE(" cached %d:%s, holding", compiler->new_entry.file_inode, stored_entry->entry.name.str.val);
			xc_entry_hold_php_unlocked(cache, stored_entry TSRMLS_CC);
		}
	} LEAVE_LOCK_EX(cache);
	/* }}} */
	TRACE("%s", stored_entry ? "stored" : "store failed");

	if (catched || !stored_php) {
		goto err_aftersandbox;
	}

	cache->compiling = 0;
	xc_free_php(&compiler->new_php TSRMLS_CC);

	if (stored_entry) {
		sandboxed_compiler->stored_entry = stored_entry;
		sandboxed_compiler->stored_php = stored_php;
		/* discard newly compiled result, restore from stored one */
		if (compiler->new_php.op_array) {
#ifdef ZEND_ENGINE_2
			destroy_op_array(compiler->new_php.op_array TSRMLS_CC);
#else
			destroy_op_array(compiler->new_php.op_array);
#endif
			efree(compiler->new_php.op_array);
			compiler->new_php.op_array = NULL;
		}
		return NULL;
	}
	else {
		return compiler->new_php.op_array;
	}

err_aftersandbox:
	xc_free_php(&compiler->new_php TSRMLS_CC);

	cache->compiling = 0;
	if (catched) {
		cache->errors ++;
		zend_bailout();
	}
	return compiler->new_php.op_array;
} /* }}} */
static zend_op_array *xc_compile_file_cached(xc_compiler_t *compiler, zend_file_handle *h, int type TSRMLS_DC) /* {{{ */
{
	/*
	if (clog) {
		return old;
	}

	if (cached_entry = getby entry_hash) {
		php = cached_entry.php;
		php = restore(php);
		return php;
	}
	else {
		if (!(php = getby md5)) {
			if (clog) {
				return old;
			}

			inside_sandbox {
				php = compile;
				entry = create entries[entry];
			}
		}

		entry.php = php;
		return php;
	}
	*/

	xc_entry_php_t *stored_entry;
	xc_entry_data_php_t *stored_php;
	zend_bool gaveup = 0;
	zend_bool catched = 0;
	zend_op_array *op_array;
	xc_cache_t *cache = xc_php_caches[compiler->entry_hash.cacheid];
	xc_sandboxed_compiler_t sandboxed_compiler;

	/* stale clogs precheck */
	if (XG(request_time) - cache->compiling < 30) {
		cache->clogs ++;
		return old_compile_file(h, type TSRMLS_CC);
	}

	/* {{{ entry_lookup/hit/md5_init/php_lookup */
	stored_entry = NULL;
	stored_php = NULL;

	ENTER_LOCK_EX(cache) {
		if (!compiler->opened_path && xc_entry_resolve_path_unlocked(compiler, compiler->filename, &stored_entry TSRMLS_CC) == SUCCESS) {
			compiler->opened_path = compiler->new_entry.entry.name.str.val;
		}
		else {
			if (!compiler->opened_path && xc_entry_php_resolve_opened_path(compiler, NULL TSRMLS_CC) != SUCCESS) {
				gaveup = 1;
				break;
			}

			/* finalize name */
			compiler->new_entry.entry.name.str.val = (char *) compiler->opened_path;
			compiler->new_entry.entry.name.str.len = strlen(compiler->new_entry.entry.name.str.val);

			stored_entry = (xc_entry_php_t *) xc_entry_find_unlocked(XC_TYPE_PHP, cache, compiler->entry_hash.entryslotid, (xc_entry_t *) &compiler->new_entry TSRMLS_CC);
		}

		if (stored_entry) {
			xc_cache_hit_unlocked(cache TSRMLS_CC);

			TRACE(" hit %d:%s, holding", compiler->new_entry.file_inode, stored_entry->entry.name.str.val);
			xc_entry_hold_php_unlocked(cache, stored_entry TSRMLS_CC);
			stored_php = stored_entry->php;
			break;
		}

		TRACE("miss entry %d:%s", compiler->new_entry.file_inode, compiler->new_entry.entry.name.str.val);

		if (xc_entry_data_php_init_md5(cache, compiler TSRMLS_CC) != SUCCESS) {
			gaveup = 1;
			break;
		}

		stored_php = xc_php_find_unlocked(cache, &compiler->new_php TSRMLS_CC);

		if (stored_php) {
			compiler->new_entry.php = stored_php;
			xc_entry_php_init(&compiler->new_entry, compiler->opened_path TSRMLS_CC);
			stored_entry = xc_entry_php_store_unlocked(cache, compiler->entry_hash.entryslotid, &compiler->new_entry TSRMLS_CC);
			if (stored_entry) {
				xc_php_addref_unlocked(stored_php);
				TRACE(" cached %d:%s, holding", compiler->new_entry.file_inode, stored_entry->entry.name.str.val);
				xc_entry_hold_php_unlocked(cache, stored_entry TSRMLS_CC);
			}
			else {
				gaveup = 1;
			}
			break;
		}

		if (XG(request_time) - cache->compiling < 30) {
			TRACE("%s", "miss php, but compiling");
			cache->clogs ++;
			gaveup = 1;
			break;
		}

		TRACE("%s", "miss php, going to compile");
		cache->compiling = XG(request_time);
	} LEAVE_LOCK_EX(cache);

	if (catched) {
		cache->compiling = 0;
		zend_bailout();
	}

	/* found entry */
	if (stored_entry && stored_php) {
		zend_llist_add_element(&CG(open_files), h);
		return xc_compile_restore(stored_entry, stored_php TSRMLS_CC);
	}

	/* gaveup */
	if (gaveup) {
		return old_compile_file(h, type TSRMLS_CC);
	}
	/* }}} */

	sandboxed_compiler.compiler = compiler;
	sandboxed_compiler.h = h;
	sandboxed_compiler.type = type;
	sandboxed_compiler.stored_php = NULL;
	sandboxed_compiler.stored_entry = NULL;
	op_array = xc_sandbox(xc_compile_file_sandboxed, (void *) &sandboxed_compiler, h->opened_path ? h->opened_path : h->filename TSRMLS_CC);
	if (sandboxed_compiler.stored_entry) {
		return xc_compile_restore(sandboxed_compiler.stored_entry, sandboxed_compiler.stored_php TSRMLS_CC);
	}
	else {
		return op_array;
	}
}
/* }}} */
static zend_op_array *xc_compile_file(zend_file_handle *h, int type TSRMLS_DC) /* {{{ */
{
	xc_compiler_t compiler;
	zend_op_array *op_array;

	assert(xc_initized);

	TRACE("xc_compile_file: type=%d name=%s", h->type, h->filename ? h->filename : "NULL");

	if (!XG(cacher)
	 || !h->filename
	 || !SG(request_info).path_translated
	 || strstr(h->filename, "://") != NULL
#ifdef ZEND_ENGINE_2_3
	 /* supported by php_resolve_path */
	 || (!XG(stat) && strstr(PG(include_path), "://") != NULL)
#else
	 || strstr(PG(include_path), "://") != NULL
#endif
	 || !xc_shm || xc_shm->disabled
	 ) {
		TRACE("%s", "cacher not enabled");
		return old_compile_file(h, type TSRMLS_CC);
	}

	/* {{{ entry_init_key */
	compiler.opened_path = h->opened_path;
	compiler.filename = compiler.opened_path ? compiler.opened_path : h->filename;
	compiler.filename_len = strlen(compiler.filename);
	if (xc_entry_php_init_key(&compiler TSRMLS_CC) != SUCCESS) {
		TRACE("failed to init key for %s", compiler.filename);
		return old_compile_file(h, type TSRMLS_CC);
	}
	/* }}} */

	op_array = xc_compile_file_cached(&compiler, h, type TSRMLS_CC);

	xc_entry_free_key_php(&compiler.new_entry TSRMLS_CC);

	return op_array;
}
/* }}} */

/* gdb helper functions, but N/A for coredump */
int xc_is_rw(const void *p) /* {{{ */
{
	xc_shm_t *shm;
	size_t i;

	if (xc_php_caches) {
		for (i = 0; i < xc_php_hcache.size; i ++) {
			shm = xc_php_caches[i]->shm;
			if (shm->handlers->is_readwrite(shm, p)) {
				return 1;
			}
		}
	}

	if (xc_var_caches) {
		for (i = 0; i < xc_var_hcache.size; i ++) {
			shm = xc_var_caches[i]->shm;
			if (shm->handlers->is_readwrite(shm, p)) {
				return 1;
			}
		}
	}
	return 0;
}
/* }}} */
int xc_is_ro(const void *p) /* {{{ */
{
	xc_shm_t *shm;
	size_t i;

	if (xc_php_caches) {
		for (i = 0; i < xc_php_hcache.size; i ++) {
			shm = xc_php_caches[i]->shm;
			if (shm->handlers->is_readonly(shm, p)) {
				return 1;
			}
		}
	}

	if (xc_var_caches) {
		for (i = 0; i < xc_var_hcache.size; i ++) {
			shm = xc_var_caches[i]->shm;
			if (shm->handlers->is_readonly(shm, p)) {
				return 1;
			}
		}
	}
	return 0;
}
/* }}} */
int xc_is_shm(const void *p) /* {{{ */
{
	return xc_is_ro(p) || xc_is_rw(p);
}
/* }}} */

void xc_gc_add_op_array(xc_gc_op_array_t *gc_op_array TSRMLS_DC) /* {{{ */
{
	zend_llist_add_element(&XG(gc_op_arrays), (void *) gc_op_array);
}
/* }}} */
static void xc_gc_op_array(void *pDest) /* {{{ */
{
	xc_gc_op_array_t *op_array = (xc_gc_op_array_t *) pDest;
	zend_uint i;
#ifdef ZEND_ENGINE_2
	if (op_array->arg_info) {
		for (i = 0; i < op_array->num_args; i++) {
			efree((char *) ZSTR_V(op_array->arg_info[i].name));
			if (ZSTR_V(op_array->arg_info[i].class_name)) {
				efree((char *) ZSTR_V(op_array->arg_info[i].class_name));
			}
		}
		efree(op_array->arg_info);
	}
#endif
	if (op_array->opcodes) {
		efree(op_array->opcodes);
	}
}
/* }}} */

/* module helper function */
static int xc_init_constant(int module_number TSRMLS_DC) /* {{{ */
{
	typedef struct {
		const char *prefix;
		zend_uchar (*getsize)();
		const char *(*get)(zend_uchar i);
	} xc_meminfo_t;
	xc_meminfo_t nameinfos[] = {
		{ "",        xc_get_op_type_count,   xc_get_op_type   },
		{ "",        xc_get_data_type_count, xc_get_data_type },
		{ "",        xc_get_opcode_count,    xc_get_opcode    },
		{ "OPSPEC_", xc_get_op_spec_count,   xc_get_op_spec   },
		{ NULL, NULL, NULL }
	};
	xc_meminfo_t* p;
	zend_uchar i, count;
	char const_name[96];
	int const_name_len;
	int undefdone = 0;

	for (p = nameinfos; p->getsize; p ++) {
		count = p->getsize();
		for (i = 0; i < count; i ++) {
			const char *name = p->get(i);
			if (!name) continue;
			if (strcmp(name, "UNDEF") == 0) {
				if (undefdone) continue;
				undefdone = 1;
			}
			const_name_len = snprintf(const_name, sizeof(const_name), "XC_%s%s", p->prefix, name);
			zend_register_long_constant(const_name, const_name_len+1, i, CONST_CS | CONST_PERSISTENT, module_number TSRMLS_CC);
		}
	}

	zend_register_long_constant(ZEND_STRS("XC_SIZEOF_TEMP_VARIABLE"), sizeof(temp_variable), CONST_CS | CONST_PERSISTENT, module_number TSRMLS_CC);
	zend_register_long_constant(ZEND_STRS("XC_TYPE_PHP"), XC_TYPE_PHP, CONST_CS | CONST_PERSISTENT, module_number TSRMLS_CC);
	zend_register_long_constant(ZEND_STRS("XC_TYPE_VAR"), XC_TYPE_VAR, CONST_CS | CONST_PERSISTENT, module_number TSRMLS_CC);
	zend_register_stringl_constant(ZEND_STRS("XCACHE_VERSION"), ZEND_STRL(XCACHE_VERSION), CONST_CS | CONST_PERSISTENT, module_number TSRMLS_CC);
	zend_register_stringl_constant(ZEND_STRS("XCACHE_MODULES"), ZEND_STRL(XCACHE_MODULES), CONST_CS | CONST_PERSISTENT, module_number TSRMLS_CC);
	return 0;
}
/* }}} */
static void xc_cache_destroy(xc_cache_t **caches, xc_hash_t *hcache) /* {{{ */
{
	size_t i;
	xc_cache_t *cache;

	if (!caches) {
		return;
	}

	for (i = 0; i < hcache->size; i ++) {
		cache = caches[i];
		if (cache) {
			if (cache->lck) {
				xc_lock_destroy(cache->lck);
			}
			/* do NOT free
			if (cache->entries) {
				cache->mem->handlers->free(cache->mem, cache->entries);
			}
			cache->mem->handlers->free(cache->mem, cache);
			*/
			cache->shm->handlers->memdestroy(cache->mem);
		}
	}
	free(caches);
}
/* }}} */
static xc_cache_t **xc_cache_init(xc_shm_t *shm, xc_hash_t *hcache, xc_hash_t *hentry, xc_hash_t *hphp, xc_shmsize_t shmsize) /* {{{ */
{
	xc_cache_t **caches = NULL, *cache;
	xc_mem_t *mem;
	time_t now = time(NULL);
	size_t i;
	xc_memsize_t memsize;

	memsize = shmsize / hcache->size;

	/* Don't let it break out of mem after ALIGNed
	 * This is important for 
	 * Simply loop until it fit our need
	 */
	while (ALIGN(memsize) * hcache->size > shmsize && ALIGN(memsize) != memsize) {
		if (memsize < ALIGN(1)) {
			CHECK(NULL, "cache too small");
		}
		memsize --;
	}

	CHECK(caches = calloc(hcache->size, sizeof(xc_cache_t *)), "caches OOM");

	for (i = 0; i < hcache->size; i ++) {
		CHECK(mem            = shm->handlers->meminit(shm, memsize), "Failed init memory allocator");
		CHECK(cache          = mem->handlers->calloc(mem, 1, sizeof(xc_cache_t)), "cache OOM");
		CHECK(cache->entries = mem->handlers->calloc(mem, hentry->size, sizeof(xc_entry_t*)), "entries OOM");
		if (hphp) {
			CHECK(cache->phps= mem->handlers->calloc(mem, hphp->size, sizeof(xc_entry_data_php_t*)), "phps OOM");
		}
		CHECK(cache->lck     = xc_lock_init(NULL), "can't create lock");

		cache->hcache  = hcache;
		cache->hentry  = hentry;
		cache->hphp    = hphp;
		cache->shm     = shm;
		cache->mem     = mem;
		cache->cacheid = i;
		cache->last_gc_deletes = now;
		cache->last_gc_expires = now;
		caches[i] = cache;
	}
	return caches;

err:
	if (caches) {
		xc_cache_destroy(caches, hcache);
	}
	return NULL;
}
/* }}} */
static void xc_destroy() /* {{{ */
{
	if (old_compile_file) {
		zend_compile_file = old_compile_file;
		old_compile_file = NULL;
	}

	if (origin_compile_file) {
		zend_compile_file = origin_compile_file;
		origin_compile_file = NULL;
	}

	if (xc_php_caches) {
		xc_cache_destroy(xc_php_caches, &xc_php_hcache);
		xc_php_caches = NULL;
	}

	if (xc_var_caches) {
		xc_cache_destroy(xc_var_caches, &xc_var_hcache);
		xc_var_caches = NULL;
	}

	if (xc_shm) {
		xc_shm_destroy(xc_shm);
		xc_shm = NULL;
	}

	xc_initized = 0;
}
/* }}} */
static int xc_init(int module_number TSRMLS_DC) /* {{{ */
{
	xc_shmsize_t shmsize = ALIGN(xc_php_size) + ALIGN(xc_var_size);

	xc_php_caches = xc_var_caches = NULL;
	xc_shm = NULL;

	if (shmsize < (size_t) xc_php_size || shmsize < (size_t) xc_var_size) {
		zend_error(E_ERROR, "XCache: neither xcache.size nor xcache.var_size can be negative");
		goto err;
	}

	if (xc_php_size || xc_var_size) {
		CHECK(xc_shm = xc_shm_init(xc_shm_scheme, shmsize, xc_readonly_protection, xc_mmap_path, NULL), "Cannot create shm");
		if (!xc_shm->handlers->can_readonly(xc_shm)) {
			xc_readonly_protection = 0;
		}

		if (xc_php_size) {
			old_compile_file = zend_compile_file;
			zend_compile_file = xc_compile_file;

			CHECK(xc_php_caches = xc_cache_init(xc_shm, &xc_php_hcache, &xc_php_hentry, &xc_php_hentry, xc_php_size), "failed init opcode cache");
		}

		if (xc_var_size) {
			CHECK(xc_var_caches = xc_cache_init(xc_shm, &xc_var_hcache, &xc_var_hentry, NULL, xc_var_size), "failed init variable cache");
		}
	}
	return SUCCESS;

err:
	if (xc_php_caches || xc_var_caches) {
		xc_destroy();
		/* shm destroied in xc_destroy() */
	}
	else if (xc_shm) {
		xc_destroy();
		xc_shm_destroy(xc_shm);
		xc_shm = NULL;
	}
	return 0;
}
/* }}} */
static void xc_request_init(TSRMLS_D) /* {{{ */
{
	size_t i;

	if (!XG(internal_table_copied)) {
		zend_function tmp_func;
		xc_cest_t tmp_cest;

#ifdef HAVE_XCACHE_CONSTANT
		zend_hash_destroy(&XG(internal_constant_table));
#endif
		zend_hash_destroy(&XG(internal_function_table));
		zend_hash_destroy(&XG(internal_class_table));

#ifdef HAVE_XCACHE_CONSTANT
		zend_hash_init_ex(&XG(internal_constant_table), 20,  NULL, (dtor_func_t) xc_zend_constant_dtor, 1, 0);
#endif
		zend_hash_init_ex(&XG(internal_function_table), 100, NULL, NULL, 1, 0);
		zend_hash_init_ex(&XG(internal_class_table),    10,  NULL, NULL, 1, 0);

#ifdef HAVE_XCACHE_CONSTANT
		xc_copy_internal_zend_constants(&XG(internal_constant_table), EG(zend_constants));
#endif
		zend_hash_copy(&XG(internal_function_table), CG(function_table), NULL, &tmp_func, sizeof(tmp_func));
		zend_hash_copy(&XG(internal_class_table), CG(class_table), NULL, &tmp_cest, sizeof(tmp_cest));

		XG(internal_table_copied) = 1;
	}
	if (xc_php_caches && !XG(php_holds)) {
		XG(php_holds) = calloc(xc_php_hcache.size, sizeof(xc_stack_t));
		for (i = 0; i < xc_php_hcache.size; i ++) {
			xc_stack_init(&XG(php_holds[i]));
		}
	}

	if (xc_var_caches && !XG(var_holds)) {
		XG(var_holds) = calloc(xc_var_hcache.size, sizeof(xc_stack_t));
		for (i = 0; i < xc_var_hcache.size; i ++) {
			xc_stack_init(&XG(var_holds[i]));
		}
	}

#ifdef ZEND_ENGINE_2
	zend_llist_init(&XG(gc_op_arrays), sizeof(xc_gc_op_array_t), xc_gc_op_array, 0);
#endif

#if PHP_API_VERSION <= 20041225
	XG(request_time) = time(NULL);
#else
	XG(request_time) = sapi_get_request_time(TSRMLS_C);
#endif
}
/* }}} */
static void xc_request_shutdown(TSRMLS_D) /* {{{ */
{
	if (xc_shm && !xc_shm->disabled) {
		xc_entry_unholds(TSRMLS_C);
		xc_gc_expires_php(TSRMLS_C);
		xc_gc_expires_var(TSRMLS_C);
		xc_gc_deletes(TSRMLS_C);
	}
#ifdef ZEND_ENGINE_2
	zend_llist_destroy(&XG(gc_op_arrays));
#endif
}
/* }}} */
/* {{{ PHP_GINIT_FUNCTION(xcache) */
static
#ifdef PHP_GINIT_FUNCTION
PHP_GINIT_FUNCTION(xcache)
#else
void xc_init_globals(zend_xcache_globals* xcache_globals TSRMLS_DC)
#endif
{
	memset(xcache_globals, 0, sizeof(zend_xcache_globals));

#ifdef HAVE_XCACHE_CONSTANT
	zend_hash_init_ex(&xcache_globals->internal_constant_table, 1, NULL, (dtor_func_t) xc_zend_constant_dtor, 1, 0);
#endif
	zend_hash_init_ex(&xcache_globals->internal_function_table, 1, NULL, NULL, 1, 0);
	zend_hash_init_ex(&xcache_globals->internal_class_table,    1, NULL, NULL, 1, 0);
}
/* }}} */
/* {{{ PHP_GSHUTDOWN_FUNCTION(xcache) */
static
#ifdef PHP_GSHUTDOWN_FUNCTION
PHP_GSHUTDOWN_FUNCTION(xcache)
#else
void xc_shutdown_globals(zend_xcache_globals* xcache_globals TSRMLS_DC)
#endif
{
	size_t i;

	if (xcache_globals->php_holds != NULL) {
		for (i = 0; i < xc_php_hcache.size; i ++) {
			xc_stack_destroy(&xcache_globals->php_holds[i]);
		}
		free(xcache_globals->php_holds);
		xcache_globals->php_holds = NULL;
	}

	if (xcache_globals->var_holds != NULL) {
		for (i = 0; i < xc_var_hcache.size; i ++) {
			xc_stack_destroy(&xcache_globals->var_holds[i]);
		}
		free(xcache_globals->var_holds);
		xcache_globals->var_holds = NULL;
	}

	if (xcache_globals->internal_table_copied) {
#ifdef HAVE_XCACHE_CONSTANT
		zend_hash_destroy(&xcache_globals->internal_constant_table);
#endif
		zend_hash_destroy(&xcache_globals->internal_function_table);
		zend_hash_destroy(&xcache_globals->internal_class_table);
	}
}
/* }}} */

/* user functions */
static int xcache_admin_auth_check(TSRMLS_D) /* {{{ */
{
	zval **server = NULL;
	zval **user = NULL;
	zval **pass = NULL;
	char *admin_user = NULL;
	char *admin_pass = NULL;
	HashTable *ht;

	/* auth disabled, nothing to do.. */
	if (!XG(auth_enabled)) {
		return 1;
	}

	if (cfg_get_string("xcache.admin.user", &admin_user) == FAILURE || !admin_user[0]) {
		admin_user = NULL;
	}
	if (cfg_get_string("xcache.admin.pass", &admin_pass) == FAILURE || !admin_pass[0]) {
		admin_pass = NULL;
	}

	if (admin_user == NULL || admin_pass == NULL) {
		php_error_docref(XCACHE_WIKI_URL "/InstallAdministration" TSRMLS_CC, E_ERROR,
				"xcache.admin.user and/or xcache.admin.pass settings is not configured."
				" Make sure you've modified the correct php ini file for your php used in webserver.");
		zend_bailout();
	}
	if (strlen(admin_pass) != 32) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "xcache.admin.pass is %lu chars unexpectedly, it is supposed to be the password after md5() which should be 32 chars", (unsigned long) strlen(admin_pass));
		zend_bailout();
	}

#ifdef ZEND_ENGINE_2_1
	zend_is_auto_global("_SERVER", sizeof("_SERVER") - 1 TSRMLS_CC);
#endif
	if (zend_hash_find(&EG(symbol_table), "_SERVER", sizeof("_SERVER"), (void **) &server) != SUCCESS || Z_TYPE_PP(server) != IS_ARRAY) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "_SERVER is corrupted");
		zend_bailout();
	}
	ht = HASH_OF((*server));

	if (zend_hash_find(ht, "PHP_AUTH_USER", sizeof("PHP_AUTH_USER"), (void **) &user) == FAILURE) {
	 	user = NULL;
	}
	else if (Z_TYPE_PP(user) != IS_STRING) {
		user = NULL;
	}

	if (zend_hash_find(ht, "PHP_AUTH_PW", sizeof("PHP_AUTH_PW"), (void **) &pass) == FAILURE) {
	 	pass = NULL;
	}
	else if (Z_TYPE_PP(pass) != IS_STRING) {
		pass = NULL;
	}

	if (user != NULL && pass != NULL && strcmp(admin_user, Z_STRVAL_PP(user)) == 0) {
		PHP_MD5_CTX context;
		char md5str[33];
		unsigned char digest[16];

		PHP_MD5Init(&context);
		PHP_MD5Update(&context, (unsigned char *) Z_STRVAL_PP(pass), Z_STRLEN_PP(pass));
		PHP_MD5Final(digest, &context);

		md5str[0] = '\0';
		make_digest(md5str, digest);
		if (strcmp(admin_pass, md5str) == 0) {
			return 1;
		}
	}

#define STR "HTTP/1.0 401 Unauthorized"
	sapi_add_header_ex(STR, sizeof(STR) - 1, 1, 1 TSRMLS_CC);
#undef STR
#define STR "WWW-authenticate: Basic Realm=\"XCache Administration\""
	sapi_add_header_ex(STR, sizeof(STR) - 1, 1, 1 TSRMLS_CC);
#undef STR
#define STR "Content-type: text/html; charset=UTF-8"
	sapi_add_header_ex(STR, sizeof(STR) - 1, 1, 1 TSRMLS_CC);
#undef STR
	ZEND_PUTS("<html>\n");
	ZEND_PUTS("<head><title>XCache Authentication Failed</title></head>\n");
	ZEND_PUTS("<body>\n");
	ZEND_PUTS("<h1>XCache Authentication Failed</h1>\n");
	ZEND_PUTS("<p>You're not authorized to access this page due to wrong username and/or password you typed.<br />The following check points is suggested:</p>\n");
	ZEND_PUTS("<ul>\n");
	ZEND_PUTS("<li>Be aware that `Username' and `Password' is case sense. Check capslock status led on your keyboard, and punch left/right Shift keys once for each</li>\n");
	ZEND_PUTS("<li>Make sure the md5 password is generated correctly. You may use <a href=\"mkpassword.php\">mkpassword.php</a></li>\n");
	ZEND_PUTS("<li>Reload browser cache by pressing F5 and/or Ctrl+F5, or simply clear browser cache after you've updated username/password in php ini.</li>\n");
	ZEND_PUTS("</ul>\n");
	ZEND_PUTS("Check <a href=\"" XCACHE_WIKI_URL "/InstallAdministration\">XCache wiki page</a> for more information.\n");
	ZEND_PUTS("</body>\n");
	ZEND_PUTS("</html>\n");

	zend_bailout();
	return 0;
}
/* }}} */
static void xc_clear(long type, xc_cache_t *cache TSRMLS_DC) /* {{{ */
{
	xc_entry_t *e, *next;
	int entryslotid, c;

	ENTER_LOCK(cache) {
		for (entryslotid = 0, c = cache->hentry->size; entryslotid < c; entryslotid ++) {
			for (e = cache->entries[entryslotid]; e; e = next) {
				next = e->next;
				xc_entry_remove_unlocked(type, cache, entryslotid, e TSRMLS_CC);
			}
			cache->entries[entryslotid] = NULL;
		}
	} LEAVE_LOCK(cache);
} /* }}} */
/* {{{ xcache_admin_operate */
typedef enum { XC_OP_COUNT, XC_OP_INFO, XC_OP_LIST, XC_OP_CLEAR } xcache_op_type;
static void xcache_admin_operate(xcache_op_type optype, INTERNAL_FUNCTION_PARAMETERS)
{
	long type;
	int size;
	xc_cache_t **caches, *cache;
	long id = 0;

	xcache_admin_auth_check(TSRMLS_C);

	if (!xc_initized || !xc_shm || xc_shm->disabled) {
		RETURN_NULL();
	}

	switch (optype) {
		case XC_OP_COUNT:
			if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &type) == FAILURE) {
				return;
			}
			break;
		case XC_OP_CLEAR:
			id = -1;
			if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|l", &type, &id) == FAILURE) {
				return;
			}
			break;
		default:
			if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ll", &type, &id) == FAILURE) {
				return;
			}
	}

	switch (type) {
		case XC_TYPE_PHP:
			size = xc_php_hcache.size;
			caches = xc_php_caches;
			break;

		case XC_TYPE_VAR:
			size = xc_var_hcache.size;
			caches = xc_var_caches;
			break;

		default:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unknown type %ld", type);
			RETURN_FALSE;
	}

	switch (optype) {
		case XC_OP_COUNT:
			RETURN_LONG(caches ? size : 0)
			break;

		case XC_OP_INFO:
		case XC_OP_LIST:
			if (!caches || id < 0 || id >= size) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cache not exists");
				RETURN_FALSE;
			}

			array_init(return_value);

			cache = caches[id];
			ENTER_LOCK(cache) {
				if (optype == XC_OP_INFO) {
					xc_fillinfo_unlocked(type, cache, return_value TSRMLS_CC);
				}
				else {
					xc_filllist_unlocked(type, cache, return_value TSRMLS_CC);
				}
			} LEAVE_LOCK(cache);
			break;

		case XC_OP_CLEAR:
			if (!caches || id < -1 || id >= size) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cache not exists");
				RETURN_FALSE;
			}

			if (id == -1) {
				for (id = 0; id < size; ++id) {
					xc_clear(type, caches[id] TSRMLS_CC);
				}
			}
			else {
				xc_clear(type, caches[id] TSRMLS_CC);
			}

			xc_gc_deletes(TSRMLS_C);
			break;

		default:
			assert(0);
	}
}
/* }}} */
/* {{{ proto int xcache_count(int type)
   Return count of cache on specified cache type */
PHP_FUNCTION(xcache_count)
{
	xcache_admin_operate(XC_OP_COUNT, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */
/* {{{ proto array xcache_info(int type, int id)
   Get cache info by id on specified cache type */
PHP_FUNCTION(xcache_info)
{
	xcache_admin_operate(XC_OP_INFO, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */
/* {{{ proto array xcache_list(int type, int id)
   Get cache entries list by id on specified cache type */
PHP_FUNCTION(xcache_list)
{
	xcache_admin_operate(XC_OP_LIST, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */
/* {{{ proto array xcache_clear_cache(int type, [ int id = -1 ])
   Clear cache by id on specified cache type */
PHP_FUNCTION(xcache_clear_cache)
{
	xcache_admin_operate(XC_OP_CLEAR, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

#define VAR_DISABLED_WARNING() do { \
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "XCache var cache was not initialized properly. Check php log for actual reason"); \
} while (0)

static int xc_entry_var_init_key(xc_entry_var_t *entry_var, xc_entry_hash_t *entry_hash, zval *name TSRMLS_DC) /* {{{ */
{
	xc_hash_value_t hv;

	switch (name->type) {
#ifdef IS_UNICODE
		case IS_UNICODE:
		case IS_STRING:
#endif
		default:
#ifdef IS_UNICODE
			convert_to_unicode(name);
#else
			convert_to_string(name);
#endif
	}

#ifdef IS_UNICODE
	entry_var->name_type = name->type;
#endif
	entry_var->entry.name = name->value;

	hv = xc_entry_hash_var((xc_entry_t *) entry_var TSRMLS_CC);

	entry_hash->cacheid = (hv & xc_var_hcache.mask);
	hv >>= xc_var_hcache.bits;
	entry_hash->entryslotid = (hv & xc_var_hentry.mask);
	return SUCCESS;
}
/* }}} */
/* {{{ proto mixed xcache_get(string name)
   Get cached data by specified name */
PHP_FUNCTION(xcache_get)
{
	xc_entry_hash_t entry_hash;
	xc_cache_t *cache;
	xc_entry_var_t entry_var, *stored_entry_var;
	zval *name;

	if (!xc_var_caches || !xc_shm || xc_shm->disabled) {
		VAR_DISABLED_WARNING();
		RETURN_NULL();
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &name) == FAILURE) {
		return;
	}
	xc_entry_var_init_key(&entry_var, &entry_hash, name TSRMLS_CC);
	cache = xc_var_caches[entry_hash.cacheid];

	ENTER_LOCK(cache) {
		stored_entry_var = (xc_entry_var_t *) xc_entry_find_unlocked(XC_TYPE_VAR, cache, entry_hash.entryslotid, (xc_entry_t *) &entry_var TSRMLS_CC);
		if (stored_entry_var) {
			/* return */
			xc_processor_restore_zval(return_value, stored_entry_var->value, stored_entry_var->have_references TSRMLS_CC);
			xc_cache_hit_unlocked(cache TSRMLS_CC);
		}
		else {
			RETVAL_NULL();
		}
	} LEAVE_LOCK(cache);
}
/* }}} */
/* {{{ proto bool  xcache_set(string name, mixed value [, int ttl])
   Store data to cache by specified name */
PHP_FUNCTION(xcache_set)
{
	xc_entry_hash_t entry_hash;
	xc_cache_t *cache;
	xc_entry_var_t entry_var, *stored_entry_var;
	zval *name;
	zval *value;

	if (!xc_var_caches || !xc_shm || xc_shm->disabled) {
		VAR_DISABLED_WARNING();
		RETURN_NULL();
	}

	entry_var.entry.ttl = XG(var_ttl);
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz|l", &name, &value, &entry_var.entry.ttl) == FAILURE) {
		return;
	}

	if (Z_TYPE_P(value) == IS_OBJECT) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "Objects cannot be stored in the variable cache. Use serialize before xcache_set");
		RETURN_NULL();
	}

	/* max ttl */
	if (xc_var_maxttl && (!entry_var.entry.ttl || entry_var.entry.ttl > xc_var_maxttl)) {
		entry_var.entry.ttl = xc_var_maxttl;
	}

	xc_entry_var_init_key(&entry_var, &entry_hash, name TSRMLS_CC);
	cache = xc_var_caches[entry_hash.cacheid];

	ENTER_LOCK(cache) {
		stored_entry_var = (xc_entry_var_t *) xc_entry_find_unlocked(XC_TYPE_VAR, cache, entry_hash.entryslotid, (xc_entry_t *) &entry_var TSRMLS_CC);
		if (stored_entry_var) {
			xc_entry_remove_unlocked(XC_TYPE_VAR, cache, entry_hash.entryslotid, (xc_entry_t *) stored_entry_var TSRMLS_CC);
		}
		entry_var.value = value;
		RETVAL_BOOL(xc_entry_var_store_unlocked(cache, entry_hash.entryslotid, &entry_var TSRMLS_CC) != NULL ? 1 : 0);
	} LEAVE_LOCK(cache);
}
/* }}} */
/* {{{ proto bool  xcache_isset(string name)
   Check if an entry exists in cache by specified name */
PHP_FUNCTION(xcache_isset)
{
	xc_entry_hash_t entry_hash;
	xc_cache_t *cache;
	xc_entry_var_t entry_var, *stored_entry_var;
	zval *name;

	if (!xc_var_caches || !xc_shm || xc_shm->disabled) {
		VAR_DISABLED_WARNING();
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &name) == FAILURE) {
		return;
	}
	xc_entry_var_init_key(&entry_var, &entry_hash, name TSRMLS_CC);
	cache = xc_var_caches[entry_hash.cacheid];

	ENTER_LOCK(cache) {
		stored_entry_var = (xc_entry_var_t *) xc_entry_find_unlocked(XC_TYPE_VAR, cache, entry_hash.entryslotid, (xc_entry_t *) &entry_var TSRMLS_CC);
		if (stored_entry_var) {
			xc_cache_hit_unlocked(cache TSRMLS_CC);
			RETVAL_TRUE;
			/* return */
		}
		else {
			RETVAL_FALSE;
		}

	} LEAVE_LOCK(cache);
}
/* }}} */
/* {{{ proto bool  xcache_unset(string name)
   Unset existing data in cache by specified name */
PHP_FUNCTION(xcache_unset)
{
	xc_entry_hash_t entry_hash;
	xc_cache_t *cache;
	xc_entry_var_t entry_var, *stored_entry_var;
	zval *name;

	if (!xc_var_caches || !xc_shm || xc_shm->disabled) {
		VAR_DISABLED_WARNING();
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &name) == FAILURE) {
		return;
	}
	xc_entry_var_init_key(&entry_var, &entry_hash, name TSRMLS_CC);
	cache = xc_var_caches[entry_hash.cacheid];

	ENTER_LOCK(cache) {
		stored_entry_var = (xc_entry_var_t *) xc_entry_find_unlocked(XC_TYPE_VAR, cache, entry_hash.entryslotid, (xc_entry_t *) &entry_var TSRMLS_CC);
		if (stored_entry_var) {
			xc_entry_remove_unlocked(XC_TYPE_VAR, cache, entry_hash.entryslotid, (xc_entry_t *) stored_entry_var TSRMLS_CC);
			RETVAL_TRUE;
		}
		else {
			RETVAL_FALSE;
		}
	} LEAVE_LOCK(cache);
}
/* }}} */
/* {{{ proto bool  xcache_unset_by_prefix(string prefix)
   Unset existing data in cache by specified prefix */
PHP_FUNCTION(xcache_unset_by_prefix)
{
	zval *prefix;
	int i, iend;

	if (!xc_var_caches || !xc_shm || xc_shm->disabled) {
		VAR_DISABLED_WARNING();
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &prefix) == FAILURE) {
		return;
	}

	for (i = 0, iend = xc_var_hcache.size; i < iend; i ++) {
		xc_cache_t *cache = xc_var_caches[i];
		ENTER_LOCK(cache) {
			int entryslotid, jend;
			for (entryslotid = 0, jend = cache->hentry->size; entryslotid < jend; entryslotid ++) {
				xc_entry_t *entry, *next;
				for (entry = cache->entries[entryslotid]; entry; entry = next) {
					next = entry->next;
					if (xc_entry_has_prefix_unlocked(XC_TYPE_VAR, entry, prefix)) {
						xc_entry_remove_unlocked(XC_TYPE_VAR, cache, entryslotid, entry TSRMLS_CC);
					}
				}
			}
		} LEAVE_LOCK(cache);
	}
}
/* }}} */
static inline void xc_var_inc_dec(int inc, INTERNAL_FUNCTION_PARAMETERS) /* {{{ */
{
	xc_entry_hash_t entry_hash;
	xc_cache_t *cache;
	xc_entry_var_t entry_var, *stored_entry_var;
	zval *name;
	long count = 1;
	long value = 0;
	zval oldzval;

	if (!xc_var_caches || !xc_shm || xc_shm->disabled) {
		VAR_DISABLED_WARNING();
		RETURN_NULL();
	}

	entry_var.entry.ttl = XG(var_ttl);
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|ll", &name, &count, &entry_var.entry.ttl) == FAILURE) {
		return;
	}

	/* max ttl */
	if (xc_var_maxttl && (!entry_var.entry.ttl || entry_var.entry.ttl > xc_var_maxttl)) {
		entry_var.entry.ttl = xc_var_maxttl;
	}

	xc_entry_var_init_key(&entry_var, &entry_hash, name TSRMLS_CC);
	cache = xc_var_caches[entry_hash.cacheid];

	ENTER_LOCK(cache) {
		stored_entry_var = (xc_entry_var_t *) xc_entry_find_unlocked(XC_TYPE_VAR, cache, entry_hash.entryslotid, (xc_entry_t *) &entry_var TSRMLS_CC);
		if (stored_entry_var) {
			TRACE("incdec: got entry_var %s", entry_var.entry.name.str.val);
			/* do it in place */
			if (Z_TYPE_P(stored_entry_var->value) == IS_LONG) {
				zval *zv;
				stored_entry_var->entry.ctime = XG(request_time);
				stored_entry_var->entry.ttl   = entry_var.entry.ttl;
				TRACE("%s", "incdec: islong");
				value = Z_LVAL_P(stored_entry_var->value);
				value += (inc == 1 ? count : - count);
				RETVAL_LONG(value);

				zv = (zval *) cache->shm->handlers->to_readwrite(cache->shm, (char *) stored_entry_var->value);
				Z_LVAL_P(zv) = value;
				++cache->updates;
				break; /* leave lock */
			}

			TRACE("%s", "incdec: notlong");
			xc_processor_restore_zval(&oldzval, stored_entry_var->value, stored_entry_var->have_references TSRMLS_CC);
			convert_to_long(&oldzval);
			value = Z_LVAL(oldzval);
			zval_dtor(&oldzval);
		}
		else {
			TRACE("incdec: %s not found", entry_var.entry.name.str.val);
		}

		value += (inc == 1 ? count : - count);
		RETVAL_LONG(value);
		entry_var.value = return_value;

		if (stored_entry_var) {
			entry_var.entry.atime = stored_entry_var->entry.atime;
			entry_var.entry.ctime = stored_entry_var->entry.ctime;
			entry_var.entry.hits  = stored_entry_var->entry.hits;
			xc_entry_remove_unlocked(XC_TYPE_VAR, cache, entry_hash.entryslotid, (xc_entry_t *) stored_entry_var TSRMLS_CC);
		}
		xc_entry_var_store_unlocked(cache, entry_hash.entryslotid, &entry_var TSRMLS_CC);
	} LEAVE_LOCK(cache);
}
/* }}} */
/* {{{ proto int xcache_inc(string name [, int value [, int ttl]])
   Increase an int counter in cache by specified name, create it if not exists */
PHP_FUNCTION(xcache_inc)
{
	xc_var_inc_dec(1, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */
/* {{{ proto int xcache_dec(string name [, int value [, int ttl]])
   Decrease an int counter in cache by specified name, create it if not exists */
PHP_FUNCTION(xcache_dec)
{
	xc_var_inc_dec(-1, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */
/* {{{ proto int xcache_get_refcount(mixed variable)
   XCache internal uses only: Get reference count of variable */
PHP_FUNCTION(xcache_get_refcount)
{
	zval *variable;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &variable) == FAILURE) {
		RETURN_NULL();
	}

	RETURN_LONG(Z_REFCOUNT(*variable));
}
/* }}} */
/* {{{ proto bool xcache_get_isref(mixed variable)
   XCache internal uses only: Check if variable data is marked referenced */
#ifdef ZEND_BEGIN_ARG_INFO_EX
ZEND_BEGIN_ARG_INFO_EX(arginfo_xcache_get_isref, 0, 0, 1)
	ZEND_ARG_INFO(1, variable)
ZEND_END_ARG_INFO()
#else
static unsigned char arginfo_xcache_get_isref[] = { 1, BYREF_FORCE };
#endif

PHP_FUNCTION(xcache_get_isref)
{
	zval *variable;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &variable) == FAILURE) {
		RETURN_NULL();
	}

	RETURN_BOOL(Z_ISREF(*variable) && Z_REFCOUNT(*variable) >= 3);
}
/* }}} */
#ifdef HAVE_XCACHE_DPRINT
/* {{{ proto bool  xcache_dprint(mixed value)
   Prints variable (or value) internal struct (debug only) */
PHP_FUNCTION(xcache_dprint)
{
	zval *value;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &value) == FAILURE) {
		return;
	}
	xc_dprint_zval(value, 0 TSRMLS_CC);
}
/* }}} */
#endif
/* {{{ proto string xcache_asm(string filename)
 */
#ifdef HAVE_XCACHE_ASSEMBLER
PHP_FUNCTION(xcache_asm)
{
}
#endif
/* }}} */
/* {{{ proto string xcache_encode(string filename)
   Encode php file into XCache opcode encoded format */
#ifdef HAVE_XCACHE_ENCODER
PHP_FUNCTION(xcache_encode)
{
}
#endif
/* }}} */
/* {{{ proto bool xcache_decode_file(string filename)
   Decode(load) opcode from XCache encoded format file */
#ifdef HAVE_XCACHE_DECODER
PHP_FUNCTION(xcache_decode_file)
{
}
#endif
/* }}} */
/* {{{ proto bool xcache_decode_string(string data)
   Decode(load) opcode from XCache encoded format data */
#ifdef HAVE_XCACHE_DECODER
PHP_FUNCTION(xcache_decode_string)
{
}
#endif
/* }}} */
/* {{{ xc_call_getter */
typedef const char *(xc_name_getter_t)(zend_uchar type);
static void xc_call_getter(xc_name_getter_t getter, int count, INTERNAL_FUNCTION_PARAMETERS)
{
	long spec;
	const char *name;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &spec) == FAILURE) {
		return;
	}
	if (spec >= 0 && spec < count) {
		name = getter((zend_uchar) spec);
		if (name) {
			/* RETURN_STRING */
			int len = strlen(name);
			return_value->value.str.len = len;
			return_value->value.str.val = estrndup(name, len);
			return_value->type = IS_STRING; 
			return;
		}
	}
	RETURN_NULL();
}
/* }}} */
/* {{{ proto string xcache_get_op_type(int op_type) */
PHP_FUNCTION(xcache_get_op_type)
{
	xc_call_getter(xc_get_op_type, xc_get_op_type_count(), INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */
/* {{{ proto string xcache_get_data_type(int type) */
PHP_FUNCTION(xcache_get_data_type)
{
	xc_call_getter(xc_get_data_type, xc_get_data_type_count(), INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */
/* {{{ proto string xcache_get_opcode(int opcode) */
PHP_FUNCTION(xcache_get_opcode)
{
	xc_call_getter(xc_get_opcode, xc_get_opcode_count(), INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */
/* {{{ proto string xcache_get_op_spec(int op_type) */
PHP_FUNCTION(xcache_get_op_spec)
{
	xc_call_getter(xc_get_op_spec, xc_get_op_spec_count(), INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */
/* {{{ proto string xcache_get_opcode_spec(int opcode) */
PHP_FUNCTION(xcache_get_opcode_spec)
{
	long spec;
	const xc_opcode_spec_t *opspec;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &spec) == FAILURE) {
		return;
	}
	if ((zend_uchar) spec <= xc_get_opcode_spec_count()) {
		opspec = xc_get_opcode_spec((zend_uchar) spec);
		if (opspec) {
			array_init(return_value);
			add_assoc_long_ex(return_value, ZEND_STRS("ext"), opspec->ext);
			add_assoc_long_ex(return_value, ZEND_STRS("op1"), opspec->op1);
			add_assoc_long_ex(return_value, ZEND_STRS("op2"), opspec->op2);
			add_assoc_long_ex(return_value, ZEND_STRS("res"), opspec->res);
			return;
		}
	}
	RETURN_NULL();
}
/* }}} */
/* {{{ proto mixed xcache_get_special_value(zval value)
   XCache internal use only: For decompiler to get static value with type fixed */
PHP_FUNCTION(xcache_get_special_value)
{
	zval *value;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &value) == FAILURE) {
		return;
	}

	switch ((Z_TYPE_P(value) & IS_CONSTANT_TYPE_MASK)) {
	case IS_CONSTANT:
		*return_value = *value;
		zval_copy_ctor(return_value);
		return_value->type = UNISW(IS_STRING, UG(unicode) ? IS_UNICODE : IS_STRING);
		break;

	case IS_CONSTANT_ARRAY:
		*return_value = *value;
		zval_copy_ctor(return_value);
		return_value->type = IS_ARRAY;
		break;

	default:
		RETURN_NULL();
	}
}
/* }}} */
/* {{{ proto int xcache_get_type(zval value)
   XCache internal use only for disassembler to get variable type in engine level */
PHP_FUNCTION(xcache_get_type)
{
	zval *value;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &value) == FAILURE) {
		return;
	}

	RETURN_LONG(Z_TYPE_P(value));
}
/* }}} */
/* {{{ proto string xcache_coredump(int op_type) */
PHP_FUNCTION(xcache_coredump)
{
	if (xc_test) {
		raise(SIGSEGV);
	}
	else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "xcache.test must be enabled to test xcache_coredump()");
	}
}
/* }}} */
/* {{{ proto string xcache_is_autoglobal(string name) */
PHP_FUNCTION(xcache_is_autoglobal)
{
	zval *name;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &name) == FAILURE) {
		return;
	}

#ifdef IS_UNICODE
	convert_to_unicode(name);
#else
	convert_to_string(name);
#endif

	RETURN_BOOL(zend_u_hash_exists(CG(auto_globals), UG(unicode), Z_STRVAL_P(name), Z_STRLEN_P(name) + 1));
}
/* }}} */
static zend_function_entry xcache_functions[] = /* {{{ */
{
	PHP_FE(xcache_count,             NULL)
	PHP_FE(xcache_info,              NULL)
	PHP_FE(xcache_list,              NULL)
	PHP_FE(xcache_clear_cache,       NULL)
	PHP_FE(xcache_coredump,          NULL)
#ifdef HAVE_XCACHE_ASSEMBLER
	PHP_FE(xcache_asm,               NULL)
#endif
#ifdef HAVE_XCACHE_ENCODER
	PHP_FE(xcache_encode,            NULL)
#endif
#ifdef HAVE_XCACHE_DECODER
	PHP_FE(xcache_decode_file,       NULL)
	PHP_FE(xcache_decode_string,     NULL)
#endif
	PHP_FE(xcache_get_special_value, NULL)
	PHP_FE(xcache_get_type,          NULL)
	PHP_FE(xcache_get_op_type,       NULL)
	PHP_FE(xcache_get_data_type,     NULL)
	PHP_FE(xcache_get_opcode,        NULL)
	PHP_FE(xcache_get_opcode_spec,   NULL)
	PHP_FE(xcache_is_autoglobal,     NULL)
	PHP_FE(xcache_inc,               NULL)
	PHP_FE(xcache_dec,               NULL)
	PHP_FE(xcache_get,               NULL)
	PHP_FE(xcache_set,               NULL)
	PHP_FE(xcache_isset,             NULL)
	PHP_FE(xcache_unset,             NULL)
	PHP_FE(xcache_unset_by_prefix,   NULL)
	PHP_FE(xcache_get_refcount,      NULL)
	PHP_FE(xcache_get_isref,         arginfo_xcache_get_isref)
#ifdef HAVE_XCACHE_DPRINT
	PHP_FE(xcache_dprint,            NULL)
#endif
	PHP_FE_END
};
/* }}} */

#ifdef ZEND_WIN32
#include "dbghelp.h"
typedef BOOL (WINAPI *MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType,
		CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
		CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
		CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam
		);

static PTOP_LEVEL_EXCEPTION_FILTER oldFilter = NULL;
static HMODULE dbghelpModule = NULL;
static char crash_dumpPath[_MAX_PATH] = { 0 };
static MINIDUMPWRITEDUMP dbghelp_MiniDumpWriteDump = NULL;

static LONG WINAPI miniDumperFilter(struct _EXCEPTION_POINTERS *pExceptionInfo) /* {{{ */
{
	HANDLE fileHandle;

	SetUnhandledExceptionFilter(oldFilter);

	/* create the file */
	fileHandle = CreateFile(crash_dumpPath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (fileHandle != INVALID_HANDLE_VALUE) {
		MINIDUMP_EXCEPTION_INFORMATION exceptionInformation;
		BOOL ok;

		exceptionInformation.ThreadId = GetCurrentThreadId();
		exceptionInformation.ExceptionPointers = pExceptionInfo;
		exceptionInformation.ClientPointers = FALSE;

		/* write the dump */
		ok = dbghelp_MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), fileHandle, MiniDumpNormal|MiniDumpWithDataSegs|MiniDumpWithIndirectlyReferencedMemory, &exceptionInformation, NULL, NULL);
		CloseHandle(fileHandle);
		if (ok) {
			zend_error(E_ERROR, "Saved dump file to '%s'", crash_dumpPath);
			return EXCEPTION_EXECUTE_HANDLER;
		}
		else {
			zend_error(E_ERROR, "Failed to save dump file to '%s' (error %d)", crash_dumpPath, GetLastError());
		}
	}
	else {
		zend_error(E_ERROR, "Failed to create dump file '%s' (error %d)", crash_dumpPath, GetLastError());
	}

	return EXCEPTION_CONTINUE_SEARCH;
}
/* }}} */

static void xcache_restore_crash_handler() /* {{{ */
{
	if (oldFilter) {
		SetUnhandledExceptionFilter(oldFilter);
		oldFilter = NULL;
	}
}
/* }}} */
static void xcache_init_crash_handler() /* {{{ */
{
	/* firstly see if dbghelp.dll is around and has the function we need
	   look next to the EXE first, as the one in System32 might be old
	   (e.g. Windows 2000) */
	char dbghelpPath[_MAX_PATH];

	if (GetModuleFileName(NULL, dbghelpPath, _MAX_PATH)) {
		char *slash = strchr(dbghelpPath, '\\');
		if (slash) {
			strcpy(slash + 1, "DBGHELP.DLL");
			dbghelpModule = LoadLibrary(dbghelpPath);
		}
	}

	if (!dbghelpModule) {
		/* load any version we can */
		dbghelpModule = LoadLibrary("DBGHELP.DLL");
		if (!dbghelpModule) {
			zend_error(E_ERROR, "Unable to enable crash dump saver: %s", "DBGHELP.DLL not found");
			return;
		}
	}

	dbghelp_MiniDumpWriteDump = (MINIDUMPWRITEDUMP)GetProcAddress(dbghelpModule, "MiniDumpWriteDump");
	if (!dbghelp_MiniDumpWriteDump) {
		zend_error(E_ERROR, "Unable to enable crash dump saver: %s", "DBGHELP.DLL too old. Get updated dll and put it aside of php_xcache.dll");
		return;
	}

#ifdef XCACHE_VERSION_REVISION
#define REVISION "r" XCACHE_VERSION_REVISION
#else
#define REVISION ""
#endif
	sprintf(crash_dumpPath, "%s\\php-%s-xcache-%s%s-%lu-%lu.dmp", xc_coredump_dir, zend_get_module_version("standard"), XCACHE_VERSION, REVISION, (unsigned long) time(NULL), (unsigned long) GetCurrentProcessId());
#undef REVISION

	oldFilter = SetUnhandledExceptionFilter(&miniDumperFilter);
}
/* }}} */
#else
/* old signal handlers {{{ */
typedef void (*xc_sighandler_t)(int);
#define FOREACH_SIG(sig) static xc_sighandler_t old_##sig##_handler = NULL
#include "util/xc_foreachcoresig.h"
#undef FOREACH_SIG
/* }}} */
static void xcache_signal_handler(int sig);
static void xcache_restore_crash_handler() /* {{{ */
{
#define FOREACH_SIG(sig) do { \
	if (old_##sig##_handler != xcache_signal_handler) { \
		signal(sig, old_##sig##_handler); \
	} \
	else { \
		signal(sig, SIG_DFL); \
	} \
} while (0)
#include "util/xc_foreachcoresig.h"
#undef FOREACH_SIG
}
/* }}} */
static void xcache_init_crash_handler() /* {{{ */
{
#define FOREACH_SIG(sig) \
	old_##sig##_handler = signal(sig, xcache_signal_handler)
#include "util/xc_foreachcoresig.h"
#undef FOREACH_SIG
}
/* }}} */
static void xcache_signal_handler(int sig) /* {{{ */
{
	xcache_restore_crash_handler();
	if (xc_coredump_dir && xc_coredump_dir[0]) {
		if (chdir(xc_coredump_dir) != 0) {
			/* error, but nothing can do about it
			 * and should'nt print anything which might SEGV again */
		}
	}
	if (xc_disable_on_crash) {
		xc_disable_on_crash = 0;
		if (xc_shm) {
			xc_shm->disabled = 1;
		}
	}
	raise(sig);
}
/* }}} */
#endif

static startup_func_t xc_last_ext_startup;
static int xc_zend_startup_last(zend_extension *extension) /* {{{ */
{
	zend_extension *ext = zend_get_extension(XCACHE_NAME);
	if (ext) {
		zend_error(E_WARNING, "Module '" XCACHE_NAME "' already loaded");
	}
	/* restore */
	extension->startup = xc_last_ext_startup;
	if (extension->startup) {
		if (extension->startup(extension) != SUCCESS) {
			return FAILURE;
		}
	}
	assert(xc_llist_zend_extension);
	xcache_llist_prepend(&zend_extensions, xc_llist_zend_extension);
	return SUCCESS;
}
/* }}} */
static int xc_zend_startup(zend_extension *extension) /* {{{ */
{
	if (!origin_compile_file) {
		origin_compile_file = zend_compile_file;
		zend_compile_file = xc_check_initial_compile_file;
	}

	if (zend_llist_count(&zend_extensions) > 1) {
		zend_llist_position lpos;
		zend_extension *ext;

		xc_llist_zend_extension = xcache_llist_get_element_by_zend_extension(&zend_extensions, XCACHE_NAME);
		if (xc_llist_zend_extension != zend_extensions.head) {
			zend_error(E_WARNING, "XCache failed to load itself as the first zend_extension. compatibility downgraded");
		}
		/* hide myself */
		xcache_llist_unlink(&zend_extensions, xc_llist_zend_extension);

		ext = (zend_extension *) zend_llist_get_last_ex(&zend_extensions, &lpos);
		assert(ext && ext != (zend_extension *) xc_llist_zend_extension->data);
		xc_last_ext_startup = ext->startup;
		ext->startup = xc_zend_startup_last;
	}
	return SUCCESS;
}
/* }}} */
static void xc_zend_shutdown(zend_extension *extension) /* {{{ */
{
}
/* }}} */
/* {{{ zend extension definition structure */
static zend_extension zend_extension_entry = {
	XCACHE_NAME,
	XCACHE_VERSION,
	XCACHE_AUTHOR,
	XCACHE_URL,
	XCACHE_COPYRIGHT,
	xc_zend_startup,
	xc_zend_shutdown,
	NULL,           /* activate_func_t */
	NULL,           /* deactivate_func_t */
	NULL,           /* message_handler_func_t */
	NULL,           /* op_array_handler_func_t */
	NULL,           /* statement_handler_func_t */
	NULL,           /* fcall_begin_handler_func_t */
	NULL,           /* fcall_end_handler_func_t */
	NULL,           /* op_array_ctor_func_t */
	NULL,           /* op_array_dtor_func_t */
	STANDARD_ZEND_EXTENSION_PROPERTIES
};
/* }}} */

/* {{{ PHP_INI */
#ifdef ZEND_WIN32
#	define DEFAULT_PATH "xcache"
#else
#	define DEFAULT_PATH "/dev/zero"
#endif
PHP_INI_BEGIN()
	PHP_INI_ENTRY1     ("xcache.mmap_path",     DEFAULT_PATH, PHP_INI_SYSTEM, xcache_OnUpdateString,   &xc_mmap_path)
	PHP_INI_ENTRY1     ("xcache.coredump_directory",      "", PHP_INI_SYSTEM, xcache_OnUpdateString,   &xc_coredump_dir)
	PHP_INI_ENTRY1     ("xcache.disable_on_crash",       "0", PHP_INI_SYSTEM, xcache_OnUpdateBool,     &xc_disable_on_crash)
	PHP_INI_ENTRY1     ("xcache.test",                   "0", PHP_INI_SYSTEM, xcache_OnUpdateBool,     &xc_test)
	PHP_INI_ENTRY1     ("xcache.readonly_protection",    "0", PHP_INI_SYSTEM, xcache_OnUpdateBool,     &xc_readonly_protection)
	/* opcode cache */
	PHP_INI_ENTRY1     ("xcache.size",                   "0", PHP_INI_SYSTEM, xcache_OnUpdateDummy,    NULL)
	PHP_INI_ENTRY1     ("xcache.count",                  "1", PHP_INI_SYSTEM, xcache_OnUpdateDummy,    NULL)
	PHP_INI_ENTRY1     ("xcache.slots",                 "8K", PHP_INI_SYSTEM, xcache_OnUpdateDummy,    NULL)
	PHP_INI_ENTRY1     ("xcache.shm_scheme",          "mmap", PHP_INI_SYSTEM, xcache_OnUpdateString,   &xc_shm_scheme)
	PHP_INI_ENTRY1     ("xcache.ttl",                    "0", PHP_INI_SYSTEM, xcache_OnUpdateULong,    &xc_php_ttl)
	PHP_INI_ENTRY1     ("xcache.gc_interval",            "0", PHP_INI_SYSTEM, xcache_OnUpdateULong,    &xc_php_gc_interval)
	/* var cache */
	PHP_INI_ENTRY1     ("xcache.var_size",               "0", PHP_INI_SYSTEM, xcache_OnUpdateDummy,    NULL)
	PHP_INI_ENTRY1     ("xcache.var_count",              "1", PHP_INI_SYSTEM, xcache_OnUpdateDummy,    NULL)
	PHP_INI_ENTRY1     ("xcache.var_slots",             "8K", PHP_INI_SYSTEM, xcache_OnUpdateDummy,    NULL)
	PHP_INI_ENTRY1     ("xcache.var_maxttl",             "0", PHP_INI_SYSTEM, xcache_OnUpdateULong,    &xc_var_maxttl)
	PHP_INI_ENTRY1     ("xcache.var_gc_interval",      "120", PHP_INI_SYSTEM, xcache_OnUpdateULong,    &xc_var_gc_interval)

	STD_PHP_INI_BOOLEAN("xcache.cacher",                 "1", PHP_INI_ALL,    OnUpdateBool,        cacher,            zend_xcache_globals, xcache_globals)
	STD_PHP_INI_BOOLEAN("xcache.stat",                   "1", PHP_INI_ALL,    OnUpdateBool,        stat,              zend_xcache_globals, xcache_globals)
	STD_PHP_INI_BOOLEAN("xcache.admin.enable_auth",      "1", PHP_INI_SYSTEM, OnUpdateBool,        auth_enabled,      zend_xcache_globals, xcache_globals)
	STD_PHP_INI_BOOLEAN("xcache.experimental",           "0", PHP_INI_ALL,    OnUpdateBool,        experimental,      zend_xcache_globals, xcache_globals)
	STD_PHP_INI_ENTRY  ("xcache.var_ttl",                "0", PHP_INI_ALL,    OnUpdateLong,        var_ttl,           zend_xcache_globals, xcache_globals)
PHP_INI_END()
/* }}} */
static PHP_MINFO_FUNCTION(xcache) /* {{{ */
{
	char buf[100];
	char *ptr;
	int left, len;
	xc_shm_scheme_t *scheme;

	php_info_print_table_start();
	php_info_print_table_row(2, "XCache Version", XCACHE_VERSION);
#ifdef XCACHE_VERSION_REVISION
	php_info_print_table_row(2, "Revision", "r" XCACHE_VERSION_REVISION);
#endif
	php_info_print_table_row(2, "Modules Built", XCACHE_MODULES);
	php_info_print_table_row(2, "Readonly Protection", xc_readonly_protection ? "enabled" : "disabled");
#ifdef ZEND_ENGINE_2_1
	ptr = php_format_date("Y-m-d H:i:s", sizeof("Y-m-d H:i:s") - 1, xc_init_time, 1 TSRMLS_CC);
	php_info_print_table_row(2, "Cache Init Time", ptr);
	efree(ptr);
#else
	snprintf(buf, sizeof(buf), "%lu", (long unsigned) xc_init_time);
	php_info_print_table_row(2, "Cache Init Time", buf);
#endif

#ifdef ZTS
	snprintf(buf, sizeof(buf), "%lu.%lu", xc_init_instance_id, xc_init_instance_subid);
#else
	snprintf(buf, sizeof(buf), "%lu", xc_init_instance_id);
#endif
	php_info_print_table_row(2, "Cache Instance Id", buf);

	if (xc_php_size) {
		ptr = _php_math_number_format(xc_php_size, 0, '.', ',');
		snprintf(buf, sizeof(buf), "enabled, %s bytes, %lu split(s), with %lu slots each", ptr, xc_php_hcache.size, xc_php_hentry.size);
		php_info_print_table_row(2, "Opcode Cache", buf);
		efree(ptr);
	}
	else {
		php_info_print_table_row(2, "Opcode Cache", "disabled");
	}
	if (xc_var_size) {
		ptr = _php_math_number_format(xc_var_size, 0, '.', ',');
		snprintf(buf, sizeof(buf), "enabled, %s bytes, %lu split(s), with %lu slots each", ptr, xc_var_hcache.size, xc_var_hentry.size);
		php_info_print_table_row(2, "Variable Cache", buf);
		efree(ptr);
	}
	else {
		php_info_print_table_row(2, "Variable Cache", "disabled");
	}

	left = sizeof(buf);
	ptr = buf;
	buf[0] = '\0';
	for (scheme = xc_shm_scheme_first(); scheme; scheme = xc_shm_scheme_next(scheme)) {
		len = snprintf(ptr, left, ptr == buf ? "%s" : ", %s", xc_shm_scheme_name(scheme));
		left -= len;
		ptr += len;
	}
	php_info_print_table_row(2, "Shared Memory Schemes", buf);

	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */
static int xc_config_hash(xc_hash_t *p, char *name, char *default_value) /* {{{ */
{
	size_t bits, size;
	char *value;

	if (cfg_get_string(name, &value) != SUCCESS) {
		value = default_value;
	}

	p->size = zend_atoi(value, strlen(value));
	for (size = 1, bits = 1; size < p->size; bits ++, size <<= 1) {
		/* empty body */
	}
	p->size = size;
	p->bits = bits;
	p->mask = size - 1;

	return SUCCESS;
}
/* }}} */
static int xc_config_long(zend_ulong *p, char *name, char *default_value) /* {{{ */
{
	char *value;

	if (cfg_get_string(name, &value) != SUCCESS) {
		value = default_value;
	}

	*p = zend_atol(value, strlen(value));
	return SUCCESS;
}
/* }}} */
static PHP_MINIT_FUNCTION(xcache) /* {{{ */
{
	char *env;
	zend_extension *ext;
	zend_llist_position lpos;

	xcache_zend_extension_register(&zend_extension_entry, 1);
	ext = zend_get_extension("Zend Optimizer");
	if (ext) {
		/* zend_optimizer.optimization_level>0 is not compatible with other cacher, disabling */
		ext->op_array_handler = NULL;
	}
	/* cache if there's an op_array_ctor */
	for (ext = zend_llist_get_first_ex(&zend_extensions, &lpos);
			ext;
			ext = zend_llist_get_next_ex(&zend_extensions, &lpos)) {
		if (ext->op_array_ctor) {
			xc_have_op_array_ctor = 1;
			break;
		}
	}


#ifndef PHP_GINIT
	ZEND_INIT_MODULE_GLOBALS(xcache, xc_init_globals, xc_shutdown_globals);
#endif
	REGISTER_INI_ENTRIES();

	/* additional_functions requires PHP 5.3. TODO: find simpler way to do it */
#ifdef ZEND_ENGINE_2_3
	if (strcmp(sapi_module.name, "cgi-fcgi") == 0 && !sapi_module.additional_functions && !getenv("XCACHE_SKIP_FCGI_WARNING") && !getenv("GATEWAY_INTERFACE")) {
		if ((getenv("PHP_FCGI_CHILDREN") == NULL) || (atoi(getenv("PHP_FCGI_CHILDREN")) < 1)) {
			zend_error(E_WARNING, "PHP_FCGI_CHILDREN should be >= 1 and use 1 group of parent/childs model. Set XCACHE_SKIP_FCGI_WARNING=1 to skip this warning. See " XCACHE_WIKI_URL "/Faq");
		}
	}
#endif

	xc_config_long(&xc_php_size,       "xcache.size",        "0");
	xc_config_hash(&xc_php_hcache,     "xcache.count",       "1");
	xc_config_hash(&xc_php_hentry,     "xcache.slots",      "8K");

	xc_config_long(&xc_var_size,       "xcache.var_size",    "0");
	xc_config_hash(&xc_var_hcache,     "xcache.var_count",   "1");
	xc_config_hash(&xc_var_hentry,     "xcache.var_slots",  "8K");

	if (strcmp(sapi_module.name, "cli") == 0) {
		if ((env = getenv("XCACHE_TEST")) != NULL) {
			xc_test = atoi(env);
		}
		if (!xc_test) {
			/* disable cache for cli except for testing */
			xc_php_size = xc_var_size = 0;
		}
	}

	if (xc_php_size <= 0) {
		xc_php_size = xc_php_hcache.size = 0;
	}
	if (xc_var_size <= 0) {
		xc_var_size = xc_var_hcache.size = 0;
	}

	if (xc_coredump_dir && xc_coredump_dir[0]) {
		xcache_init_crash_handler();
	}

	xc_init_constant(module_number TSRMLS_CC);
	xc_shm_init_modules();

	if ((xc_php_size || xc_var_size) && xc_mmap_path && xc_mmap_path[0]) {
		if (xc_init(module_number TSRMLS_CC) != SUCCESS) {
			zend_error(E_ERROR, "XCache: Cannot init");
			goto err_init;
		}
		xc_initized = 1;
		xc_init_time = time(NULL);
#ifdef PHP_WIN32
		xc_init_instance_id = GetCurrentProcessId();
#else
		xc_init_instance_id = getpid();
#endif
#ifdef ZTS
		xc_init_instance_subid = tsrm_thread_id();
#endif
	}

#ifdef HAVE_XCACHE_OPTIMIZER
	xc_optimizer_startup_module();
#endif
#ifdef HAVE_XCACHE_COVERAGER
	xc_coverager_startup_module();
#endif
#ifdef HAVE_XCACHE_DISASSEMBLER
	xc_disassembler_startup_module();
#endif
	xc_sandbox_module_init(module_number TSRMLS_CC);
	return SUCCESS;

err_init:
	return FAILURE;
}
/* }}} */
/* {{{ PHP_MSHUTDOWN_FUNCTION(xcache) */
static PHP_MSHUTDOWN_FUNCTION(xcache)
{
	xc_sandbox_module_shutdown();

	if (xc_initized) {
		xc_destroy();
	}
	if (xc_mmap_path) {
		pefree(xc_mmap_path, 1);
		xc_mmap_path = NULL;
	}
	if (xc_shm_scheme) {
		pefree(xc_shm_scheme, 1);
		xc_shm_scheme = NULL;
	}

	if (xc_coredump_dir && xc_coredump_dir[0]) {
		xcache_restore_crash_handler();
	}
	if (xc_coredump_dir) {
		pefree(xc_coredump_dir, 1);
		xc_coredump_dir = NULL;
	}
#ifndef PHP_GINIT
#	ifdef ZTS
	ts_free_id(xcache_globals_id);
#	else
	xc_shutdown_globals(&xcache_globals TSRMLS_CC);
#	endif
#endif

	xcache_zend_extension_unregister(&zend_extension_entry);
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}
/* }}} */
/* {{{ PHP_RINIT_FUNCTION(xcache) */
static PHP_RINIT_FUNCTION(xcache)
{
	xc_request_init(TSRMLS_C);
	return SUCCESS;
}
/* }}} */
/* {{{ PHP_RSHUTDOWN_FUNCTION(xcache) */
#ifndef ZEND_ENGINE_2
static PHP_RSHUTDOWN_FUNCTION(xcache)
#else
static ZEND_MODULE_POST_ZEND_DEACTIVATE_D(xcache)
#endif
{
#ifdef ZEND_ENGINE_2
	TSRMLS_FETCH();
#endif

	xc_request_shutdown(TSRMLS_C);
	return SUCCESS;
}
/* }}} */
/* {{{ module dependencies */
#ifdef STANDARD_MODULE_HEADER_EX
static zend_module_dep xcache_module_deps[] = {
	ZEND_MOD_REQUIRED("standard")
	ZEND_MOD_CONFLICTS("apc")
	ZEND_MOD_CONFLICTS("eAccelerator")
	ZEND_MOD_CONFLICTS("Turck MMCache")
	ZEND_MOD_END
};
#endif
/* }}} */ 
/* {{{ module definition structure */
zend_module_entry xcache_module_entry = {
#ifdef STANDARD_MODULE_HEADER_EX
	STANDARD_MODULE_HEADER_EX,
	NULL,
	xcache_module_deps,
#else
	STANDARD_MODULE_HEADER,
#endif
	XCACHE_NAME,
	xcache_functions,
	PHP_MINIT(xcache),
	PHP_MSHUTDOWN(xcache),
	PHP_RINIT(xcache),
#ifndef ZEND_ENGINE_2
	PHP_RSHUTDOWN(xcache),
#else
	NULL,
#endif
	PHP_MINFO(xcache),
	XCACHE_VERSION,
#ifdef PHP_GINIT
	PHP_MODULE_GLOBALS(xcache),
	PHP_GINIT(xcache),
	PHP_GSHUTDOWN(xcache),
#endif
#ifdef ZEND_ENGINE_2
	ZEND_MODULE_POST_ZEND_DEACTIVATE_N(xcache),
#else
	NULL,
	NULL,
#endif
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_XCACHE
ZEND_GET_MODULE(xcache)
#endif
/* }}} */
