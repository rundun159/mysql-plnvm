/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0flu.cc
The database buffer buf_pool flush algorithm

Created 11/11/1995 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"
#include <mysql/service_thd_wait.h>
#include <my_dbug.h>

#include "buf0flu.h"

#ifdef UNIV_NONINL
#include "buf0flu.ic"
#endif

#include "buf0buf.h"
#include "buf0checksum.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#ifndef UNIV_HOTBACKUP
#include "ut0byte.h"
#include "page0page.h"
#include "fil0fil.h"
#include "buf0lru.h"
#include "buf0rea.h"
#include "ibuf0ibuf.h"
#include "log0log.h"
#include "os0file.h"
#include "trx0sys.h"
#include "srv0mon.h"
#include "fsp0sysspace.h"
#include "ut0stage.h"

#ifdef UNIV_LINUX
/* include defs for CPU time priority settings */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/resource.h>
static const int buf_flush_page_cleaner_priority = -20;
#endif /* UNIV_LINUX */

#if defined(UNIV_TRACE_FLUSH_TIME)
extern ulint gb_flush_time;
#endif
#if defined(UNIV_PMEMOBJ_BUF) || defined (UNIV_PMEMOBJ_PART_PL)
#include <sys/syscall.h>
#include <sys/types.h> //for gettid()
#include "my_pmemobj.h"
#include <libpmemobj.h>
extern PMEM_WRAPPER* gb_pmw;
static const int buf_flusher_priority = -20;
#endif /* UNIV_PMEMOBJ_BUF */

/** Sleep time in microseconds for loop waiting for the oldest
modification lsn */
static const ulint buf_flush_wait_flushed_sleep_time = 10000;

/** Number of pages flushed through non flush_list flushes. */
static ulint buf_lru_flush_page_count = 0;

/** Flag indicating if the page_cleaner is in active state. This flag
is set to TRUE by the page_cleaner thread when it is spawned and is set
back to FALSE at shutdown by the page_cleaner as well. Therefore no
need to protect it by a mutex. It is only ever read by the thread
doing the shutdown */
bool buf_page_cleaner_is_active = false;

/** Factor for scan length to determine n_pages for intended oldest LSN
progress */
static ulint buf_flush_lsn_scan_factor = 3;

/** Average redo generation rate */
static lsn_t lsn_avg_rate = 0;

/** Target oldest LSN for the requested flush_sync */
static lsn_t buf_flush_sync_lsn = 0;

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t page_cleaner_thread_key;
#endif /* UNIV_PFS_THREAD */

/** Event to synchronise with the flushing. */
os_event_t	buf_flush_event;

/** State for page cleaner array slot */
enum page_cleaner_state_t {
	/** Not requested any yet.
	Moved from FINISHED by the coordinator. */
	PAGE_CLEANER_STATE_NONE = 0,
	/** Requested but not started flushing.
	Moved from NONE by the coordinator. */
	PAGE_CLEANER_STATE_REQUESTED,
	/** Flushing is on going.
	Moved from REQUESTED by the worker. */
	PAGE_CLEANER_STATE_FLUSHING,
	/** Flushing was finished.
	Moved from FLUSHING by the worker. */
	PAGE_CLEANER_STATE_FINISHED
};

/** Page cleaner request state for each buffer pool instance */
struct page_cleaner_slot_t {
	page_cleaner_state_t	state;	/*!< state of the request.
					protected by page_cleaner_t::mutex
					if the worker thread got the slot and
					set to PAGE_CLEANER_STATE_FLUSHING,
					n_flushed_lru and n_flushed_list can be
					updated only by the worker thread */
	/* This value is set during state==PAGE_CLEANER_STATE_NONE */
	ulint			n_pages_requested;
					/*!< number of requested pages
					for the slot */
	/* These values are updated during state==PAGE_CLEANER_STATE_FLUSHING,
	and commited with state==PAGE_CLEANER_STATE_FINISHED.
	The consistency is protected by the 'state' */
	ulint			n_flushed_lru;
					/*!< number of flushed pages
					by LRU scan flushing */
	ulint			n_flushed_list;
					/*!< number of flushed pages
					by flush_list flushing */
	bool			succeeded_list;
					/*!< true if flush_list flushing
					succeeded. */
	ulint			flush_lru_time;
					/*!< elapsed time for LRU flushing */
	ulint			flush_list_time;
					/*!< elapsed time for flush_list
					flushing */
	ulint			flush_lru_pass;
					/*!< count to attempt LRU flushing */
	ulint			flush_list_pass;
					/*!< count to attempt flush_list
					flushing */
};

/** Page cleaner structure common for all threads */
struct page_cleaner_t {
	ib_mutex_t		mutex;		/*!< mutex to protect whole of
						page_cleaner_t struct and
						page_cleaner_slot_t slots. */
	os_event_t		is_requested;	/*!< event to activate worker
						threads. */
	os_event_t		is_finished;	/*!< event to signal that all
						slots were finished. */
	volatile ulint		n_workers;	/*!< number of worker threads
						in existence */
	bool			requested;	/*!< true if requested pages
						to flush */
	lsn_t			lsn_limit;	/*!< upper limit of LSN to be
						flushed */
	ulint			n_slots;	/*!< total number of slots */
	ulint			n_slots_requested;
						/*!< number of slots
						in the state
						PAGE_CLEANER_STATE_REQUESTED */
	ulint			n_slots_flushing;
						/*!< number of slots
						in the state
						PAGE_CLEANER_STATE_FLUSHING */
	ulint			n_slots_finished;
						/*!< number of slots
						in the state
						PAGE_CLEANER_STATE_FINISHED */
	ulint			flush_time;	/*!< elapsed time to flush
						requests for all slots */
	ulint			flush_pass;	/*!< count to finish to flush
						requests for all slots */
	page_cleaner_slot_t*	slots;		/*!< pointer to the slots */
	bool			is_running;	/*!< false if attempt
						to shutdown */

#ifdef UNIV_DEBUG
	ulint			n_disabled_debug;
						/*<! how many of pc threads
						have been disabled */
#endif /* UNIV_DEBUG */
};

static page_cleaner_t*	page_cleaner = NULL;

#ifdef UNIV_DEBUG
my_bool innodb_page_cleaner_disabled_debug;
#endif /* UNIV_DEBUG */

/** If LRU list of a buf_pool is less than this size then LRU eviction
should not happen. This is because when we do LRU flushing we also put
the blocks on free list. If LRU list is very small then we can end up
in thrashing. */
#define BUF_LRU_MIN_LEN		256

/* @} */

/******************************************************************//**
Increases flush_list size in bytes with the page size in inline function */
static inline
void
incr_flush_list_size_in_bytes(
/*==========================*/
	buf_block_t*	block,		/*!< in: control block */
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instance */
{
	ut_ad(buf_flush_list_mutex_own(buf_pool));

	buf_pool->stat.flush_list_bytes += block->page.size.physical();

	ut_ad(buf_pool->stat.flush_list_bytes <= buf_pool->curr_pool_size);
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/******************************************************************//**
Validates the flush list.
@return TRUE if ok */
static
ibool
buf_flush_validate_low(
/*===================*/
	buf_pool_t*	buf_pool);	/*!< in: Buffer pool instance */

/******************************************************************//**
Validates the flush list some of the time.
@return TRUE if ok or the check was skipped */
static
ibool
buf_flush_validate_skip(
/*====================*/
	buf_pool_t*	buf_pool)	/*!< in: Buffer pool instance */
{
/** Try buf_flush_validate_low() every this many times */
# define BUF_FLUSH_VALIDATE_SKIP	23

	/** The buf_flush_validate_low() call skip counter.
	Use a signed type because of the race condition below. */
	static int buf_flush_validate_count = BUF_FLUSH_VALIDATE_SKIP;

	/* There is a race condition below, but it does not matter,
	because this call is only for heuristic purposes. We want to
	reduce the call frequency of the costly buf_flush_validate_low()
	check in debug builds. */
	if (--buf_flush_validate_count > 0) {
		return(TRUE);
	}

	buf_flush_validate_count = BUF_FLUSH_VALIDATE_SKIP;
	return(buf_flush_validate_low(buf_pool));
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/******************************************************************//**
Insert a block in the flush_rbt and returns a pointer to its
predecessor or NULL if no predecessor. The ordering is maintained
on the basis of the <oldest_modification, space, offset> key.
@return pointer to the predecessor or NULL if no predecessor. */
static
buf_page_t*
buf_flush_insert_in_flush_rbt(
/*==========================*/
	buf_page_t*	bpage)	/*!< in: bpage to be inserted. */
{
	const ib_rbt_node_t*	c_node;
	const ib_rbt_node_t*	p_node;
	buf_page_t*		prev = NULL;
	buf_pool_t*		buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_flush_list_mutex_own(buf_pool));

	/* Insert this buffer into the rbt. */
	c_node = rbt_insert(buf_pool->flush_rbt, &bpage, &bpage);
	ut_a(c_node != NULL);

	/* Get the predecessor. */
	p_node = rbt_prev(buf_pool->flush_rbt, c_node);

	if (p_node != NULL) {
		buf_page_t**	value;
		value = rbt_value(buf_page_t*, p_node);
		prev = *value;
		ut_a(prev != NULL);
	}

	return(prev);
}

/*********************************************************//**
Delete a bpage from the flush_rbt. */
static
void
buf_flush_delete_from_flush_rbt(
/*============================*/
	buf_page_t*	bpage)	/*!< in: bpage to be removed. */
{
#ifdef UNIV_DEBUG
	ibool		ret = FALSE;
#endif /* UNIV_DEBUG */
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_flush_list_mutex_own(buf_pool));

#ifdef UNIV_DEBUG
	ret =
#endif /* UNIV_DEBUG */
	rbt_delete(buf_pool->flush_rbt, &bpage);

	ut_ad(ret);
}

/*****************************************************************//**
Compare two modified blocks in the buffer pool. The key for comparison
is:
key = <oldest_modification, space, offset>
This comparison is used to maintian ordering of blocks in the
buf_pool->flush_rbt.
Note that for the purpose of flush_rbt, we only need to order blocks
on the oldest_modification. The other two fields are used to uniquely
identify the blocks.
@return < 0 if b2 < b1, 0 if b2 == b1, > 0 if b2 > b1 */
static
int
buf_flush_block_cmp(
/*================*/
	const void*	p1,		/*!< in: block1 */
	const void*	p2)		/*!< in: block2 */
{
	int			ret;
	const buf_page_t*	b1 = *(const buf_page_t**) p1;
	const buf_page_t*	b2 = *(const buf_page_t**) p2;

	ut_ad(b1 != NULL);
	ut_ad(b2 != NULL);

#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_bpage(b1);
#endif /* UNIV_DEBUG */

	ut_ad(buf_flush_list_mutex_own(buf_pool));

	ut_ad(b1->in_flush_list);
	ut_ad(b2->in_flush_list);

	if (b2->oldest_modification > b1->oldest_modification) {
		return(1);
	} else if (b2->oldest_modification < b1->oldest_modification) {
		return(-1);
	}

	/* If oldest_modification is same then decide on the space. */
	ret = (int)(b2->id.space() - b1->id.space());

	/* Or else decide ordering on the page number. */
	return(ret ? ret : (int) (b2->id.page_no() - b1->id.page_no()));
}

/********************************************************************//**
Initialize the red-black tree to speed up insertions into the flush_list
during recovery process. Should be called at the start of recovery
process before any page has been read/written. */
void
buf_flush_init_flush_rbt(void)
/*==========================*/
{
	ulint	i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_flush_list_mutex_enter(buf_pool);

		ut_ad(buf_pool->flush_rbt == NULL);

		/* Create red black tree for speedy insertions in flush list. */
		buf_pool->flush_rbt = rbt_create(
			sizeof(buf_page_t*), buf_flush_block_cmp);

		buf_flush_list_mutex_exit(buf_pool);
	}
}

/********************************************************************//**
Frees up the red-black tree. */
void
buf_flush_free_flush_rbt(void)
/*==========================*/
{
	ulint	i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_flush_list_mutex_enter(buf_pool);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		ut_a(buf_flush_validate_low(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

		rbt_free(buf_pool->flush_rbt);
		buf_pool->flush_rbt = NULL;

		buf_flush_list_mutex_exit(buf_pool);
	}
}

/********************************************************************//**
Inserts a modified block into the flush list. */
void
buf_flush_insert_into_flush_list(
/*=============================*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_block_t*	block,		/*!< in/out: block which is modified */
	lsn_t		lsn)		/*!< in: oldest modification */
{
	ut_ad(!buf_pool_mutex_own(buf_pool));
	ut_ad(log_flush_order_mutex_own());
	ut_ad(buf_page_mutex_own(block));

	buf_flush_list_mutex_enter(buf_pool);

#if defined (UNIV_PMEMOBJ_PART_PL)
	/*In PPL, the lsn ordering only guarantee inside a local partition log, not in global order.
	 * Therefore, the lsn ordering in buf_pool->flush_list is not correct*/
	
	//we don't check the order here
	buf_page_t* first_page = UT_LIST_GET_FIRST(buf_pool->flush_list);
#else //original
	ut_ad((UT_LIST_GET_FIRST(buf_pool->flush_list) == NULL)
	      || (UT_LIST_GET_FIRST(buf_pool->flush_list)->oldest_modification
		  <= lsn));
#endif


	/* If we are in the recovery then we need to update the flush
	red-black tree as well. */
	if (buf_pool->flush_rbt != NULL) {
		buf_flush_list_mutex_exit(buf_pool);
		buf_flush_insert_sorted_into_flush_list(buf_pool, block, lsn);
		return;
	}

	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(!block->page.in_flush_list);

	ut_d(block->page.in_flush_list = TRUE);
	block->page.oldest_modification = lsn;

	UT_LIST_ADD_FIRST(buf_pool->flush_list, &block->page);
//#if defined (UNIV_PMEMOBJ_PART_PL)
//	printf("buf_flush_insert_into_flush_list() space %zu page_no %zu oldest_lsn %zu\n", 
//			block->page.id.space(), block->page.id.page_no(),
//			block->page.oldest_modification);
//#endif
	incr_flush_list_size_in_bytes(block, buf_pool);

#ifdef UNIV_DEBUG_VALGRIND
	void*	p;

	if (block->page.size.is_compressed()) {
		p = block->page.zip.data;
	} else {
		p = block->frame;
	}

	UNIV_MEM_ASSERT_RW(p, block->page.size.physical());
#endif /* UNIV_DEBUG_VALGRIND */

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_skip(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_flush_list_mutex_exit(buf_pool);
}

/********************************************************************//**
Inserts a modified block into the flush list in the right sorted position.
This function is used by recovery, because there the modifications do not
necessarily come in the order of lsn's. */
void
buf_flush_insert_sorted_into_flush_list(
/*====================================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_block_t*	block,		/*!< in/out: block which is modified */
	lsn_t		lsn)		/*!< in: oldest modification */
{
	buf_page_t*	prev_b;
	buf_page_t*	b;

	ut_ad(!buf_pool_mutex_own(buf_pool));
	ut_ad(log_flush_order_mutex_own());
	ut_ad(buf_page_mutex_own(block));
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	buf_flush_list_mutex_enter(buf_pool);

	/* The field in_LRU_list is protected by buf_pool->mutex, which
	we are not holding.  However, while a block is in the flush
	list, it is dirty and cannot be discarded, not from the
	page_hash or from the LRU list.  At most, the uncompressed
	page frame of a compressed block may be discarded or created
	(copying the block->page to or from a buf_page_t that is
	dynamically allocated from buf_buddy_alloc()).  Because those
	transitions hold block->mutex and the flush list mutex (via
	buf_flush_relocate_on_flush_list()), there is no possibility
	of a race condition in the assertions below. */
	ut_ad(block->page.in_LRU_list);
	ut_ad(block->page.in_page_hash);
	/* buf_buddy_block_register() will take a block in the
	BUF_BLOCK_MEMORY state, not a file page. */
	ut_ad(!block->page.in_zip_hash);

	ut_ad(!block->page.in_flush_list);
	ut_d(block->page.in_flush_list = TRUE);
	block->page.oldest_modification = lsn;

#ifdef UNIV_DEBUG_VALGRIND
	void*	p;

	if (block->page.size.is_compressed()) {
		p = block->page.zip.data;
	} else {
		p = block->frame;
	}

	UNIV_MEM_ASSERT_RW(p, block->page.size.physical());
#endif /* UNIV_DEBUG_VALGRIND */

	prev_b = NULL;

	/* For the most part when this function is called the flush_rbt
	should not be NULL. In a very rare boundary case it is possible
	that the flush_rbt has already been freed by the recovery thread
	before the last page was hooked up in the flush_list by the
	io-handler thread. In that case we'll just do a simple
	linear search in the else block. */
	if (buf_pool->flush_rbt != NULL) {

		prev_b = buf_flush_insert_in_flush_rbt(&block->page);

	} else {

		b = UT_LIST_GET_FIRST(buf_pool->flush_list);

		while (b != NULL && b->oldest_modification
		       > block->page.oldest_modification) {

			ut_ad(b->in_flush_list);
			prev_b = b;
			b = UT_LIST_GET_NEXT(list, b);
		}
	}

	if (prev_b == NULL) {
		UT_LIST_ADD_FIRST(buf_pool->flush_list, &block->page);
	} else {
		UT_LIST_INSERT_AFTER(buf_pool->flush_list, prev_b, &block->page);
	}

	incr_flush_list_size_in_bytes(block, buf_pool);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_low(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_flush_list_mutex_exit(buf_pool);
}

/********************************************************************//**
Returns TRUE if the file page block is immediately suitable for replacement,
i.e., the transition FILE_PAGE => NOT_USED allowed.
@return TRUE if can replace immediately */
ibool
buf_flush_ready_for_replace(
/*========================*/
	buf_page_t*	bpage)	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) and in the LRU list */
{
#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	ut_ad(buf_pool_mutex_own(buf_pool));
#endif /* UNIV_DEBUG */
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(bpage->in_LRU_list);

	if (buf_page_in_file(bpage)) {

		return(bpage->oldest_modification == 0
		       && bpage->buf_fix_count == 0
		       && buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	}

	ib::fatal() << "Buffer block " << bpage << " state " <<  bpage->state
		<< " in the LRU list!";

	return(FALSE);
}

/********************************************************************//**
Returns true if the block is modified and ready for flushing.
@return true if can flush immediately */
bool
buf_flush_ready_for_flush(
/*======================*/
	buf_page_t*	bpage,	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) */
	buf_flush_t	flush_type)/*!< in: type of flush */
{
#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	ut_ad(buf_pool_mutex_own(buf_pool));
#endif /* UNIV_DEBUG */

	ut_a(buf_page_in_file(bpage));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(flush_type < BUF_FLUSH_N_TYPES);

	if (bpage->oldest_modification == 0
	    || buf_page_get_io_fix(bpage) != BUF_IO_NONE) {
		return(false);
	}

	ut_ad(bpage->in_flush_list);

	switch (flush_type) {
	case BUF_FLUSH_LIST:
	case BUF_FLUSH_LRU:
	case BUF_FLUSH_SINGLE_PAGE:
		return(true);

	case BUF_FLUSH_N_TYPES:
		break;
	}

	ut_error;
	return(false);
}

/********************************************************************//**
Remove a block from the flush list of modified blocks. */
void
buf_flush_remove(
/*=============*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(bpage->in_flush_list);

	buf_flush_list_mutex_enter(buf_pool);

	/* Important that we adjust the hazard pointer before removing
	the bpage from flush list. */
	buf_pool->flush_hp.adjust(bpage);

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_POOL_WATCH:
	case BUF_BLOCK_ZIP_PAGE:
		/* Clean compressed pages should not be on the flush list */
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		return;
	case BUF_BLOCK_ZIP_DIRTY:
		buf_page_set_state(bpage, BUF_BLOCK_ZIP_PAGE);
		UT_LIST_REMOVE(buf_pool->flush_list, bpage);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		buf_LRU_insert_zip_clean(bpage);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		break;
	case BUF_BLOCK_FILE_PAGE:
		UT_LIST_REMOVE(buf_pool->flush_list, bpage);
		break;
	}

	/* If the flush_rbt is active then delete from there as well. */
	if (buf_pool->flush_rbt != NULL) {
		buf_flush_delete_from_flush_rbt(bpage);
	}

	/* Must be done after we have removed it from the flush_rbt
	because we assert on in_flush_list in comparison function. */
	ut_d(bpage->in_flush_list = FALSE);

	buf_pool->stat.flush_list_bytes -= bpage->size.physical();

	bpage->oldest_modification = 0;

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_skip(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	/* If there is an observer that want to know if the asynchronous
	flushing was done then notify it. */
	if (bpage->flush_observer != NULL) {
		bpage->flush_observer->notify_remove(buf_pool, bpage);

		bpage->flush_observer = NULL;
	}

	buf_flush_list_mutex_exit(buf_pool);
}

/*******************************************************************//**
Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage have already been
copied to dpage.
IMPORTANT: When this function is called bpage and dpage are not
exact copies of each other. For example, they both will have different
::state. Also the ::list pointers in dpage may be stale. We need to
use the current list node (bpage) to do the list manipulation because
the list pointers could have changed between the time that we copied
the contents of bpage to the dpage and the flush list manipulation
below. */
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage)	/*!< in/out: destination block */
{
	buf_page_t*	prev;
	buf_page_t*	prev_b = NULL;
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool_mutex_own(buf_pool));
	/* Must reside in the same buffer pool. */
	ut_ad(buf_pool == buf_pool_from_bpage(dpage));

	ut_ad(mutex_own(buf_page_get_mutex(bpage)));

	buf_flush_list_mutex_enter(buf_pool);

	/* FIXME: At this point we have both buf_pool and flush_list
	mutexes. Theoretically removal of a block from flush list is
	only covered by flush_list mutex but currently we do
	have buf_pool mutex in buf_flush_remove() therefore this block
	is guaranteed to be in the flush list. We need to check if
	this will work without the assumption of block removing code
	having the buf_pool mutex. */
	ut_ad(bpage->in_flush_list);
	ut_ad(dpage->in_flush_list);

	/* If recovery is active we must swap the control blocks in
	the flush_rbt as well. */
	if (buf_pool->flush_rbt != NULL) {
		buf_flush_delete_from_flush_rbt(bpage);
		prev_b = buf_flush_insert_in_flush_rbt(dpage);
	}

	/* Important that we adjust the hazard pointer before removing
	the bpage from the flush list. */
	buf_pool->flush_hp.adjust(bpage);

	/* Must be done after we have removed it from the flush_rbt
	because we assert on in_flush_list in comparison function. */
	ut_d(bpage->in_flush_list = FALSE);

	prev = UT_LIST_GET_PREV(list, bpage);
	UT_LIST_REMOVE(buf_pool->flush_list, bpage);

	if (prev) {
		ut_ad(prev->in_flush_list);
		UT_LIST_INSERT_AFTER( buf_pool->flush_list, prev, dpage);
	} else {
		UT_LIST_ADD_FIRST(buf_pool->flush_list, dpage);
	}

	/* Just an extra check. Previous in flush_list
	should be the same control block as in flush_rbt. */
	ut_a(buf_pool->flush_rbt == NULL || prev_b == prev);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_low(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_flush_list_mutex_exit(buf_pool);
}

/********************************************************************//**
Updates the flush system data structures when a write is completed. */
void
buf_flush_write_complete(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	buf_flush_t	flush_type;
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(bpage);
	buf_flush_remove(bpage);

	flush_type = buf_page_get_flush_type(bpage);
	buf_pool->n_flush[flush_type]--;

	if (buf_pool->n_flush[flush_type] == 0
	    && buf_pool->init_flush[flush_type] == FALSE) {

		/* The running flush batch has ended */

		os_event_set(buf_pool->no_flush[flush_type]);
	}
#if defined(UNIV_PMEMOBJ_BUF)
	//we do not need this anymore 
#else //original
	buf_dblwr_update(bpage, flush_type);
#endif /* UNIV_PMEMOBJ_BUF */

#if defined (UNIV_PMEMOBJ_PART_PL)
	//we only call pm_ppl_flush_page when the flushed page is persist on storage
	pm_ppl_flush_page(
			gb_pmw->pop, gb_pmw, gb_pmw->ppl,
			bpage,
			bpage->id.space(),
			bpage->id.page_no(),
			bpage->id.fold(),
			bpage->newest_modification);

#endif //UNIV_PMEMBOJ_PART_PL
}
#endif /* !UNIV_HOTBACKUP */

/** Calculate the checksum of a page from compressed table and update
the page.
@param[in,out]	page	page to update
@param[in]	size	compressed page size
@param[in]	lsn	LSN to stamp on the page */
void
buf_flush_update_zip_checksum(
	buf_frame_t*	page,
	ulint		size,
	lsn_t		lsn)
{
	ut_a(size > 0);

	const uint32_t	checksum = page_zip_calc_checksum(
		page, size,
		static_cast<srv_checksum_algorithm_t>(srv_checksum_algorithm));

	mach_write_to_8(page + FIL_PAGE_LSN, lsn);
	mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
}

/** Initialize a page for writing to the tablespace.
@param[in]	block		buffer block; NULL if bypassing the buffer pool
@param[in,out]	page		page frame
@param[in,out]	page_zip_	compressed page, or NULL if uncompressed
@param[in]	newest_lsn	newest modification LSN to the page
@param[in]	skip_checksum	whether to disable the page checksum */
void
buf_flush_init_for_writing(
	const buf_block_t*	block,
	byte*			page,
	void*			page_zip_,
	lsn_t			newest_lsn,
	bool			skip_checksum)
{
	ib_uint32_t	checksum = BUF_NO_CHECKSUM_MAGIC;

	ut_ad(block == NULL || block->frame == page);
	ut_ad(block == NULL || page_zip_ == NULL
	      || &block->page.zip == page_zip_);
	ut_ad(page);

	if (page_zip_) {
		page_zip_des_t*	page_zip;
		ulint		size;

		page_zip = static_cast<page_zip_des_t*>(page_zip_);
		size = page_zip_get_size(page_zip);

		ut_ad(size);
		ut_ad(ut_is_2pow(size));
		ut_ad(size <= UNIV_ZIP_SIZE_MAX);

		switch (fil_page_get_type(page)) {
		case FIL_PAGE_TYPE_ALLOCATED:
		case FIL_PAGE_INODE:
		case FIL_PAGE_IBUF_BITMAP:
		case FIL_PAGE_TYPE_FSP_HDR:
		case FIL_PAGE_TYPE_XDES:
			/* These are essentially uncompressed pages. */
			memcpy(page_zip->data, page, size);
			/* fall through */
		case FIL_PAGE_TYPE_ZBLOB:
		case FIL_PAGE_TYPE_ZBLOB2:
		case FIL_PAGE_INDEX:
		case FIL_PAGE_RTREE:

			buf_flush_update_zip_checksum(
				page_zip->data, size, newest_lsn);

			return;
		}

		ib::error() << "The compressed page to be written"
			" seems corrupt:";
		ut_print_buf(stderr, page, size);
		fputs("\nInnoDB: Possibly older version of the page:", stderr);
		ut_print_buf(stderr, page_zip->data, size);
		putc('\n', stderr);
		ut_error;
	}

	/* Write the newest modification lsn to the page header and trailer */
	mach_write_to_8(page + FIL_PAGE_LSN, newest_lsn);

	mach_write_to_8(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
			newest_lsn);

	if (skip_checksum) {
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
	} else {
		if (block != NULL && UNIV_PAGE_SIZE == 16384) {
			/* The page type could be garbage in old files
			created before MySQL 5.5. Such files always
			had a page size of 16 kilobytes. */
			ulint	page_type = fil_page_get_type(page);
			ulint	reset_type = page_type;

			switch (block->page.id.page_no() % 16384) {
			case 0:
				reset_type = block->page.id.page_no() == 0
					? FIL_PAGE_TYPE_FSP_HDR
					: FIL_PAGE_TYPE_XDES;
				break;
			case 1:
				reset_type = FIL_PAGE_IBUF_BITMAP;
				break;
			default:
				switch (page_type) {
				case FIL_PAGE_INDEX:
				case FIL_PAGE_RTREE:
				case FIL_PAGE_UNDO_LOG:
				case FIL_PAGE_INODE:
				case FIL_PAGE_IBUF_FREE_LIST:
				case FIL_PAGE_TYPE_ALLOCATED:
				case FIL_PAGE_TYPE_SYS:
				case FIL_PAGE_TYPE_TRX_SYS:
				case FIL_PAGE_TYPE_BLOB:
				case FIL_PAGE_TYPE_ZBLOB:
				case FIL_PAGE_TYPE_ZBLOB2:
					break;
				case FIL_PAGE_TYPE_FSP_HDR:
				case FIL_PAGE_TYPE_XDES:
				case FIL_PAGE_IBUF_BITMAP:
					/* These pages should have
					predetermined page numbers
					(see above). */
				default:
					reset_type = FIL_PAGE_TYPE_UNKNOWN;
					break;
				}
			}

			if (UNIV_UNLIKELY(page_type != reset_type)) {
				ib::info()
					<< "Resetting invalid page "
					<< block->page.id << " type "
					<< page_type << " to "
					<< reset_type << " when flushing.";
				fil_page_set_type(page, reset_type);
			}
		}

		switch ((srv_checksum_algorithm_t) srv_checksum_algorithm) {
		case SRV_CHECKSUM_ALGORITHM_CRC32:
		case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
			checksum = buf_calc_page_crc32(page);
			mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
					checksum);
			break;
		case SRV_CHECKSUM_ALGORITHM_INNODB:
		case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
			checksum = (ib_uint32_t) buf_calc_page_new_checksum(
				page);
			mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
					checksum);
			checksum = (ib_uint32_t) buf_calc_page_old_checksum(
				page);
			break;
		case SRV_CHECKSUM_ALGORITHM_NONE:
		case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
			mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
					checksum);
			break;
			/* no default so the compiler will emit a warning if
			new enum is added and not handled here */
		}
	}

	/* With the InnoDB checksum, we overwrite the first 4 bytes of
	the end lsn field to store the old formula checksum. Since it
	depends also on the field FIL_PAGE_SPACE_OR_CHKSUM, it has to
	be calculated after storing the new formula checksum.

	In other cases we write the same value to both fields.
	If CRC32 is used then it is faster to use that checksum
	(calculated above) instead of calculating another one.
	We can afford to store something other than
	buf_calc_page_old_checksum() or BUF_NO_CHECKSUM_MAGIC in
	this field because the file will not be readable by old
	versions of MySQL/InnoDB anyway (older than MySQL 5.6.3) */

	mach_write_to_4(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
			checksum);
}

#ifndef UNIV_HOTBACKUP
/********************************************************************//**
Does an asynchronous write of a buffer page. NOTE: in simulated aio and
also when the doublewrite buffer is used, we must call
buf_dblwr_flush_buffered_writes after we have posted a batch of
writes! */
static
void
buf_flush_write_block_low(
/*======================*/
	buf_page_t*	bpage,		/*!< in: buffer block to write */
	buf_flush_t	flush_type,	/*!< in: type of flush */
	bool		sync)		/*!< in: true if sync IO request */
{
	page_t*	frame = NULL;

#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	ut_ad(!buf_pool_mutex_own(buf_pool));
#endif /* UNIV_DEBUG */

	DBUG_PRINT("ib_buf", ("flush %s %u page " UINT32PF ":" UINT32PF,
			      sync ? "sync" : "async", (unsigned) flush_type,
			      bpage->id.space(), bpage->id.page_no()));

	ut_ad(buf_page_in_file(bpage));

	/* We are not holding buf_pool->mutex or block_mutex here.
	Nevertheless, it is safe to access bpage, because it is
	io_fixed and oldest_modification != 0.  Thus, it cannot be
	relocated in the buffer pool or removed from flush_list or
	LRU_list. */
	ut_ad(!buf_pool_mutex_own(buf_pool));
	ut_ad(!buf_flush_list_mutex_own(buf_pool));
	ut_ad(!buf_page_get_mutex(bpage)->is_owned());
	ut_ad(buf_page_get_io_fix(bpage) == BUF_IO_WRITE);
	ut_ad(bpage->oldest_modification != 0);

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(bpage->id) == 0);
#endif /* UNIV_IBUF_COUNT_DEBUG */

	ut_ad(bpage->newest_modification != 0);

	/* Force the log to the disk before writing the modified block */
	if (!srv_read_only_mode) {
#if defined (UNIV_PMEMOBJ_LOG) || defined (UNIV_PMEMOBJ_WAL) || defined (UNIV_PMEMOBJ_PL) || defined (UNIV_SKIPLOG)
		//Since the log records are persist in NVM we don't need to follow WAL rule
		//Skip flush log here
#else //original 
		log_write_up_to(bpage->newest_modification, true);
#endif
	}

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_POOL_WATCH:
	case BUF_BLOCK_ZIP_PAGE: /* The page should be dirty. */
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		break;
	case BUF_BLOCK_ZIP_DIRTY:
		frame = bpage->zip.data;

		mach_write_to_8(frame + FIL_PAGE_LSN,
				bpage->newest_modification);

		ut_a(page_zip_verify_checksum(frame, bpage->size.physical()));
		break;
	case BUF_BLOCK_FILE_PAGE:
		frame = bpage->zip.data;
		if (!frame) {
			frame = ((buf_block_t*) bpage)->frame;
		}

		buf_flush_init_for_writing(
			reinterpret_cast<const buf_block_t*>(bpage),
			reinterpret_cast<const buf_block_t*>(bpage)->frame,
			bpage->zip.data ? &bpage->zip : NULL,
			bpage->newest_modification,
			fsp_is_checksum_disabled(bpage->id.space()));
		break;
	}
#if defined(UNIV_PMEMOBJ_BUF)

#if defined(UNIV_PMEMOBJ_LSB)
	int ret = pm_lsb_write(gb_pmw->pop, gb_pmw->plsb, bpage->id, bpage->size, frame, sync);
#elif defined (UNIV_PMEMOBJ_BUF_FLUSHER)
	//int ret = pm_buf_write_with_flusher(gb_pmw->pop, gb_pmw, bpage->id, bpage->size, frame, sync);
	int ret = pm_buf_write_with_flusher(gb_pmw->pop, gb_pmw, bpage->id, bpage->size, bpage->newest_modification, frame, sync);
#else
	int ret = pm_buf_write(gb_pmw->pop, gb_pmw->pbuf, bpage->id, bpage->size, frame, sync);
#endif // UNIV_PMEMOBJ_LSB
		assert(ret == PMEM_SUCCESS);
		//we remove this page from LRU
		//assert(buf_page_io_complete(bpage, true));
		assert(buf_page_io_complete(bpage,sync));
		goto skip_write_and_fsync;
	//skip_pm_write:
#endif /*UNIV_PMEMOBJ_BUF*/

#if defined (UNIV_PMEMOBJ_PART_PL)
		/*We don't set state anymore*/
		//pm_ppl_set_flush_state(gb_pmw->pop, gb_pmw->ppl, bpage);
#endif /*UNIV_PMEMOBJ_PART_PL*/

	/* Disable use of double-write buffer for temporary tablespace.
	Given the nature and load of temporary tablespace doublewrite buffer
	adds an overhead during flushing. */

	if (!srv_use_doublewrite_buf
	    || buf_dblwr == NULL
	    || srv_read_only_mode
	    || fsp_is_system_temporary(bpage->id.space())) {

		ut_ad(!srv_read_only_mode
		      || fsp_is_system_temporary(bpage->id.space()));

		ulint	type = IORequest::WRITE | IORequest::DO_NOT_WAKE;

		IORequest	request(type);

		fil_io(request,
		       sync, bpage->id, bpage->size, 0, bpage->size.physical(),
		       frame, bpage);

	} else if (flush_type == BUF_FLUSH_SINGLE_PAGE) {
		buf_dblwr_write_single_page(bpage, sync);
	} else {
		ut_ad(!sync);
		buf_dblwr_add_to_batch(bpage);
	}

	/* When doing single page flushing the IO is done synchronously
	and we flush the changes to disk only for the tablespace we
	are working on. */
	if (sync) {
		ut_ad(flush_type == BUF_FLUSH_SINGLE_PAGE);
		fil_flush(bpage->id.space());

		/* true means we want to evict this page from the
		LRU list as well. */
		buf_page_io_complete(bpage, true);
	}

#if defined(UNIV_PMEMOBJ_BUF)
skip_write_and_fsync:
#endif /*UNIV_PMEMOBJ_BUF*/

	/* Increment the counter of I/O operations used
	for selecting LRU policy. */
	buf_LRU_stat_inc_io();
}

/********************************************************************//**
Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: in simulated aio we must call
os_aio_simulated_wake_handler_threads after we have posted a batch of
writes! NOTE: buf_pool->mutex and buf_page_get_mutex(bpage) must be
held upon entering this function, and they will be released by this
function if it returns true.
@return TRUE if the page was flushed */
ibool
buf_flush_page(
/*===========*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_page_t*	bpage,		/*!< in: buffer control block */
	buf_flush_t	flush_type,	/*!< in: type of flush */
	bool		sync)		/*!< in: true if sync IO request */
{
	BPageMutex*	block_mutex;

	ut_ad(flush_type < BUF_FLUSH_N_TYPES);
	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(buf_page_in_file(bpage));
	ut_ad(!sync || flush_type == BUF_FLUSH_SINGLE_PAGE);

	block_mutex = buf_page_get_mutex(bpage);
	ut_ad(mutex_own(block_mutex));

	ut_ad(buf_flush_ready_for_flush(bpage, flush_type));

	bool	is_uncompressed;

#if defined(UNIV_TRACE_FLUSH_TIME)
	ulint start_time;
	ulint end_time;

	start_time = ut_time_ms();
#endif
	is_uncompressed = (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);
	ut_ad(is_uncompressed == (block_mutex != &buf_pool->zip_mutex));

	ibool		flush;
	rw_lock_t*	rw_lock;
	bool		no_fix_count = bpage->buf_fix_count == 0;

	if (!is_uncompressed) {
		flush = TRUE;
		rw_lock = NULL;
	} else if (!(no_fix_count || flush_type == BUF_FLUSH_LIST)
		   || (!no_fix_count
		       && srv_shutdown_state <= SRV_SHUTDOWN_CLEANUP
		       && fsp_is_system_temporary(bpage->id.space()))) {
		/* This is a heuristic, to avoid expensive SX attempts. */
		/* For table residing in temporary tablespace sync is done
		using IO_FIX and so before scheduling for flush ensure that
		page is not fixed. */
		flush = FALSE;
	} else {
		rw_lock = &reinterpret_cast<buf_block_t*>(bpage)->lock;
		if (flush_type != BUF_FLUSH_LIST) {
			flush = rw_lock_sx_lock_nowait(rw_lock, BUF_IO_WRITE);
		} else {
			/* Will SX lock later */
			flush = TRUE;
		}
	}

	if (flush) {

		/* We are committed to flushing by the time we get here */

		buf_page_set_io_fix(bpage, BUF_IO_WRITE);

		buf_page_set_flush_type(bpage, flush_type);

		if (buf_pool->n_flush[flush_type] == 0) {
			os_event_reset(buf_pool->no_flush[flush_type]);
		}

		++buf_pool->n_flush[flush_type];

		mutex_exit(block_mutex);

		buf_pool_mutex_exit(buf_pool);

		if (flush_type == BUF_FLUSH_LIST
		    && is_uncompressed
		    && !rw_lock_sx_lock_nowait(rw_lock, BUF_IO_WRITE)) {

		//tdnguyen test
		//printf("\n [begin handle buf_dblwr ==> ");
			if (!fsp_is_system_temporary(bpage->id.space())) {
				/* avoiding deadlock possibility involves
				doublewrite buffer, should flush it, because
				it might hold the another block->lock. */
				buf_dblwr_flush_buffered_writes();
			} else {
				buf_dblwr_sync_datafiles();
			}

			rw_lock_sx_lock_gen(rw_lock, BUF_IO_WRITE);
		
		//printf("end handle buf_dblwr] ");
		}

		/* If there is an observer that want to know if the asynchronous
		flushing was sent then notify it.
		Note: we set flush observer to a page with x-latch, so we can
		guarantee that notify_flush and notify_remove are called in pair
		with s-latch on a uncompressed page. */
		if (bpage->flush_observer != NULL) {
			buf_pool_mutex_enter(buf_pool);

			bpage->flush_observer->notify_flush(buf_pool, bpage);

			buf_pool_mutex_exit(buf_pool);
		}

		/* Even though bpage is not protected by any mutex at this
		point, it is safe to access bpage, because it is io_fixed and
		oldest_modification != 0.  Thus, it cannot be relocated in the
		buffer pool or removed from flush_list or LRU_list. */
		
		//tdnguyen test
		//printf("\n [begin buf_flush_write_block_low ==> ");
		buf_flush_write_block_low(bpage, flush_type, sync);
		//printf(" END buf_flush_write_block_low ] ");
	}

#if defined(UNIV_TRACE_FLUSH_TIME)
	end_time = ut_time_ms();
    gb_flush_time += (end_time - start_time);
#endif 

	return(flush);
}

# if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/********************************************************************//**
Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: buf_pool->mutex and block->mutex must be held upon entering this
function, and they will be released by this function after flushing.
This is loosely based on buf_flush_batch() and buf_flush_page().
@return TRUE if the page was flushed and the mutexes released */
ibool
buf_flush_page_try(
/*===============*/
	buf_pool_t*	buf_pool,	/*!< in/out: buffer pool instance */
	buf_block_t*	block)		/*!< in/out: buffer control block */
{
	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(buf_page_mutex_own(block));

	if (!buf_flush_ready_for_flush(&block->page, BUF_FLUSH_SINGLE_PAGE)) {
		return(FALSE);
	}

	/* The following call will release the buffer pool and
	block mutex. */
	return(buf_flush_page(
			buf_pool, &block->page,
			BUF_FLUSH_SINGLE_PAGE, true));
}
# endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

/** Check the page is in buffer pool and can be flushed.
@param[in]	page_id		page id
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST
@return true if the page can be flushed. */
static
bool
buf_flush_check_neighbor(
	const page_id_t&	page_id,
	buf_flush_t		flush_type)
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);
	bool		ret;

	ut_ad(flush_type == BUF_FLUSH_LRU
	      || flush_type == BUF_FLUSH_LIST);

	buf_pool_mutex_enter(buf_pool);

	/* We only want to flush pages from this buffer pool. */
	bpage = buf_page_hash_get(buf_pool, page_id);

	if (!bpage) {

		buf_pool_mutex_exit(buf_pool);
		return(false);
	}

	ut_a(buf_page_in_file(bpage));

	/* We avoid flushing 'non-old' blocks in an LRU flush,
	because the flushed blocks are soon freed */

	ret = false;
	if (flush_type != BUF_FLUSH_LRU || buf_page_is_old(bpage)) {
		BPageMutex* block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);
		if (buf_flush_ready_for_flush(bpage, flush_type)) {
			ret = true;
		}
		mutex_exit(block_mutex);
	}
	buf_pool_mutex_exit(buf_pool);

	return(ret);
}

/** Flushes to disk all flushable pages within the flush area.
@param[in]	page_id		page id
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST
@param[in]	n_flushed	number of pages flushed so far in this batch
@param[in]	n_to_flush	maximum number of pages we are allowed to flush
@return number of pages flushed */
static
ulint
buf_flush_try_neighbors(
	const page_id_t&	page_id,
	buf_flush_t		flush_type,
	ulint			n_flushed,
	ulint			n_to_flush)
{
	ulint		i;
	ulint		low;
	ulint		high;
	ulint		count = 0;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);

	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);

	if (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN
	    || srv_flush_neighbors == 0) {
		/* If there is little space or neighbor flushing is
		not enabled then just flush the victim. */
		low = page_id.page_no();
		high = page_id.page_no() + 1;
	} else {
		/* When flushed, dirty blocks are searched in
		neighborhoods of this size, and flushed along with the
		original page. */

		ulint	buf_flush_area;

		buf_flush_area	= ut_min(
			BUF_READ_AHEAD_AREA(buf_pool),
			buf_pool->curr_size / 16);

		low = (page_id.page_no() / buf_flush_area) * buf_flush_area;
		high = (page_id.page_no() / buf_flush_area + 1) * buf_flush_area;

		if (srv_flush_neighbors == 1) {
			/* adjust 'low' and 'high' to limit
			   for contiguous dirty area */
			if (page_id.page_no() > low) {
				for (i = page_id.page_no() - 1; i >= low; i--) {
					if (!buf_flush_check_neighbor(
						page_id_t(page_id.space(), i),
						flush_type)) {

						break;
					}

					if (i == low) {
						/* Avoid overwrap when low == 0
						and calling
						buf_flush_check_neighbor() with
						i == (ulint) -1 */
						i--;
						break;
					}
				}
				low = i + 1;
			}

			for (i = page_id.page_no() + 1;
			     i < high
			     && buf_flush_check_neighbor(
				     page_id_t(page_id.space(), i),
				     flush_type);
			     i++) {
				/* do nothing */
			}
			high = i;
		}
	}

	const ulint	space_size = fil_space_get_size(page_id.space());
	if (high > space_size) {
		high = space_size;
	}

	DBUG_PRINT("ib_buf", ("flush " UINT32PF ":%u..%u",
			      page_id.space(),
			      (unsigned) low, (unsigned) high));

	for (ulint i = low; i < high; i++) {
		buf_page_t*	bpage;

		if ((count + n_flushed) >= n_to_flush) {

			/* We have already flushed enough pages and
			should call it a day. There is, however, one
			exception. If the page whose neighbors we
			are flushing has not been flushed yet then
			we'll try to flush the victim that we
			selected originally. */
			if (i <= page_id.page_no()) {
				i = page_id.page_no();
			} else {
				break;
			}
		}

		const page_id_t	cur_page_id(page_id.space(), i);

		buf_pool = buf_pool_get(cur_page_id);

		buf_pool_mutex_enter(buf_pool);

		/* We only want to flush pages from this buffer pool. */
		bpage = buf_page_hash_get(buf_pool, cur_page_id);

		if (bpage == NULL) {

			buf_pool_mutex_exit(buf_pool);
			continue;
		}

		ut_a(buf_page_in_file(bpage));

		/* We avoid flushing 'non-old' blocks in an LRU flush,
		because the flushed blocks are soon freed */

		if (flush_type != BUF_FLUSH_LRU
		    || i == page_id.page_no()
		    || buf_page_is_old(bpage)) {

			BPageMutex* block_mutex = buf_page_get_mutex(bpage);

			mutex_enter(block_mutex);

			if (buf_flush_ready_for_flush(bpage, flush_type)
			    && (i == page_id.page_no()
				|| bpage->buf_fix_count == 0)) {

				/* We also try to flush those
				neighbors != offset */

				if (buf_flush_page(
					buf_pool, bpage, flush_type, false)) {

					++count;
				} else {
					mutex_exit(block_mutex);
					buf_pool_mutex_exit(buf_pool);
				}

				continue;
			} else {
				mutex_exit(block_mutex);
			}
		}
		buf_pool_mutex_exit(buf_pool);
	}

	if (count > 1) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_FLUSH_NEIGHBOR_TOTAL_PAGE,
			MONITOR_FLUSH_NEIGHBOR_COUNT,
			MONITOR_FLUSH_NEIGHBOR_PAGES,
			(count - 1));
	}

	return(count);
}

/** Check if the block is modified and ready for flushing.
If the the block is ready to flush then flush the page and try o flush
its neighbors.
@param[in]	bpage		buffer control block,
must be buf_page_in_file(bpage)
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST
@param[in]	n_to_flush	number of pages to flush
@param[in,out]	count		number of pages flushed
@return TRUE if buf_pool mutex was released during this function.
This does not guarantee that some pages were written as well.
Number of pages written are incremented to the count. */
static
bool
buf_flush_page_and_try_neighbors(
	buf_page_t*		bpage,
	buf_flush_t		flush_type,
	ulint			n_to_flush,
	ulint*			count)
{
#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool_mutex_own(buf_pool));
#endif /* UNIV_DEBUG */

	bool		flushed;
	BPageMutex*	block_mutex = buf_page_get_mutex(bpage);

	mutex_enter(block_mutex);

	ut_a(buf_page_in_file(bpage));

	if (buf_flush_ready_for_flush(bpage, flush_type)) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_bpage(bpage);

		const page_id_t	page_id = bpage->id;

		mutex_exit(block_mutex);

		buf_pool_mutex_exit(buf_pool);

		/* Try to flush also all the neighbors */
		*count += buf_flush_try_neighbors(
			page_id, flush_type, *count, n_to_flush);

		buf_pool_mutex_enter(buf_pool);
		flushed = TRUE;
	} else {
		mutex_exit(block_mutex);

		flushed = false;
	}

	ut_ad(buf_pool_mutex_own(buf_pool));

	return(flushed);
}

/*******************************************************************//**
This utility moves the uncompressed frames of pages to the free list.
Note that this function does not actually flush any data to disk. It
just detaches the uncompressed frames from the compressed pages at the
tail of the unzip_LRU and puts those freed frames in the free list.
Note that it is a best effort attempt and it is not guaranteed that
after a call to this function there will be 'max' blocks in the free
list.
@return number of blocks moved to the free list. */
static
ulint
buf_free_from_unzip_LRU_list_batch(
/*===============================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		max)		/*!< in: desired number of
					blocks in the free_list */
{
	ulint		scanned = 0;
	ulint		count = 0;
	ulint		free_len = UT_LIST_GET_LEN(buf_pool->free);
	ulint		lru_len = UT_LIST_GET_LEN(buf_pool->unzip_LRU);

	ut_ad(buf_pool_mutex_own(buf_pool));

	buf_block_t*	block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);

	while (block != NULL
	       && count < max
	       && free_len < srv_LRU_scan_depth
	       && lru_len > UT_LIST_GET_LEN(buf_pool->LRU) / 10) {

		++scanned;
		if (buf_LRU_free_page(&block->page, false)) {
			/* Block was freed. buf_pool->mutex potentially
			released and reacquired */
			++count;
			block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);

		} else {

			block = UT_LIST_GET_PREV(unzip_LRU, block);
		}

		free_len = UT_LIST_GET_LEN(buf_pool->free);
		lru_len = UT_LIST_GET_LEN(buf_pool->unzip_LRU);
	}

	ut_ad(buf_pool_mutex_own(buf_pool));

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_SCANNED,
			MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
			MONITOR_LRU_BATCH_SCANNED_PER_CALL,
			scanned);
	}

	return(count);
}

/*******************************************************************//**
This utility flushes dirty blocks from the end of the LRU list.
The calling thread is not allowed to own any latches on pages!
It attempts to make 'max' blocks available in the free list. Note that
it is a best effort attempt and it is not guaranteed that after a call
to this function there will be 'max' blocks in the free list.
@return number of blocks for which the write request was queued. */
static
ulint
buf_flush_LRU_list_batch(
/*=====================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		max)		/*!< in: desired number of
					blocks in the free_list */
{
	buf_page_t*	bpage;
	ulint		scanned = 0;
	ulint		evict_count = 0;
	ulint		count = 0;
	ulint		free_len = UT_LIST_GET_LEN(buf_pool->free);
	ulint		lru_len = UT_LIST_GET_LEN(buf_pool->LRU);
	ulint		withdraw_depth = 0;

	ut_ad(buf_pool_mutex_own(buf_pool));

	if (buf_pool->curr_size < buf_pool->old_size
	    && buf_pool->withdraw_target > 0) {
		withdraw_depth = buf_pool->withdraw_target
				 - UT_LIST_GET_LEN(buf_pool->withdraw);
	}

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU);
	     bpage != NULL && count + evict_count < max
	     && free_len < srv_LRU_scan_depth + withdraw_depth
	     && lru_len > BUF_LRU_MIN_LEN;
	     ++scanned,
	     bpage = buf_pool->lru_hp.get()) {

		buf_page_t* prev = UT_LIST_GET_PREV(LRU, bpage);
		buf_pool->lru_hp.set(prev);

		BPageMutex*	block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);

		if (buf_flush_ready_for_replace(bpage)) {
			/* block is ready for eviction i.e., it is
			clean and is not IO-fixed or buffer fixed. */
			mutex_exit(block_mutex);
			if (buf_LRU_free_page(bpage, true)) {
				++evict_count;
			}
		} else if (buf_flush_ready_for_flush(bpage, BUF_FLUSH_LRU)) {
			/* Block is ready for flush. Dispatch an IO
			request. The IO helper thread will put it on
			free list in IO completion routine. */
			mutex_exit(block_mutex);
			buf_flush_page_and_try_neighbors(
				bpage, BUF_FLUSH_LRU, max, &count);
		} else {
			/* Can't evict or dispatch this block. Go to
			previous. */
			ut_ad(buf_pool->lru_hp.is_hp(prev));
			mutex_exit(block_mutex);
		}

		ut_ad(!mutex_own(block_mutex));
		ut_ad(buf_pool_mutex_own(buf_pool));

		free_len = UT_LIST_GET_LEN(buf_pool->free);
		lru_len = UT_LIST_GET_LEN(buf_pool->LRU);
	}

	buf_pool->lru_hp.set(NULL);

	/* We keep track of all flushes happening as part of LRU
	flush. When estimating the desired rate at which flush_list
	should be flushed, we factor in this value. */
	buf_lru_flush_page_count += count;

	ut_ad(buf_pool_mutex_own(buf_pool));

	if (evict_count) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_EVICT_TOTAL_PAGE,
			MONITOR_LRU_BATCH_EVICT_COUNT,
			MONITOR_LRU_BATCH_EVICT_PAGES,
			evict_count);
	}

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_SCANNED,
			MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
			MONITOR_LRU_BATCH_SCANNED_PER_CALL,
			scanned);
	}

	return(count);
}

/*******************************************************************//**
Flush and move pages from LRU or unzip_LRU list to the free list.
Whether LRU or unzip_LRU is used depends on the state of the system.
@return number of blocks for which either the write request was queued
or in case of unzip_LRU the number of blocks actually moved to the
free list */
static
ulint
buf_do_LRU_batch(
/*=============*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		max)		/*!< in: desired number of
					blocks in the free_list */
{
	ulint	count = 0;

	if (buf_LRU_evict_from_unzip_LRU(buf_pool)) {
		count += buf_free_from_unzip_LRU_list_batch(buf_pool, max);
	}

	if (max > count) {
		count += buf_flush_LRU_list_batch(buf_pool, max - count);
	}

	return(count);
}

/** This utility flushes dirty blocks from the end of the flush_list.
The calling thread is not allowed to own any latches on pages!
@param[in]	buf_pool	buffer pool instance
@param[in]	min_n		wished minimum mumber of blocks flushed (it is
not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	all blocks whose oldest_modification is smaller
than this should be flushed (if their number does not exceed min_n)
@return number of blocks for which the write request was queued;
ULINT_UNDEFINED if there was a flush of the same type already
running */
static
ulint
buf_do_flush_list_batch(
	buf_pool_t*		buf_pool,
	ulint			min_n,
	lsn_t			lsn_limit)
{
	ulint		count = 0;
	ulint		scanned = 0;

	ut_ad(buf_pool_mutex_own(buf_pool));

	/* Start from the end of the list looking for a suitable
	block to be flushed. */
	buf_flush_list_mutex_enter(buf_pool);
	ulint len = UT_LIST_GET_LEN(buf_pool->flush_list);

	/* In order not to degenerate this scan to O(n*n) we attempt
	to preserve pointer of previous block in the flush list. To do
	so we declare it a hazard pointer. Any thread working on the
	flush list must check the hazard pointer and if it is removing
	the same block then it must reset it. */
	for (buf_page_t* bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
	     count < min_n && bpage != NULL && len > 0
	     && bpage->oldest_modification < lsn_limit;
	     bpage = buf_pool->flush_hp.get(),
	     ++scanned) {

		buf_page_t*	prev;

		ut_a(bpage->oldest_modification > 0);
		ut_ad(bpage->in_flush_list);

		prev = UT_LIST_GET_PREV(list, bpage);
		buf_pool->flush_hp.set(prev);
		buf_flush_list_mutex_exit(buf_pool);

#ifdef UNIV_DEBUG
		bool flushed =
#endif /* UNIV_DEBUG */
		buf_flush_page_and_try_neighbors(
			bpage, BUF_FLUSH_LIST, min_n, &count);

		buf_flush_list_mutex_enter(buf_pool);

		ut_ad(flushed || buf_pool->flush_hp.is_hp(prev));

		--len;
	}

	buf_pool->flush_hp.set(NULL);
	buf_flush_list_mutex_exit(buf_pool);

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_FLUSH_BATCH_SCANNED,
			MONITOR_FLUSH_BATCH_SCANNED_NUM_CALL,
			MONITOR_FLUSH_BATCH_SCANNED_PER_CALL,
			scanned);
	}

	if (count) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_FLUSH_BATCH_TOTAL_PAGE,
			MONITOR_FLUSH_BATCH_COUNT,
			MONITOR_FLUSH_BATCH_PAGES,
			count);
	}

	ut_ad(buf_pool_mutex_own(buf_pool));

	return(count);
}

/** This utility flushes dirty blocks from the end of the LRU list or
flush_list.
NOTE 1: in the case of an LRU flush the calling thread may own latches to
pages: to avoid deadlocks, this function must be written so that it cannot
end up waiting for these latches! NOTE 2: in the case of a flush list flush,
the calling thread is not allowed to own any latches on pages!
@param[in]	buf_pool	buffer pool instance
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST; if
BUF_FLUSH_LIST, then the caller must not own any latches on pages
@param[in]	min_n		wished minimum mumber of blocks flushed (it is
not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	in the case of BUF_FLUSH_LIST all blocks whose
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored
@return number of blocks for which the write request was queued */
static
ulint
buf_flush_batch(
	buf_pool_t*		buf_pool,
	buf_flush_t		flush_type,
	ulint			min_n,
	lsn_t			lsn_limit)
{
	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);

#ifdef UNIV_DEBUG
	{
		dict_sync_check	check(true);

		ut_ad(flush_type != BUF_FLUSH_LIST
		      || !sync_check_iterate(check));
	}
#endif /* UNIV_DEBUG */

	buf_pool_mutex_enter(buf_pool);

	ulint	count = 0;

	/* Note: The buffer pool mutex is released and reacquired within
	the flush functions. */
	switch (flush_type) {
	case BUF_FLUSH_LRU:
		count = buf_do_LRU_batch(buf_pool, min_n);
		break;
	case BUF_FLUSH_LIST:
		count = buf_do_flush_list_batch(buf_pool, min_n, lsn_limit);
		break;
	default:
		ut_error;
	}

	buf_pool_mutex_exit(buf_pool);

	DBUG_PRINT("ib_buf", ("flush %u completed, %u pages",
			      unsigned(flush_type), unsigned(count)));

	return(count);
}

/******************************************************************//**
Gather the aggregated stats for both flush list and LRU list flushing.
@param page_count_flush	number of pages flushed from the end of the flush_list
@param page_count_LRU	number of pages flushed from the end of the LRU list
*/
static
void
buf_flush_stats(
/*============*/
	ulint		page_count_flush,
	ulint		page_count_LRU)
{
	DBUG_PRINT("ib_buf", ("flush completed, from flush_list %u pages, "
			      "from LRU_list %u pages",
			      unsigned(page_count_flush),
			      unsigned(page_count_LRU)));

	srv_stats.buf_pool_flushed.add(page_count_flush + page_count_LRU);
}

/******************************************************************//**
Start a buffer flush batch for LRU or flush list */
static
ibool
buf_flush_start(
/*============*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_flush_t	flush_type)	/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
{
	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);

	buf_pool_mutex_enter(buf_pool);

	if (buf_pool->n_flush[flush_type] > 0
	   || buf_pool->init_flush[flush_type] == TRUE) {

		/* There is already a flush batch of the same type running */

		buf_pool_mutex_exit(buf_pool);

		return(FALSE);
	}

	buf_pool->init_flush[flush_type] = TRUE;

	os_event_reset(buf_pool->no_flush[flush_type]);

	buf_pool_mutex_exit(buf_pool);

	return(TRUE);
}

/******************************************************************//**
End a buffer flush batch for LRU or flush list */
static
void
buf_flush_end(
/*==========*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_flush_t	flush_type)	/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
{
	buf_pool_mutex_enter(buf_pool);

	buf_pool->init_flush[flush_type] = FALSE;

	buf_pool->try_LRU_scan = TRUE;

	if (buf_pool->n_flush[flush_type] == 0) {

		/* The running flush batch has ended */

		os_event_set(buf_pool->no_flush[flush_type]);
	}

	buf_pool_mutex_exit(buf_pool);

	if (!srv_read_only_mode) {
		buf_dblwr_flush_buffered_writes();
	} else {
		os_aio_simulated_wake_handler_threads();
	}
}

/******************************************************************//**
Waits until a flush batch of the given type ends */
void
buf_flush_wait_batch_end(
/*=====================*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_flush_t	type)		/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
{
	ut_ad(type == BUF_FLUSH_LRU || type == BUF_FLUSH_LIST);

	if (buf_pool == NULL) {
		ulint	i;

		for (i = 0; i < srv_buf_pool_instances; ++i) {
			buf_pool_t*	buf_pool;

			buf_pool = buf_pool_from_array(i);

			thd_wait_begin(NULL, THD_WAIT_DISKIO);
			os_event_wait(buf_pool->no_flush[type]);
			thd_wait_end(NULL);
		}
	} else {
		thd_wait_begin(NULL, THD_WAIT_DISKIO);
		os_event_wait(buf_pool->no_flush[type]);
		thd_wait_end(NULL);
	}
}

/** Do flushing batch of a given type.
NOTE: The calling thread is not allowed to own any latches on pages!
@param[in,out]	buf_pool	buffer pool instance
@param[in]	type		flush type
@param[in]	min_n		wished minimum mumber of blocks flushed
(it is not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	in the case BUF_FLUSH_LIST all blocks whose
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored
@param[out]	n_processed	the number of pages which were processed is
passed back to caller. Ignored if NULL
@retval true	if a batch was queued successfully.
@retval false	if another batch of same type was already running. */
bool
buf_flush_do_batch(
	buf_pool_t*		buf_pool,
	buf_flush_t		type,
	ulint			min_n,
	lsn_t			lsn_limit,
	ulint*			n_processed)
{
	ut_ad(type == BUF_FLUSH_LRU || type == BUF_FLUSH_LIST);

	if (n_processed != NULL) {
		*n_processed = 0;
	}

	if (!buf_flush_start(buf_pool, type)) {
		return(false);
	}

	ulint	page_count = buf_flush_batch(buf_pool, type, min_n, lsn_limit);

	buf_flush_end(buf_pool, type);

	if (n_processed != NULL) {
		*n_processed = page_count;
	}

	return(true);
}

/**
Waits until a flush batch of the given lsn ends
@param[in]	new_oldest	target oldest_modified_lsn to wait for */

void
buf_flush_wait_flushed(
	lsn_t		new_oldest)
{
	for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
		buf_pool_t*	buf_pool;
		lsn_t		oldest;

		buf_pool = buf_pool_from_array(i);

		for (;;) {
			/* We don't need to wait for fsync of the flushed
			blocks, because anyway we need fsync to make chekpoint.
			So, we don't need to wait for the batch end here. */

			buf_flush_list_mutex_enter(buf_pool);

			buf_page_t*	bpage;

			/* We don't need to wait for system temporary pages */
			for (bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
			     bpage != NULL
				&& fsp_is_system_temporary(bpage->id.space());
			     bpage = UT_LIST_GET_PREV(list, bpage)) {
				/* Do nothing. */
			}

			if (bpage != NULL) {
				ut_ad(bpage->in_flush_list);
				oldest = bpage->oldest_modification;
			} else {
				oldest = 0;
			}

			buf_flush_list_mutex_exit(buf_pool);

			if (oldest == 0 || oldest >= new_oldest) {
				break;
			}

			/* sleep and retry */
			os_thread_sleep(buf_flush_wait_flushed_sleep_time);

			MONITOR_INC(MONITOR_FLUSH_SYNC_WAITS);
		}
	}
}

/** This utility flushes dirty blocks from the end of the flush list of all
buffer pool instances.
NOTE: The calling thread is not allowed to own any latches on pages!
@param[in]	min_n		wished minimum mumber of blocks flushed (it is
not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	in the case BUF_FLUSH_LIST all blocks whose
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored
@param[out]	n_processed	the number of pages which were processed is
passed back to caller. Ignored if NULL.
@return true if a batch was queued successfully for each buffer pool
instance. false if another batch of same type was already running in
at least one of the buffer pool instance */
bool
buf_flush_lists(
	ulint			min_n,
	lsn_t			lsn_limit,
	ulint*			n_processed)
{
	ulint		i;
	ulint		n_flushed = 0;
	bool		success = true;

	if (n_processed) {
		*n_processed = 0;
	}

	if (min_n != ULINT_MAX) {
		/* Ensure that flushing is spread evenly amongst the
		buffer pool instances. When min_n is ULINT_MAX
		we need to flush everything up to the lsn limit
		so no limit here. */
		min_n = (min_n + srv_buf_pool_instances - 1)
			 / srv_buf_pool_instances;
	}

	/* Flush to lsn_limit in all buffer pool instances */
	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;
		ulint		page_count = 0;

		buf_pool = buf_pool_from_array(i);

		if (!buf_flush_do_batch(buf_pool,
					BUF_FLUSH_LIST,
					min_n,
					lsn_limit,
					&page_count)) {
			/* We have two choices here. If lsn_limit was
			specified then skipping an instance of buffer
			pool means we cannot guarantee that all pages
			up to lsn_limit has been flushed. We can
			return right now with failure or we can try
			to flush remaining buffer pools up to the
			lsn_limit. We attempt to flush other buffer
			pools based on the assumption that it will
			help in the retry which will follow the
			failure. */
			success = false;

			continue;
		}

		n_flushed += page_count;
	}

	if (n_flushed) {
		buf_flush_stats(n_flushed, 0);
	}

	if (n_processed) {
		*n_processed = n_flushed;
	}

	return(success);
}

/******************************************************************//**
This function picks up a single page from the tail of the LRU
list, flushes it (if it is dirty), removes it from page_hash and LRU
list and puts it on the free list. It is called from user threads when
they are unable to find a replaceable page at the tail of the LRU
list i.e.: when the background LRU flushing in the page_cleaner thread
is not fast enough to keep pace with the workload.
@return true if success. */
bool
buf_flush_single_page_from_LRU(
/*===========================*/
	buf_pool_t*	buf_pool)	/*!< in/out: buffer pool instance */
{
	ulint		scanned;
	buf_page_t*	bpage;
	ibool		freed;

	buf_pool_mutex_enter(buf_pool);

	for (bpage = buf_pool->single_scan_itr.start(), scanned = 0,
	     freed = false;
	     bpage != NULL;
	     ++scanned, bpage = buf_pool->single_scan_itr.get()) {

		ut_ad(buf_pool_mutex_own(buf_pool));

		buf_page_t*	prev = UT_LIST_GET_PREV(LRU, bpage);

		buf_pool->single_scan_itr.set(prev);

		BPageMutex*	block_mutex;

		block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);

		if (buf_flush_ready_for_replace(bpage)) {
			/* block is ready for eviction i.e., it is
			clean and is not IO-fixed or buffer fixed. */
			mutex_exit(block_mutex);

			if (buf_LRU_free_page(bpage, true)) {
				buf_pool_mutex_exit(buf_pool);
				freed = true;
				break;
			}

		} else if (buf_flush_ready_for_flush(
				   bpage, BUF_FLUSH_SINGLE_PAGE)) {

			/* Block is ready for flush. Try and dispatch an IO
			request. We'll put it on free list in IO completion
			routine if it is not buffer fixed. The following call
			will release the buffer pool and block mutex.

			Note: There is no guarantee that this page has actually
			been freed, only that it has been flushed to disk */
			
			//tdnguyen test
			//printf("\n [begin buf_flush_page ==>");
			freed = buf_flush_page(
				buf_pool, bpage, BUF_FLUSH_SINGLE_PAGE, true);

			//printf("END buf_flush_page ");
			if (freed) {
				break;
			}

			mutex_exit(block_mutex);
		} else {
			mutex_exit(block_mutex);
		}

		ut_ad(!mutex_own(block_mutex));
	}

	if (!freed) {
		/* Can't find a single flushable page. */
		ut_ad(!bpage);
		buf_pool_mutex_exit(buf_pool);
	}

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_SINGLE_FLUSH_SCANNED,
			MONITOR_LRU_SINGLE_FLUSH_SCANNED_NUM_CALL,
			MONITOR_LRU_SINGLE_FLUSH_SCANNED_PER_CALL,
			scanned);
	}

	ut_ad(!buf_pool_mutex_own(buf_pool));

	return(freed);
}

/**
Clears up tail of the LRU list of a given buffer pool instance:
* Put replaceable pages at the tail of LRU to the free list
* Flush dirty pages at the tail of LRU to the disk
The depth to which we scan each buffer pool is controlled by dynamic
config parameter innodb_LRU_scan_depth.
@param buf_pool buffer pool instance
@return total pages flushed */
static
ulint
buf_flush_LRU_list(
	buf_pool_t*	buf_pool)
{
	ulint	scan_depth, withdraw_depth;
	ulint	n_flushed = 0;

	ut_ad(buf_pool);

	/* srv_LRU_scan_depth can be arbitrarily large value.
	We cap it with current LRU size. */
	buf_pool_mutex_enter(buf_pool);
	scan_depth = UT_LIST_GET_LEN(buf_pool->LRU);
	if (buf_pool->curr_size < buf_pool->old_size
	    && buf_pool->withdraw_target > 0) {
		withdraw_depth = buf_pool->withdraw_target
				 - UT_LIST_GET_LEN(buf_pool->withdraw);
	} else {
		withdraw_depth = 0;
	}
	buf_pool_mutex_exit(buf_pool);

	if (withdraw_depth > srv_LRU_scan_depth) {
		scan_depth = ut_min(withdraw_depth, scan_depth);
	} else {
		scan_depth = ut_min(static_cast<ulint>(srv_LRU_scan_depth),
				    scan_depth);
	}

	/* Currently one of page_cleaners is the only thread
	that can trigger an LRU flush at the same time.
	So, it is not possible that a batch triggered during
	last iteration is still running, */
	buf_flush_do_batch(buf_pool, BUF_FLUSH_LRU, scan_depth,
			   0, &n_flushed);

	return(n_flushed);
}

/*********************************************************************//**
Clears up tail of the LRU lists:
* Put replaceable pages at the tail of LRU to the free list
* Flush dirty pages at the tail of LRU to the disk
The depth to which we scan each buffer pool is controlled by dynamic
config parameter innodb_LRU_scan_depth.
@return total pages flushed */
ulint
buf_flush_LRU_lists(void)
/*=====================*/
{
	ulint	n_flushed = 0;

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {

		n_flushed += buf_flush_LRU_list(buf_pool_from_array(i));
	}

	if (n_flushed) {
		buf_flush_stats(0, n_flushed);
	}

	return(n_flushed);
}

/*********************************************************************//**
Wait for any possible LRU flushes that are in progress to end. */
void
buf_flush_wait_LRU_batch_end(void)
/*==============================*/
{
	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_pool_mutex_enter(buf_pool);

		if (buf_pool->n_flush[BUF_FLUSH_LRU] > 0
		   || buf_pool->init_flush[BUF_FLUSH_LRU]) {

			buf_pool_mutex_exit(buf_pool);
			buf_flush_wait_batch_end(buf_pool, BUF_FLUSH_LRU);
		} else {
			buf_pool_mutex_exit(buf_pool);
		}
	}
}

/*********************************************************************//**
Calculates if flushing is required based on number of dirty pages in
the buffer pool.
@return percent of io_capacity to flush to manage dirty page ratio */
static
ulint
af_get_pct_for_dirty()
/*==================*/
{
	double	dirty_pct = buf_get_modified_ratio_pct();

	if (dirty_pct == 0.0) {
		/* No pages modified */
		return(0);
	}

	ut_a(srv_max_dirty_pages_pct_lwm
	     <= srv_max_buf_pool_modified_pct);

	if (srv_max_dirty_pages_pct_lwm == 0) {
		/* The user has not set the option to preflush dirty
		pages as we approach the high water mark. */
		if (dirty_pct >= srv_max_buf_pool_modified_pct) {
			/* We have crossed the high water mark of dirty
			pages In this case we start flushing at 100% of
			innodb_io_capacity. */
			return(100);
		}
	} else if (dirty_pct >= srv_max_dirty_pages_pct_lwm) {
		/* We should start flushing pages gradually. */
		return(static_cast<ulint>((dirty_pct * 100)
		       / (srv_max_buf_pool_modified_pct + 1)));
	}

	return(0);
}

/*********************************************************************//**
Calculates if flushing is required based on redo generation rate.
@return percent of io_capacity to flush to manage redo space */
static
ulint
af_get_pct_for_lsn(
/*===============*/
	lsn_t	age)	/*!< in: current age of LSN. */
{
	lsn_t	max_async_age;
	lsn_t	lsn_age_factor;
	lsn_t	af_lwm = (srv_adaptive_flushing_lwm
			  * log_get_capacity()) / 100;

	if (age < af_lwm) {
		/* No adaptive flushing. */
		return(0);
	}

	max_async_age = log_get_max_modified_age_async();

	if (age < max_async_age && !srv_adaptive_flushing) {
		/* We have still not reached the max_async point and
		the user has disabled adaptive flushing. */
		return(0);
	}

	/* If we are here then we know that either:
	1) User has enabled adaptive flushing
	2) User may have disabled adaptive flushing but we have reached
	max_async_age. */
	lsn_age_factor = (age * 100) / max_async_age;

	ut_ad(srv_max_io_capacity >= srv_io_capacity);
	return(static_cast<ulint>(
		((srv_max_io_capacity / srv_io_capacity)
		* (lsn_age_factor * sqrt((double)lsn_age_factor)))
		/ 7.5));
}

/*********************************************************************//**
This function is called approximately once every second by the
page_cleaner thread. Based on various factors it decides if there is a
need to do flushing.
@return number of pages recommended to be flushed
@param lsn_limit	pointer to return LSN up to which flushing must happen
@param last_pages_in	the number of pages flushed by the last flush_list
			flushing. */
static
ulint
page_cleaner_flush_pages_recommendation(
/*====================================*/
	lsn_t*	lsn_limit,
	ulint	last_pages_in)
{
	static	lsn_t		prev_lsn = 0;
	static	ulint		sum_pages = 0;
	static	ulint		avg_page_rate = 0;
	static	ulint		n_iterations = 0;
	static	time_t		prev_time;
	lsn_t			oldest_lsn;
	lsn_t			cur_lsn;
	lsn_t			age;
	lsn_t			lsn_rate;
	ulint			n_pages = 0;
	ulint			pct_for_dirty = 0;
	ulint			pct_for_lsn = 0;
	ulint			pct_total = 0;

	cur_lsn = log_get_lsn();

	if (prev_lsn == 0) {
		/* First time around. */
		prev_lsn = cur_lsn;
		prev_time = ut_time();
		return(0);
	}

	if (prev_lsn == cur_lsn) {
		return(0);
	}

	sum_pages += last_pages_in;

	time_t	curr_time = ut_time();
	double	time_elapsed = difftime(curr_time, prev_time);

	/* We update our variables every srv_flushing_avg_loops
	iterations to smooth out transition in workload. */
	if (++n_iterations >= srv_flushing_avg_loops
	    || time_elapsed >= srv_flushing_avg_loops) {

		if (time_elapsed < 1) {
			time_elapsed = 1;
		}

		avg_page_rate = static_cast<ulint>(
			((static_cast<double>(sum_pages)
			  / time_elapsed)
			 + avg_page_rate) / 2);

		/* How much LSN we have generated since last call. */
		lsn_rate = static_cast<lsn_t>(
			static_cast<double>(cur_lsn - prev_lsn)
			/ time_elapsed);

		lsn_avg_rate = (lsn_avg_rate + lsn_rate) / 2;


		/* aggregate stats of all slots */
		mutex_enter(&page_cleaner->mutex);

		ulint	flush_tm = page_cleaner->flush_time;
		ulint	flush_pass = page_cleaner->flush_pass;

		page_cleaner->flush_time = 0;
		page_cleaner->flush_pass = 0;

		ulint	lru_tm = 0;
		ulint	list_tm = 0;
		ulint	lru_pass = 0;
		ulint	list_pass = 0;

		for (ulint i = 0; i < page_cleaner->n_slots; i++) {
			page_cleaner_slot_t*	slot;

			slot = &page_cleaner->slots[i];

			lru_tm    += slot->flush_lru_time;
			lru_pass  += slot->flush_lru_pass;
			list_tm   += slot->flush_list_time;
			list_pass += slot->flush_list_pass;

			slot->flush_lru_time  = 0;
			slot->flush_lru_pass  = 0;
			slot->flush_list_time = 0;
			slot->flush_list_pass = 0;
		}

		mutex_exit(&page_cleaner->mutex);

		/* minimum values are 1, to avoid dividing by zero. */
		if (lru_tm < 1) {
			lru_tm = 1;
		}
		if (list_tm < 1) {
			list_tm = 1;
		}
		if (flush_tm < 1) {
			flush_tm = 1;
		}

		if (lru_pass < 1) {
			lru_pass = 1;
		}
		if (list_pass < 1) {
			list_pass = 1;
		}
		if (flush_pass < 1) {
			flush_pass = 1;
		}

		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_TIME_SLOT,
			    list_tm / list_pass);
		MONITOR_SET(MONITOR_LRU_BATCH_FLUSH_AVG_TIME_SLOT,
			    lru_tm  / lru_pass);

		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_TIME_THREAD,
			    list_tm / (srv_n_page_cleaners * flush_pass));
		MONITOR_SET(MONITOR_LRU_BATCH_FLUSH_AVG_TIME_THREAD,
			    lru_tm / (srv_n_page_cleaners * flush_pass));
		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_TIME_EST,
			    flush_tm * list_tm / flush_pass
			    / (list_tm + lru_tm));
		MONITOR_SET(MONITOR_LRU_BATCH_FLUSH_AVG_TIME_EST,
			    flush_tm * lru_tm / flush_pass
			    / (list_tm + lru_tm));
		MONITOR_SET(MONITOR_FLUSH_AVG_TIME, flush_tm / flush_pass);

		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_PASS,
			    list_pass / page_cleaner->n_slots);
		MONITOR_SET(MONITOR_LRU_BATCH_FLUSH_AVG_PASS,
			    lru_pass / page_cleaner->n_slots);
		MONITOR_SET(MONITOR_FLUSH_AVG_PASS, flush_pass);

		prev_lsn = cur_lsn;
		prev_time = curr_time;

		n_iterations = 0;

		sum_pages = 0;
	}

	oldest_lsn = buf_pool_get_oldest_modification();

	ut_ad(oldest_lsn <= log_get_lsn());

	age = cur_lsn > oldest_lsn ? cur_lsn - oldest_lsn : 0;

	pct_for_dirty = af_get_pct_for_dirty();
	pct_for_lsn = af_get_pct_for_lsn(age);

	pct_total = ut_max(pct_for_dirty, pct_for_lsn);

	/* Estimate pages to be flushed for the lsn progress */
	ulint	sum_pages_for_lsn = 0;
	lsn_t	target_lsn = oldest_lsn
			     + lsn_avg_rate * buf_flush_lsn_scan_factor;

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool = buf_pool_from_array(i);
		ulint		pages_for_lsn = 0;

		buf_flush_list_mutex_enter(buf_pool);
		for (buf_page_t* b = UT_LIST_GET_LAST(buf_pool->flush_list);
		     b != NULL;
		     b = UT_LIST_GET_PREV(list, b)) {
			if (b->oldest_modification > target_lsn) {
				break;
			}
			++pages_for_lsn;
		}
		buf_flush_list_mutex_exit(buf_pool);

		sum_pages_for_lsn += pages_for_lsn;

		mutex_enter(&page_cleaner->mutex);
		ut_ad(page_cleaner->slots[i].state
		      == PAGE_CLEANER_STATE_NONE);
		page_cleaner->slots[i].n_pages_requested
			= pages_for_lsn / buf_flush_lsn_scan_factor + 1;
		mutex_exit(&page_cleaner->mutex);
	}

	sum_pages_for_lsn /= buf_flush_lsn_scan_factor;
	if(sum_pages_for_lsn < 1) {
		sum_pages_for_lsn = 1;
	}

	/* Cap the maximum IO capacity that we are going to use by
	max_io_capacity. Limit the value to avoid too quick increase */
	ulint	pages_for_lsn =
		std::min<ulint>(sum_pages_for_lsn, srv_max_io_capacity * 2);

	n_pages = (PCT_IO(pct_total) + avg_page_rate + pages_for_lsn) / 3;

	if (n_pages > srv_max_io_capacity) {
		n_pages = srv_max_io_capacity;
	}

	/* Normalize request for each instance */
	mutex_enter(&page_cleaner->mutex);
	ut_ad(page_cleaner->n_slots_requested == 0);
	ut_ad(page_cleaner->n_slots_flushing == 0);
	ut_ad(page_cleaner->n_slots_finished == 0);

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		/* if REDO has enough of free space,
		don't care about age distribution of pages */
		page_cleaner->slots[i].n_pages_requested = pct_for_lsn > 30 ?
			page_cleaner->slots[i].n_pages_requested
			* n_pages / sum_pages_for_lsn + 1
			: n_pages / srv_buf_pool_instances;
	}
	mutex_exit(&page_cleaner->mutex);

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_REQUESTED, n_pages);

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_BY_AGE, sum_pages_for_lsn);

	MONITOR_SET(MONITOR_FLUSH_AVG_PAGE_RATE, avg_page_rate);
	MONITOR_SET(MONITOR_FLUSH_LSN_AVG_RATE, lsn_avg_rate);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_DIRTY, pct_for_dirty);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_LSN, pct_for_lsn);

	*lsn_limit = LSN_MAX;

	return(n_pages);
}

/*********************************************************************//**
Puts the page_cleaner thread to sleep if it has finished work in less
than a second
@retval 0 wake up by event set,
@retval OS_SYNC_TIME_EXCEEDED if timeout was exceeded
@param next_loop_time	time when next loop iteration should start
@param sig_count	zero or the value returned by previous call of
			os_event_reset() */
static
ulint
pc_sleep_if_needed(
/*===============*/
	ulint		next_loop_time,
	int64_t		sig_count)
{
	ulint	cur_time = ut_time_ms();

	if (next_loop_time > cur_time) {
		/* Get sleep interval in micro seconds. We use
		ut_min() to avoid long sleep in case of wrap around. */
		ulint	sleep_us;

		sleep_us = ut_min(static_cast<ulint>(1000000),
				  (next_loop_time - cur_time) * 1000);

		return(os_event_wait_time_low(buf_flush_event,
					      sleep_us, sig_count));
	}

	return(OS_SYNC_TIME_EXCEEDED);
}

/******************************************************************//**
Initialize page_cleaner. */
void
buf_flush_page_cleaner_init(void)
/*=============================*/
{
	ut_ad(page_cleaner == NULL);

	page_cleaner = static_cast<page_cleaner_t*>(
		ut_zalloc_nokey(sizeof(*page_cleaner)));

	mutex_create(LATCH_ID_PAGE_CLEANER, &page_cleaner->mutex);

	page_cleaner->is_requested = os_event_create("pc_is_requested");
	page_cleaner->is_finished = os_event_create("pc_is_finished");

	page_cleaner->n_slots = static_cast<ulint>(srv_buf_pool_instances);

	page_cleaner->slots = static_cast<page_cleaner_slot_t*>(
		ut_zalloc_nokey(page_cleaner->n_slots
				* sizeof(*page_cleaner->slots)));

	ut_d(page_cleaner->n_disabled_debug = 0);

	page_cleaner->is_running = true;
}

/**
Close page_cleaner. */
static
void
buf_flush_page_cleaner_close(void)
{
	/* waiting for all worker threads exit */
	while (page_cleaner->n_workers > 0) {
		os_thread_sleep(10000);
	}

	mutex_destroy(&page_cleaner->mutex);

	ut_free(page_cleaner->slots);

	os_event_destroy(page_cleaner->is_finished);
	os_event_destroy(page_cleaner->is_requested);

	ut_free(page_cleaner);

	page_cleaner = NULL;
}

/**
Requests for all slots to flush all buffer pool instances.
@param min_n	wished minimum mumber of blocks flushed
		(it is not guaranteed that the actual number is that big)
@param lsn_limit in the case BUF_FLUSH_LIST all blocks whose
		oldest_modification is smaller than this should be flushed
		(if their number does not exceed min_n), otherwise ignored
*/
static
void
pc_request(
	ulint		min_n,
	lsn_t		lsn_limit)
{
	if (min_n != ULINT_MAX) {
		/* Ensure that flushing is spread evenly amongst the
		buffer pool instances. When min_n is ULINT_MAX
		we need to flush everything up to the lsn limit
		so no limit here. */
		min_n = (min_n + srv_buf_pool_instances - 1)
			/ srv_buf_pool_instances;
	}

	mutex_enter(&page_cleaner->mutex);

	ut_ad(page_cleaner->n_slots_requested == 0);
	ut_ad(page_cleaner->n_slots_flushing == 0);
	ut_ad(page_cleaner->n_slots_finished == 0);

	page_cleaner->requested = (min_n > 0);
	page_cleaner->lsn_limit = lsn_limit;

	for (ulint i = 0; i < page_cleaner->n_slots; i++) {
		page_cleaner_slot_t* slot = &page_cleaner->slots[i];

		ut_ad(slot->state == PAGE_CLEANER_STATE_NONE);

		if (min_n == ULINT_MAX) {
			slot->n_pages_requested = ULINT_MAX;
		} else if (min_n == 0) {
			slot->n_pages_requested = 0;
		}

		/* slot->n_pages_requested was already set by
		page_cleaner_flush_pages_recommendation() */

		slot->state = PAGE_CLEANER_STATE_REQUESTED;
	}

	page_cleaner->n_slots_requested = page_cleaner->n_slots;
	page_cleaner->n_slots_flushing = 0;
	page_cleaner->n_slots_finished = 0;

	os_event_set(page_cleaner->is_requested);

	mutex_exit(&page_cleaner->mutex);
}

/**
Do flush for one slot.
@return	the number of the slots which has not been treated yet. */
static
ulint
pc_flush_slot(void)
{
	ulint	lru_tm = 0;
	ulint	list_tm = 0;
	int	lru_pass = 0;
	int	list_pass = 0;

	mutex_enter(&page_cleaner->mutex);

	if (page_cleaner->n_slots_requested > 0) {
		page_cleaner_slot_t*	slot = NULL;
		ulint			i;

		for (i = 0; i < page_cleaner->n_slots; i++) {
			slot = &page_cleaner->slots[i];

			if (slot->state == PAGE_CLEANER_STATE_REQUESTED) {
				break;
			}
		}

		/* slot should be found because
		page_cleaner->n_slots_requested > 0 */
		ut_a(i < page_cleaner->n_slots);

		buf_pool_t* buf_pool = buf_pool_from_array(i);

		page_cleaner->n_slots_requested--;
		page_cleaner->n_slots_flushing++;
		slot->state = PAGE_CLEANER_STATE_FLUSHING;

		if (page_cleaner->n_slots_requested == 0) {
			os_event_reset(page_cleaner->is_requested);
		}

		if (!page_cleaner->is_running) {
			slot->n_flushed_lru = 0;
			slot->n_flushed_list = 0;
			goto finish_mutex;
		}

		mutex_exit(&page_cleaner->mutex);

		lru_tm = ut_time_ms();

		/* Flush pages from end of LRU if required */
		slot->n_flushed_lru = buf_flush_LRU_list(buf_pool);

		lru_tm = ut_time_ms() - lru_tm;
		lru_pass++;

		if (!page_cleaner->is_running) {
			slot->n_flushed_list = 0;
			goto finish;
		}

		/* Flush pages from flush_list if required */
		if (page_cleaner->requested) {

			list_tm = ut_time_ms();

			slot->succeeded_list = buf_flush_do_batch(
				buf_pool, BUF_FLUSH_LIST,
				slot->n_pages_requested,
				page_cleaner->lsn_limit,
				&slot->n_flushed_list);

			list_tm = ut_time_ms() - list_tm;
			list_pass++;
		} else {
			slot->n_flushed_list = 0;
			slot->succeeded_list = true;
		}
finish:
		mutex_enter(&page_cleaner->mutex);
finish_mutex:
		page_cleaner->n_slots_flushing--;
		page_cleaner->n_slots_finished++;
		slot->state = PAGE_CLEANER_STATE_FINISHED;

		slot->flush_lru_time += lru_tm;
		slot->flush_list_time += list_tm;
		slot->flush_lru_pass += lru_pass;
		slot->flush_list_pass += list_pass;

		if (page_cleaner->n_slots_requested == 0
		    && page_cleaner->n_slots_flushing == 0) {
			os_event_set(page_cleaner->is_finished);
		}
	}

	ulint	ret = page_cleaner->n_slots_requested;

	mutex_exit(&page_cleaner->mutex);

	return(ret);
}

/**
Wait until all flush requests are finished.
@param n_flushed_lru	number of pages flushed from the end of the LRU list.
@param n_flushed_list	number of pages flushed from the end of the
			flush_list.
@return			true if all flush_list flushing batch were success. */
static
bool
pc_wait_finished(
	ulint*	n_flushed_lru,
	ulint*	n_flushed_list)
{
	bool	all_succeeded = true;

	*n_flushed_lru = 0;
	*n_flushed_list = 0;

	os_event_wait(page_cleaner->is_finished);

	mutex_enter(&page_cleaner->mutex);

	ut_ad(page_cleaner->n_slots_requested == 0);
	ut_ad(page_cleaner->n_slots_flushing == 0);
	ut_ad(page_cleaner->n_slots_finished == page_cleaner->n_slots);

	for (ulint i = 0; i < page_cleaner->n_slots; i++) {
		page_cleaner_slot_t* slot = &page_cleaner->slots[i];

		ut_ad(slot->state == PAGE_CLEANER_STATE_FINISHED);

		*n_flushed_lru += slot->n_flushed_lru;
		*n_flushed_list += slot->n_flushed_list;
		all_succeeded &= slot->succeeded_list;

		slot->state = PAGE_CLEANER_STATE_NONE;

		slot->n_pages_requested = 0;
	}

	page_cleaner->n_slots_finished = 0;

	os_event_reset(page_cleaner->is_finished);

	mutex_exit(&page_cleaner->mutex);

	return(all_succeeded);
}

#ifdef UNIV_LINUX
/**
Set priority for page_cleaner threads.
@param[in]	priority	priority intended to set
@return	true if set as intended */
static
bool
buf_flush_page_cleaner_set_priority(
	int	priority)
{
	setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid),
		    priority);
	return(getpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid))
	       == priority);
}
#endif /* UNIV_LINUX */

#ifdef UNIV_DEBUG
/** Loop used to disable page cleaner threads. */
static
void
buf_flush_page_cleaner_disabled_loop(void)
{
	ut_ad(page_cleaner != NULL);

	if (!innodb_page_cleaner_disabled_debug) {
		/* We return to avoid entering and exiting mutex. */
		return;
	}

	mutex_enter(&page_cleaner->mutex);
	page_cleaner->n_disabled_debug++;
	mutex_exit(&page_cleaner->mutex);

	while (innodb_page_cleaner_disabled_debug
	       && srv_shutdown_state == SRV_SHUTDOWN_NONE
	       && page_cleaner->is_running) {

		os_thread_sleep(100000); /* [A] */
	}

	/* We need to wait for threads exiting here, otherwise we would
	encounter problem when we quickly perform following steps:
		1) SET GLOBAL innodb_page_cleaner_disabled_debug = 1;
		2) SET GLOBAL innodb_page_cleaner_disabled_debug = 0;
		3) SET GLOBAL innodb_page_cleaner_disabled_debug = 1;
	That's because after step 1 this thread could still be sleeping
	inside the loop above at [A] and steps 2, 3 could happen before
	this thread wakes up from [A]. In such case this thread would
	not re-increment n_disabled_debug and we would be waiting for
	him forever in buf_flush_page_cleaner_disabled_debug_update(...).

	Therefore we are waiting in step 2 for this thread exiting here. */

	mutex_enter(&page_cleaner->mutex);
	page_cleaner->n_disabled_debug--;
	mutex_exit(&page_cleaner->mutex);
}

/** Disables page cleaner threads (coordinator and workers).
It's used by: SET GLOBAL innodb_page_cleaner_disabled_debug = 1 (0).
@param[in]	thd		thread handle
@param[in]	var		pointer to system variable
@param[out]	var_ptr		where the formal string goes
@param[in]	save		immediate result from check function */
void
buf_flush_page_cleaner_disabled_debug_update(
	THD*				thd,
	struct st_mysql_sys_var*	var,
	void*				var_ptr,
	const void*			save)
{
	if (page_cleaner == NULL) {
		return;
	}

	if (!*static_cast<const my_bool*>(save)) {
		if (!innodb_page_cleaner_disabled_debug) {
			return;
		}

		innodb_page_cleaner_disabled_debug = false;

		/* Enable page cleaner threads. */
		while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
			mutex_enter(&page_cleaner->mutex);
			const ulint n = page_cleaner->n_disabled_debug;
			mutex_exit(&page_cleaner->mutex);
			/* Check if all threads have been enabled, to avoid
			problem when we decide to re-disable them soon. */
			if (n == 0) {
				break;
			}
		}
		return;
	}

	if (innodb_page_cleaner_disabled_debug) {
		return;
	}

	innodb_page_cleaner_disabled_debug = true;

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
		/* Workers are possibly sleeping on is_requested.

		We have to wake them, otherwise they could possibly
		have never noticed, that they should be disabled,
		and we would wait for them here forever.

		That's why we have sleep-loop instead of simply
		waiting on some disabled_debug_event. */
		os_event_set(page_cleaner->is_requested);

		mutex_enter(&page_cleaner->mutex);

		ut_ad(page_cleaner->n_disabled_debug
		      <= srv_n_page_cleaners);

		if (page_cleaner->n_disabled_debug
		    == srv_n_page_cleaners) {

			mutex_exit(&page_cleaner->mutex);
			break;
		}

		mutex_exit(&page_cleaner->mutex);

		os_thread_sleep(100000);
	}
}
#endif /* UNIV_DEBUG */

/******************************************************************//**
page_cleaner thread tasked with flushing dirty pages from the buffer
pools. As of now we'll have only one coordinator.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(buf_flush_page_cleaner_coordinator)(
/*===============================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ulint	next_loop_time = ut_time_ms() + 1000;
	ulint	n_flushed = 0;
	ulint	last_activity = srv_get_activity_count();
	ulint	last_pages = 0;

	my_thread_init();

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(page_cleaner_thread_key);
#endif /* UNIV_PFS_THREAD */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "page_cleaner thread running, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

#ifdef UNIV_LINUX
	/* linux might be able to set different setting for each thread.
	worth to try to set high priority for page cleaner threads */
	if (buf_flush_page_cleaner_set_priority(
		buf_flush_page_cleaner_priority)) {

		ib::info() << "page_cleaner coordinator priority: "
			<< buf_flush_page_cleaner_priority;
	} else {
		ib::info() << "If the mysqld execution user is authorized,"
		" page cleaner thread priority can be changed."
		" See the man page of setpriority().";
	}
#endif /* UNIV_LINUX */

	buf_page_cleaner_is_active = true;

	while (!srv_read_only_mode
	       && srv_shutdown_state == SRV_SHUTDOWN_NONE
	       && recv_sys->heap != NULL) {
		/* treat flushing requests during recovery. */
		ulint	n_flushed_lru = 0;
		ulint	n_flushed_list = 0;

		os_event_wait(recv_sys->flush_start);

		if (srv_shutdown_state != SRV_SHUTDOWN_NONE
		    || recv_sys->heap == NULL) {
			break;
		}

		switch (recv_sys->flush_type) {
		case BUF_FLUSH_LRU:
			/* Flush pages from end of LRU if required */
			pc_request(0, LSN_MAX);
			while (pc_flush_slot() > 0) {}
			pc_wait_finished(&n_flushed_lru, &n_flushed_list);
			break;

		case BUF_FLUSH_LIST:
			/* Flush all pages */
			do {
				pc_request(ULINT_MAX, LSN_MAX);
				while (pc_flush_slot() > 0) {}
			} while (!pc_wait_finished(&n_flushed_lru,
						   &n_flushed_list));
			break;

		default:
			ut_ad(0);
		}

		os_event_reset(recv_sys->flush_start);
		os_event_set(recv_sys->flush_end);
	}

	os_event_wait(buf_flush_event);

	ulint		ret_sleep = 0;
	ulint		n_evicted = 0;
	ulint		n_flushed_last = 0;
	ulint		warn_interval = 1;
	ulint		warn_count = 0;
	int64_t		sig_count = os_event_reset(buf_flush_event);

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {

		/* The page_cleaner skips sleep if the server is
		idle and there are no pending IOs in the buffer pool
		and there is work to do. */
		if (srv_check_activity(last_activity)
		    || buf_get_n_pending_read_ios()
		    || n_flushed == 0) {

			ret_sleep = pc_sleep_if_needed(
				next_loop_time, sig_count);

			if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
				break;
			}
		} else if (ut_time_ms() > next_loop_time) {
			ret_sleep = OS_SYNC_TIME_EXCEEDED;
		} else {
			ret_sleep = 0;
		}

		sig_count = os_event_reset(buf_flush_event);

		if (ret_sleep == OS_SYNC_TIME_EXCEEDED) {
			ulint	curr_time = ut_time_ms();

			if (curr_time > next_loop_time + 3000) {
				if (warn_count == 0) {
					ib::info() << "page_cleaner: 1000ms"
						" intended loop took "
						<< 1000 + curr_time
						   - next_loop_time
						<< "ms. The settings might not"
						" be optimal. (flushed="
						<< n_flushed_last
						<< " and evicted="
						<< n_evicted
						<< ", during the time.)";
					if (warn_interval > 300) {
						warn_interval = 600;
					} else {
						warn_interval *= 2;
					}

					warn_count = warn_interval;
				} else {
					--warn_count;
				}
			} else {
				/* reset counter */
				warn_interval = 1;
				warn_count = 0;
			}

			next_loop_time = curr_time + 1000;
			n_flushed_last = n_evicted = 0;
		}

		if (ret_sleep != OS_SYNC_TIME_EXCEEDED
		    && srv_flush_sync
		    && buf_flush_sync_lsn > 0) {
			/* woke up for flush_sync */
			mutex_enter(&page_cleaner->mutex);
			lsn_t	lsn_limit = buf_flush_sync_lsn;
			buf_flush_sync_lsn = 0;
			mutex_exit(&page_cleaner->mutex);

			/* Request flushing for threads */
			pc_request(ULINT_MAX, lsn_limit);

			ulint tm = ut_time_ms();

			/* Coordinator also treats requests */
			while (pc_flush_slot() > 0) {}

			/* only coordinator is using these counters,
			so no need to protect by lock. */
			page_cleaner->flush_time += ut_time_ms() - tm;
			page_cleaner->flush_pass++;

			/* Wait for all slots to be finished */
			ulint	n_flushed_lru = 0;
			ulint	n_flushed_list = 0;
			pc_wait_finished(&n_flushed_lru, &n_flushed_list);

			if (n_flushed_list > 0 || n_flushed_lru > 0) {
				buf_flush_stats(n_flushed_list, n_flushed_lru);

				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_FLUSH_SYNC_TOTAL_PAGE,
					MONITOR_FLUSH_SYNC_COUNT,
					MONITOR_FLUSH_SYNC_PAGES,
					n_flushed_lru + n_flushed_list);
			}

			n_flushed = n_flushed_lru + n_flushed_list;

		} else if (srv_check_activity(last_activity)) {
			ulint	n_to_flush;
			lsn_t	lsn_limit = 0;

			/* Estimate pages from flush_list to be flushed */
			if (ret_sleep == OS_SYNC_TIME_EXCEEDED) {
				last_activity = srv_get_activity_count();
				n_to_flush =
					page_cleaner_flush_pages_recommendation(
						&lsn_limit, last_pages);
			} else {
				n_to_flush = 0;
			}

			/* Request flushing for threads */
			pc_request(n_to_flush, lsn_limit);

			ulint tm = ut_time_ms();

			/* Coordinator also treats requests */
			while (pc_flush_slot() > 0) {
				/* No op */
			}

			/* only coordinator is using these counters,
			so no need to protect by lock. */
			page_cleaner->flush_time += ut_time_ms() - tm;
			page_cleaner->flush_pass++ ;

			/* Wait for all slots to be finished */
			ulint	n_flushed_lru = 0;
			ulint	n_flushed_list = 0;

			pc_wait_finished(&n_flushed_lru, &n_flushed_list);

			if (n_flushed_list > 0 || n_flushed_lru > 0) {
				buf_flush_stats(n_flushed_list, n_flushed_lru);
			}

			if (ret_sleep == OS_SYNC_TIME_EXCEEDED) {
				last_pages = n_flushed_list;
			}

			n_evicted += n_flushed_lru;
			n_flushed_last += n_flushed_list;

			n_flushed = n_flushed_lru + n_flushed_list;

			if (n_flushed_lru) {
				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_LRU_BATCH_FLUSH_TOTAL_PAGE,
					MONITOR_LRU_BATCH_FLUSH_COUNT,
					MONITOR_LRU_BATCH_FLUSH_PAGES,
					n_flushed_lru);
			}

			if (n_flushed_list) {
				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_FLUSH_ADAPTIVE_TOTAL_PAGE,
					MONITOR_FLUSH_ADAPTIVE_COUNT,
					MONITOR_FLUSH_ADAPTIVE_PAGES,
					n_flushed_list);
			}

		} else if (ret_sleep == OS_SYNC_TIME_EXCEEDED) {
			/* no activity, slept enough */
			buf_flush_lists(PCT_IO(100), LSN_MAX, &n_flushed);

			n_flushed_last += n_flushed;

			if (n_flushed) {
				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_FLUSH_BACKGROUND_TOTAL_PAGE,
					MONITOR_FLUSH_BACKGROUND_COUNT,
					MONITOR_FLUSH_BACKGROUND_PAGES,
					n_flushed);

			}

		} else {
			/* no activity, but woken up by event */
			n_flushed = 0;
		}

		ut_d(buf_flush_page_cleaner_disabled_loop());
	}

	ut_ad(srv_shutdown_state > 0);
	if (srv_fast_shutdown == 2
	    || srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS) {
		/* In very fast shutdown or when innodb failed to start, we
		simulate a crash of the buffer pool. We are not required to do
		any flushing. */
		goto thread_exit;
	}

	/* In case of normal and slow shutdown the page_cleaner thread
	must wait for all other activity in the server to die down.
	Note that we can start flushing the buffer pool as soon as the
	server enters shutdown phase but we must stay alive long enough
	to ensure that any work done by the master or purge threads is
	also flushed.
	During shutdown we pass through two stages. In the first stage,
	when SRV_SHUTDOWN_CLEANUP is set other threads like the master
	and the purge threads may be working as well. We start flushing
	the buffer pool but can't be sure that no new pages are being
	dirtied until we enter SRV_SHUTDOWN_FLUSH_PHASE phase. */

	do {
		pc_request(ULINT_MAX, LSN_MAX);

		while (pc_flush_slot() > 0) {}

		ulint	n_flushed_lru = 0;
		ulint	n_flushed_list = 0;
		pc_wait_finished(&n_flushed_lru, &n_flushed_list);

		n_flushed = n_flushed_lru + n_flushed_list;

		/* We sleep only if there are no pages to flush */
		if (n_flushed == 0) {
			os_thread_sleep(100000);
		}
	} while (srv_shutdown_state == SRV_SHUTDOWN_CLEANUP);

	/* At this point all threads including the master and the purge
	thread must have been suspended. */
	ut_a(srv_get_active_thread_type() == SRV_NONE);
	ut_a(srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE);

	/* We can now make a final sweep on flushing the buffer pool
	and exit after we have cleaned the whole buffer pool.
	It is important that we wait for any running batch that has
	been triggered by us to finish. Otherwise we can end up
	considering end of that batch as a finish of our final
	sweep and we'll come out of the loop leaving behind dirty pages
	in the flush_list */
	buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
	buf_flush_wait_LRU_batch_end();

	bool	success;

	do {
		pc_request(ULINT_MAX, LSN_MAX);

		while (pc_flush_slot() > 0) {}

		ulint	n_flushed_lru = 0;
		ulint	n_flushed_list = 0;
		success = pc_wait_finished(&n_flushed_lru, &n_flushed_list);

		n_flushed = n_flushed_lru + n_flushed_list;

		buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
		buf_flush_wait_LRU_batch_end();

	} while (!success || n_flushed > 0);

	/* Some sanity checks */
	ut_a(srv_get_active_thread_type() == SRV_NONE);
	ut_a(srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE);

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t* buf_pool = buf_pool_from_array(i);
		ut_a(UT_LIST_GET_LEN(buf_pool->flush_list) == 0);
	}

	/* We have lived our life. Time to die. */

thread_exit:
	/* All worker threads are waiting for the event here,
	and no more access to page_cleaner structure by them.
	Wakes worker threads up just to make them exit. */
	page_cleaner->is_running = false;
	os_event_set(page_cleaner->is_requested);

	buf_flush_page_cleaner_close();
	buf_page_cleaner_is_active = false;
#if defined (UNIV_PMEMOBJ_BUF_FLUSHER)
	printf ("PMEM_DEBUG buf_page_cleaner_is_active = false\n");

#if defined (UNIV_PMEMOBJ_LSB)
	PMEM_FLUSHER* flusher = gb_pmw->plsb->flusher;		
#else
	PMEM_FLUSHER* flusher = gb_pmw->pbuf->flusher;		
#endif
	os_event_set(flusher->is_req_not_empty);
#endif //UNIV_PMEMOBJ_LSB

#if defined (UNIV_PMEMOBJ_PART_PL)
	//wake up the sleeping threads to close them
	
	os_event_set(gb_pmw->ppl->flusher->is_log_req_not_empty);
#endif
	my_thread_end();

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/******************************************************************//**
Worker thread of page_cleaner.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(buf_flush_page_cleaner_worker)(
/*==========================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	my_thread_init();

	mutex_enter(&page_cleaner->mutex);
	page_cleaner->n_workers++;
	mutex_exit(&page_cleaner->mutex);

#ifdef UNIV_LINUX
	/* linux might be able to set different setting for each thread
	worth to try to set high priority for page cleaner threads */
	if (buf_flush_page_cleaner_set_priority(
		buf_flush_page_cleaner_priority)) {

		ib::info() << "page_cleaner worker priority: "
			<< buf_flush_page_cleaner_priority;
	}
#endif /* UNIV_LINUX */

	while (true) {
		os_event_wait(page_cleaner->is_requested);

		ut_d(buf_flush_page_cleaner_disabled_loop());

		if (!page_cleaner->is_running) {
			break;
		}

		pc_flush_slot();
	}

	mutex_enter(&page_cleaner->mutex);
	page_cleaner->n_workers--;
	mutex_exit(&page_cleaner->mutex);

	my_thread_end();

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/*******************************************************************//**
Synchronously flush dirty blocks from the end of the flush list of all buffer
pool instances.
NOTE: The calling thread is not allowed to own any latches on pages! */
void
buf_flush_sync_all_buf_pools(void)
/*==============================*/
{
	bool success;
	do {
		success = buf_flush_lists(ULINT_MAX, LSN_MAX, NULL);
		buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
	} while (!success);

	ut_a(success);
}

/** Request IO burst and wake page_cleaner up.
@param[in]	lsn_limit	upper limit of LSN to be flushed */
void
buf_flush_request_force(
	lsn_t	lsn_limit)
{
	/* adjust based on lsn_avg_rate not to get old */
	lsn_t	lsn_target = lsn_limit + lsn_avg_rate * 3;

	mutex_enter(&page_cleaner->mutex);
	if (lsn_target > buf_flush_sync_lsn) {
		buf_flush_sync_lsn = lsn_target;
	}
	mutex_exit(&page_cleaner->mutex);

	os_event_set(buf_flush_event);
}
#if defined (UNIV_PMEMOBJ_PART_PL)
void
pm_ppl_buf_flush_recv_note_modification(
	PMEMobjpool*		pop,
	PMEM_PAGE_PART_LOG*	ppl,
	buf_block_t*    block,
	lsn_t       start_lsn,
	lsn_t       end_lsn) 
{
	buf_page_mutex_enter(block);

	block->page.newest_modification = end_lsn;
	if (!block->page.oldest_modification) {
		buf_pool_t*	buf_pool = buf_pool_from_block(block);

		buf_flush_insert_sorted_into_flush_list(
			buf_pool, block, start_lsn);
	} else {
		ut_ad(block->page.oldest_modification <= start_lsn);
	}

	buf_page_mutex_exit(block);
}
/*
 *Called by pm_ppl_checkpoint()
 * */
void
pm_ppl_buf_flush_request_force(
	uint64_t	lsn_limit)
{
	lsn_t lsn_target = lsn_limit;

	mutex_enter(&page_cleaner->mutex);
	if (lsn_target > buf_flush_sync_lsn) {
		buf_flush_sync_lsn = lsn_target;
	}
	mutex_exit(&page_cleaner->mutex);
	os_event_set(buf_flush_event);
}
#endif //UNIV_PMEMOBJ_PART_PL

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG

/** Functor to validate the flush list. */
struct	Check {
	void	operator()(const buf_page_t* elem)
	{
		ut_a(elem->in_flush_list);
	}
};

/******************************************************************//**
Validates the flush list.
@return TRUE if ok */
static
ibool
buf_flush_validate_low(
/*===================*/
	buf_pool_t*	buf_pool)		/*!< in: Buffer pool instance */
{
	buf_page_t*		bpage;
	const ib_rbt_node_t*	rnode = NULL;
	Check			check;

#if defined (UNIV_PMEMOBJ_PL) || defined (UNIV_SKIPLOG)
	//In PL-NVM we do not use pageLSN in the flush list
	return (TRUE);
#endif //UNIV_PMEMOBJ_PL
	ut_ad(buf_flush_list_mutex_own(buf_pool));

	ut_list_validate(buf_pool->flush_list, check);

	bpage = UT_LIST_GET_FIRST(buf_pool->flush_list);

	/* If we are in recovery mode i.e.: flush_rbt != NULL
	then each block in the flush_list must also be present
	in the flush_rbt. */
	if (buf_pool->flush_rbt != NULL) {
		rnode = rbt_first(buf_pool->flush_rbt);
	}

	while (bpage != NULL) {
		const lsn_t	om = bpage->oldest_modification;

		ut_ad(buf_pool_from_bpage(bpage) == buf_pool);

		ut_ad(bpage->in_flush_list);

		/* A page in buf_pool->flush_list can be in
		BUF_BLOCK_REMOVE_HASH state. This happens when a page
		is in the middle of being relocated. In that case the
		original descriptor can have this state and still be
		in the flush list waiting to acquire the
		buf_pool->flush_list_mutex to complete the relocation. */
		ut_a(buf_page_in_file(bpage)
		     || buf_page_get_state(bpage) == BUF_BLOCK_REMOVE_HASH);
		ut_a(om > 0);

		if (buf_pool->flush_rbt != NULL) {
			buf_page_t**	prpage;

			ut_a(rnode != NULL);
			prpage = rbt_value(buf_page_t*, rnode);

			ut_a(*prpage != NULL);
			ut_a(*prpage == bpage);
			rnode = rbt_next(buf_pool->flush_rbt, rnode);
		}

		bpage = UT_LIST_GET_NEXT(list, bpage);

		ut_a(bpage == NULL || om >= bpage->oldest_modification);
	}

	/* By this time we must have exhausted the traversal of
	flush_rbt (if active) as well. */
	ut_a(rnode == NULL);

	return(TRUE);
}

/******************************************************************//**
Validates the flush list.
@return TRUE if ok */
ibool
buf_flush_validate(
/*===============*/
	buf_pool_t*	buf_pool)	/*!< buffer pool instance */
{
	ibool	ret;

	buf_flush_list_mutex_enter(buf_pool);

	ret = buf_flush_validate_low(buf_pool);

	buf_flush_list_mutex_exit(buf_pool);

	return(ret);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#endif /* !UNIV_HOTBACKUP */

/******************************************************************//**
Check if there are any dirty pages that belong to a space id in the flush
list in a particular buffer pool.
@return number of dirty pages present in a single buffer pool */
ulint
buf_pool_get_dirty_pages_count(
/*===========================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool */
	ulint		id,		/*!< in: space id to check */
	FlushObserver*	observer)	/*!< in: flush observer to check */

{
	ulint		count = 0;

	buf_pool_mutex_enter(buf_pool);
	buf_flush_list_mutex_enter(buf_pool);

	buf_page_t*	bpage;

	for (bpage = UT_LIST_GET_FIRST(buf_pool->flush_list);
	     bpage != 0;
	     bpage = UT_LIST_GET_NEXT(list, bpage)) {

		ut_ad(buf_page_in_file(bpage));
		ut_ad(bpage->in_flush_list);
		ut_ad(bpage->oldest_modification > 0);

		if ((observer != NULL
		     && observer == bpage->flush_observer)
		    || (observer == NULL
			&& id == bpage->id.space())) {
			++count;
		}
	}

	buf_flush_list_mutex_exit(buf_pool);
	buf_pool_mutex_exit(buf_pool);

	return(count);
}

/******************************************************************//**
Check if there are any dirty pages that belong to a space id in the flush list.
@return number of dirty pages present in all the buffer pools */
ulint
buf_flush_get_dirty_pages_count(
/*============================*/
	ulint		id,		/*!< in: space id to check */
	FlushObserver*	observer)	/*!< in: flush observer to check */
{
	ulint		count = 0;

	for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		count += buf_pool_get_dirty_pages_count(buf_pool, id, observer);
	}

	return(count);
}

/** FlushObserver constructor
@param[in]	space_id	table space id
@param[in]	trx		trx instance
@param[in]	stage		performance schema accounting object,
used by ALTER TABLE. It is passed to log_preflush_pool_modified_pages()
for accounting. */
FlushObserver::FlushObserver(
	ulint			space_id,
	trx_t*			trx,
	ut_stage_alter_t*	stage)
	:
	m_space_id(space_id),
	m_trx(trx),
	m_stage(stage),
	m_interrupted(false)
{
	m_flushed = UT_NEW_NOKEY(std::vector<ulint>(srv_buf_pool_instances));
	m_removed = UT_NEW_NOKEY(std::vector<ulint>(srv_buf_pool_instances));

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		m_flushed->at(i) = 0;
		m_removed->at(i) = 0;
	}

#ifdef FLUSH_LIST_OBSERVER_DEBUG
		ib::info() << "FlushObserver constructor: " << m_trx->id;
#endif /* FLUSH_LIST_OBSERVER_DEBUG */
}

/** FlushObserver deconstructor */
FlushObserver::~FlushObserver()
{
	ut_ad(buf_flush_get_dirty_pages_count(m_space_id, this) == 0);

	UT_DELETE(m_flushed);
	UT_DELETE(m_removed);

#ifdef FLUSH_LIST_OBSERVER_DEBUG
		ib::info() << "FlushObserver deconstructor: " << m_trx->id;
#endif /* FLUSH_LIST_OBSERVER_DEBUG */
}

/** Check whether trx is interrupted
@return true if trx is interrupted */
bool
FlushObserver::check_interrupted()
{
	if (trx_is_interrupted(m_trx)) {
		interrupted();

		return(true);
	}

	return(false);
}

/** Notify observer of a flush
@param[in]	buf_pool	buffer pool instance
@param[in]	bpage		buffer page to flush */
void
FlushObserver::notify_flush(
	buf_pool_t*	buf_pool,
	buf_page_t*	bpage)
{
	ut_ad(buf_pool_mutex_own(buf_pool));

	m_flushed->at(buf_pool->instance_no)++;

	if (m_stage != NULL) {
		m_stage->inc();
	}

#ifdef FLUSH_LIST_OBSERVER_DEBUG
	ib::info() << "Flush <" << bpage->id.space()
		   << ", " << bpage->id.page_no() << ">";
#endif /* FLUSH_LIST_OBSERVER_DEBUG */
}

/** Notify observer of a remove
@param[in]	buf_pool	buffer pool instance
@param[in]	bpage		buffer page flushed */
void
FlushObserver::notify_remove(
	buf_pool_t*	buf_pool,
	buf_page_t*	bpage)
{
	ut_ad(buf_pool_mutex_own(buf_pool));

	m_removed->at(buf_pool->instance_no)++;

#ifdef FLUSH_LIST_OBSERVER_DEBUG
	ib::info() << "Remove <" << bpage->id.space()
		   << ", " << bpage->id.page_no() << ">";
#endif /* FLUSH_LIST_OBSERVER_DEBUG */
}

/** Flush dirty pages and wait. */
void
FlushObserver::flush()
{
	buf_remove_t	buf_remove;

	if (m_interrupted) {
		buf_remove = BUF_REMOVE_FLUSH_NO_WRITE;
	} else {
		buf_remove = BUF_REMOVE_FLUSH_WRITE;

		if (m_stage != NULL) {
			ulint	pages_to_flush =
				buf_flush_get_dirty_pages_count(
					m_space_id, this);

			m_stage->begin_phase_flush(pages_to_flush);
		}
	}

	/* Flush or remove dirty pages. */
	buf_LRU_flush_or_remove_pages(m_space_id, buf_remove, m_trx);

	/* Wait for all dirty pages were flushed. */
	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		while (!is_complete(i)) {

			os_thread_sleep(2000);
		}
	}
}

/////////////////////// PART LOG IMPLEMENT/////////////
#if defined (UNIV_PMEMOBJ_PART_PL)
//Defined in my_pmemobj.h

/////////// FLUSHER /////////////////////
/*
 * Init flusher
 * @param[in] size: number of items in the array
 * */
PMEM_LOG_FLUSHER*
pm_log_flusher_init(
				const size_t	size,
				FLUSHER_TYPE	type) {
	PMEM_LOG_FLUSHER* flusher;
	ulint i;

	flusher = static_cast <PMEM_LOG_FLUSHER*> (
			ut_zalloc_nokey(sizeof(PMEM_LOG_FLUSHER)));

	flusher->type = type;

	mutex_create(LATCH_ID_PM_LOG_FLUSHER, &flusher->mutex);

	flusher->is_log_req_not_empty = os_event_create("flusher_is_log_req_not_empty");
	flusher->is_log_req_full = os_event_create("flusher_is_log_req_full");
	flusher->is_log_all_finished = os_event_create("flusher_is_log_all_finished");
	flusher->is_log_all_closed = os_event_create("flusher_is_log_all_closed");
	flusher->size = size;
	flusher->tail = 0;
	flusher->n_requested = 0;
	flusher->is_running = false;

		
	//flush array
	flusher->flush_list_arr = static_cast <PMEM_PAGE_LOG_BUF**> (	calloc(size, sizeof(PMEM_PAGE_LOG_BUF*)));
	for (i = 0; i < size; i++) {
		flusher->flush_list_arr[i] = NULL;
	}	

	return flusher;
}

void
pm_log_flusher_close(
		PMEM_LOG_FLUSHER*	flusher) {
	ulint i;
	
	//wait for all workers finish their work
	while (flusher->n_workers > 0) {
		os_thread_sleep(10000);
	}
	
	switch (flusher->type){
		case CATCHER_LOG_BUF:
			break;
		case FLUSHER_LOG_BUF:
		default:
			//free flush list
			for (i = 0; i < flusher->size; i++) {
				if (flusher->flush_list_arr[i]){
					flusher->flush_list_arr[i] = NULL;
				}
			}	
			if (flusher->flush_list_arr){
				free(flusher->flush_list_arr);
				flusher->flush_list_arr = NULL;
			}	
			break;
	} //end switch(flusher->type)


	mutex_destroy(&flusher->mutex);

	os_event_destroy(flusher->is_log_req_not_empty);
	os_event_destroy(flusher->is_log_req_full);
	//os_event_destroy(buf->flusher->is_flush_full);

	os_event_destroy(flusher->is_log_all_finished);
	os_event_destroy(flusher->is_log_all_closed);
	//printf("destroys mutex and events ok\n");	

	if(flusher){
		flusher = NULL;
		free(flusher);
	}
	//printf("free flusher ok\n");
}
/*
 *The coordinator
 Handle start/stop all workers
 * */
extern "C"
os_thread_ret_t
DECLARE_THREAD(pm_log_flusher_coordinator)(
/*===============================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{


	my_thread_init();

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(pm_log_flusher_thread_key);
#endif /* UNIV_PFS_THREAD */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "coordinator pm_log_flusher thread running, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

#ifdef UNIV_LINUX
	/* linux might be able to set different setting for each thread.
	worth to try to set high priority for page cleaner threads */
	if (buf_flush_page_cleaner_set_priority(
		buf_flusher_priority)) {

		ib::info() << "pm_list_cleaner coordinator priority: "
			<< buf_flush_page_cleaner_priority;
	} else {
		ib::info() << "If the mysqld execution user is authorized,"
		" page cleaner thread priority can be changed."
		" See the man page of setpriority().";
	}
#endif /* UNIV_LINUX */

	PMEM_LOG_FLUSHER* flusher = gb_pmw->ppl->flusher;

	flusher->is_running = true;
	//ulint ret;

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
		os_event_wait(flusher->is_log_all_finished);

		if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
			break;
		}
		//the workers are idle and the server is running, keep waiting
		os_event_reset(flusher->is_log_all_finished);
	} //end while thread

	flusher->is_running = false;
	//trigger waiting workers to stop
	os_event_set(flusher->is_log_req_not_empty);
	//wait for all workers closed
	printf("wait all pm_log workers close...\n");
	os_event_wait(flusher->is_log_all_closed);

	printf("all pm_log workers closed\n");
	my_thread_end();

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}


/*Worker thread of log flusher.
 * Managed by the coordinator thread
 * number of threads are equal to the number of cleaner threds from config
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(pm_log_flusher_worker)(
/*==========================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ulint i;

	PMEM_LOG_FLUSHER* flusher = gb_pmw->ppl->flusher;

	PMEM_PAGE_LOG_BUF* plogbuf = NULL;

	my_thread_init();

	mutex_enter(&flusher->mutex);
	flusher->n_workers++;
	os_event_reset(flusher->is_log_all_closed);
	mutex_exit(&flusher->mutex);

	while (true) {
		//worker thread wait until there is is_requested signal 
retry:
		os_event_wait(flusher->is_log_req_not_empty);
		//looking for a full list in wait-list and flush it
		mutex_enter(&flusher->mutex);
		if (flusher->n_requested > 0) {
			for (i = 0; i < flusher->size; i++) {
				plogbuf = flusher->flush_list_arr[i];
				if (plogbuf != NULL)
				{
					//***this call aio_batch ***
					pm_log_flush_log_buf(gb_pmw->pop, gb_pmw->ppl, plogbuf);
					flusher->n_requested--;
					os_event_set(flusher->is_log_req_full);
					//we can set the pointer to null after the pm_buf_flush_list finished
					flusher->flush_list_arr[i] = NULL;
					break;
				}
			}

		} //end if flusher->n_requested > 0

		if (flusher->n_requested == 0) {
			if (buf_page_cleaner_is_active) {
				//buf_page_cleaner is running, start waiting
				os_event_reset(flusher->is_log_req_not_empty);
			}
			else {
				mutex_exit(&flusher->mutex);
				break;
			}
		}
		mutex_exit(&flusher->mutex);
	} //end while thread

	mutex_enter(&flusher->mutex);
	flusher->n_workers--;
	if (flusher->n_workers == 0) {
		printf("The last log worker is closing\n");
		//os_event_set(flusher->is_all_closed);
	}
	mutex_exit(&flusher->mutex);

	my_thread_end();

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/////////// END FLUSHER /////////////////////

/////////// REDOER /////////////////////

/*
 * Init the REDOER
 * @param[in] size: The number of items in the array,
 * should equal to the number of hashed line
 * */
PMEM_LOG_REDOER*
pm_log_redoer_init(
				const size_t	size) {
	PMEM_LOG_REDOER* redoer;
	ulint i;

	redoer = static_cast <PMEM_LOG_REDOER*> (
			ut_zalloc_nokey(sizeof(PMEM_LOG_REDOER)));

	mutex_create(LATCH_ID_PM_LOG_REDOER, &redoer->mutex);

	redoer->is_log_req_not_empty = os_event_create("redoer_is_log_req_not_empty");
	redoer->is_log_req_full = os_event_create("redoer_is_log_req_full");
	redoer->is_log_all_finished = os_event_create("redoer_is_log_all_finished");
	redoer->is_log_all_closed = os_event_create("redoer_is_log_all_closed");
	redoer->size = size;
	redoer->tail = 0;
	redoer->n_requested = 0;
	redoer->is_running = false;

	redoer->hashed_line_arr = static_cast <PMEM_PAGE_LOG_HASHED_LINE**> (	calloc(size, sizeof(PMEM_PAGE_LOG_HASHED_LINE*)));
	for (i = 0; i < size; i++) {
		redoer->hashed_line_arr[i] = NULL;
	}	

	return redoer;
}

void
pm_log_redoer_close(
		PMEM_LOG_REDOER*	redoer) {
	ulint i;
	
	//wait for all workers finish their work
	while (redoer->n_workers > 0) {
		os_thread_sleep(10000);
	}

	for (i = 0; i < redoer->size; i++) {
		if (redoer->hashed_line_arr[i]){
			//free(buf->flusher->flush_list_arr[i]);
			redoer->hashed_line_arr[i] = NULL;
		}
			
	}	

	if (redoer->hashed_line_arr){
		free(redoer->hashed_line_arr);
		redoer->hashed_line_arr = NULL;
	}	

	mutex_destroy(&redoer->mutex);

	os_event_destroy(redoer->is_log_req_not_empty);
	os_event_destroy(redoer->is_log_req_full);

	os_event_destroy(redoer->is_log_all_finished);
	os_event_destroy(redoer->is_log_all_closed);

	if(redoer){
		redoer = NULL;
		free(redoer);
	}
	//printf("free flusher ok\n");
}
/*
 *The coordinator
 Handle start/stop all workers
 * */
extern "C"
os_thread_ret_t
DECLARE_THREAD(pm_log_redoer_coordinator)(
/*===============================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{


	my_thread_init();

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(pm_log_redoer_thread_key);
#endif /* UNIV_PFS_THREAD */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "coordinator pm_log_flusher thread running, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

	PMEM_LOG_REDOER* redoer = gb_pmw->ppl->redoer;

	redoer->is_running = true;

	while (!gb_pmw->ppl->is_redoing_done) {
		os_event_wait(redoer->is_log_all_finished);

		if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
			break;
		}
		//the workers are idle and the server is running, keep waiting
		os_event_reset(redoer->is_log_all_finished);
	} //end while thread

	redoer->is_running = false;
	//trigger waiting workers to stop
	os_event_set(redoer->is_log_req_not_empty);
	//wait for all workers closed
	printf("wait all redoers close...\n");
	os_event_wait(redoer->is_log_all_closed);

	printf("all redoers closed\n");
	my_thread_end();

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/*Worker thread of log redoer.
 * Managed by the coordinator thread
 * number of threads are defined in header file
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(pm_log_redoer_worker)(
/*==========================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ulint i;
	pid_t thread_id;
	ulint idx;
	ulint lines_per_thread;
	//int dist_mode = 2;
	int dist_mode = 1;

	ulint start_time, end_time, e_time;

	PMEM_LOG_REDOER* redoer = gb_pmw->ppl->redoer;

	PMEM_PAGE_LOG_HASHED_LINE* pline = NULL;
	PMEM_RECV_LINE* recv_line = NULL;

	my_thread_init();

	mutex_enter(&redoer->mutex);
	idx = redoer->n_workers;
	redoer->n_workers++;
	os_event_reset(redoer->is_log_all_closed);
	mutex_exit(&redoer->mutex);
	
	//thread_id = os_thread_pf(os_thread_get_curr_id());
	//lines_per_thread = redoer->size / (srv_ppl_n_redoer_threads - 1);
	lines_per_thread = (redoer->size - 1) / srv_ppl_n_redoer_threads + 1;

	//thread_id = syscall(SYS_gettid);
	//idx = thread_id % srv_ppl_n_redoer_threads;

	printf("Redoers thread %zu lines_per_thread %zu created \n",idx, lines_per_thread);

	while (true) {
		//worker thread wait until there is is_requested signal 
retry:
		os_event_wait(redoer->is_log_req_not_empty);

		//waked up, looking for a hashed line and REDO it

		if(redoer->n_remains == 0){
			//do nothing
			break;
		}
		/*Method 1: sequential distribute*/
		//for (i = 0; i < redoer->size; i++) 
		/*Method 2: segment distribute*/
		for (i = idx * lines_per_thread;
			   	i < (idx + 1) * lines_per_thread &&
			   	i < redoer->size
				; i++) 
		/*Method 3: evently distribute*/
		//for (i = idx ;
		//	   	i < redoer->size
		//		; i+= srv_ppl_n_redoer_threads) 
		{
			if (dist_mode ==1)
				mutex_enter(&redoer->mutex);

			pline = redoer->hashed_line_arr[i];

			if (pline != NULL && !pline->is_redoing)
			{
				pline->is_redoing = true;
				recv_line = pline->recv_line;
				//do not hold the mutex during REDOing
				if (dist_mode ==1)
					mutex_exit(&redoer->mutex);

				/***this call REDOing for a line ***/
				if (redoer->phase == PMEM_REDO_PHASE1){
					//printf("PMEM_REDO: start REDO_PHASE1 (scan and parse) line %zu ...\n", pline->hashed_id);


					//start_time = ut_time_us(NULL);
					bool is_err = pm_ppl_redo_line(gb_pmw->pop, gb_pmw->ppl, pline);
					//end_time = ut_time_us(NULL);

					//recv_line->redo1_thread_id = idx; 	
					//recv_line->redo1_start_time = start_time;
					//recv_line->redo1_end_time = end_time;
					//recv_line->redo1_elapse_time = (end_time - start_time);

					if (is_err){
						printf("PMEM_REDO: error redoing line %zu \n", pline->hashed_id);
						assert(0);
					}
					//printf("PMEM_REDO: end REDO_PHASE1 (scan and parse) line %zu\n", pline->hashed_id);
				}
				else {
#if defined (UNIV_PMEMOBJ_PART_PL_DEBUG)
					printf("PMEM_REDO: start REDO_PHASE2 (applying) line %zu ...\n", pline->hashed_id);
#endif
					//start_time = ut_time_us(NULL);
					pm_ppl_recv_apply_hashed_line(
							gb_pmw->pop, gb_pmw->ppl,
							pline, pline->recv_line->is_ibuf_avail);
					//end_time = ut_time_us(NULL);

					//recv_line->redo2_thread_id = idx; 	
					//recv_line->redo2_start_time = start_time;
					//recv_line->redo2_end_time = end_time;
					//recv_line->redo2_elapse_time = (end_time - start_time);
#if defined (UNIV_PMEMOBJ_PART_PL_DEBUG)
					printf("PMEM_REDO: end REDO_PHASE2 (applying) line %zu\n", pline->hashed_id);
#endif
				}

				if (dist_mode ==1)
					mutex_enter(&redoer->mutex);

				redoer->hashed_line_arr[i] = NULL;
				//redoer->n_requested--;
				redoer->n_remains--;

				if (redoer->n_remains == 0){
					//this is the last REDO
					if (dist_mode ==1)
						mutex_exit(&redoer->mutex);
					break;
				}
			}
			if (dist_mode ==1)
				mutex_exit(&redoer->mutex);
		} //end for

		// after this for loop, all lines are either done REDO or REDOing by other threads, this thread has nothing to do
		break;
	} //end while thread

	mutex_enter(&redoer->mutex);
	redoer->n_workers--;
	if (redoer->n_workers == 0) {
		printf("The last log redoer is closing. Redo phase %zu redoer->n_remains %zu ppl->n_redoing_lines %zu\n",
				redoer->phase, redoer->n_remains, gb_pmw->ppl->n_redoing_lines);
		//trigger the coordinator (the pm_ppl_redo) to wakeup
		os_event_set(redoer->is_log_all_finished);
	}
	mutex_exit(&redoer->mutex);

	my_thread_end();

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}
/////////// END REDOER /////////////////////

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t pm_log_flusher_thread_key;
mysql_pfs_key_t pm_log_redoer_thread_key;
#endif /* UNIV_PFS_THREAD */

#endif //UNIV_PMEMOBJ_PART_PL


//////////////// PB-NVM//////////////////////////
#if defined (UNIV_PMEMOBJ_BUF)

#if defined (UNIV_PMEMOBJ_BUF_FLUSHER)
//******************* FLUSHER implementation **********/
PMEM_FLUSHER*
pm_flusher_init(
				const size_t	size) {
	PMEM_FLUSHER* flusher;
	ulint i;

 
	flusher = static_cast <PMEM_FLUSHER*> (
			ut_zalloc_nokey(sizeof(PMEM_FLUSHER)));

	mutex_create(LATCH_ID_PM_FLUSHER, &flusher->mutex);

	flusher->is_req_not_empty = os_event_create("flusher_is_req_not_empty");
	flusher->is_req_full = os_event_create("flusher_is_req_full");

	//flusher->is_flush_full = os_event_create("flusher_is_flush_full");

	flusher->is_all_finished = os_event_create("flusher_is_all_finished");
	flusher->is_all_closed = os_event_create("flusher_is_all_closed");
	flusher->size = size;
	flusher->tail = 0;
	flusher->n_requested = 0;
	flusher->is_running = false;

	flusher->flush_list_arr = static_cast <PMEM_BUF_BLOCK_LIST**> (	calloc(size, sizeof(PMEM_BUF_BLOCK_LIST*)));
	for (i = 0; i < size; i++) {
		flusher->flush_list_arr[i] = NULL;
	}	
#if defined (UNIV_PMEMOBJ_LSB)
	flusher->bucket_arr = static_cast <PMEM_LSB_HASH_BUCKET**> (	calloc(size, sizeof(PMEM_LSB_HASH_BUCKET*)));
	for (i = 0; i < size; i++) {
		flusher->bucket_arr[i] = NULL;
	}	
#endif
	return flusher;
}
void
pm_buf_flusher_close(
		PMEM_FLUSHER*	flusher) {
	ulint i;
	
	//wait for all workers finish their work
	while (flusher->n_workers > 0) {
		os_thread_sleep(10000);
	}

	for (i = 0; i < flusher->size; i++) {
		if (flusher->flush_list_arr[i]){
			//free(buf->flusher->flush_list_arr[i]);
			flusher->flush_list_arr[i] = NULL;
		}
			
	}	

	if (flusher->flush_list_arr){
		free(flusher->flush_list_arr);
		flusher->flush_list_arr = NULL;
	}	
	//printf("free array ok\n");

	mutex_destroy(&flusher->mutex);

	os_event_destroy(flusher->is_req_not_empty);
	os_event_destroy(flusher->is_req_full);
	//os_event_destroy(buf->flusher->is_flush_full);

	os_event_destroy(flusher->is_all_finished);
	os_event_destroy(flusher->is_all_closed);
	//printf("destroys mutex and events ok\n");	

	if(flusher){
		flusher = NULL;
		free(flusher);
	}
	//printf("free flusher ok\n");
}
/*
 *The coordinator
 Handle start/stop all workers
 * */
extern "C"
os_thread_ret_t
DECLARE_THREAD(pm_flusher_coordinator)(
/*===============================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{


	my_thread_init();

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(pm_flusher_thread_key);
#endif /* UNIV_PFS_THREAD */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "coordinator pm_flusher thread running, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

#ifdef UNIV_LINUX
	/* linux might be able to set different setting for each thread.
	worth to try to set high priority for page cleaner threads */
	if (buf_flush_page_cleaner_set_priority(
		buf_flusher_priority)) {

		ib::info() << "pm_list_cleaner coordinator priority: "
			<< buf_flush_page_cleaner_priority;
	} else {
		ib::info() << "If the mysqld execution user is authorized,"
		" page cleaner thread priority can be changed."
		" See the man page of setpriority().";
	}
#endif /* UNIV_LINUX */

#if defined (UNIV_PMEMOBJ_LSB)
	PMEM_FLUSHER* flusher = gb_pmw->plsb->flusher;
#else
	PMEM_FLUSHER* flusher = gb_pmw->pbuf->flusher;
#endif //UNIV_PMEMOBJ_LSB

	flusher->is_running = true;
	//ulint ret;

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
		os_event_wait(flusher->is_all_finished);

		if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
			break;
		}
		//the workers are idle and the server is running, keep waiting
		os_event_reset(flusher->is_all_finished);
	} //end while thread

	flusher->is_running = false;
	//trigger waiting workers to stop
	os_event_set(flusher->is_req_not_empty);
	//wait for all workers closed
	printf("wait all workers close...\n");
	os_event_wait(flusher->is_all_closed);

	printf("all workers closed\n");
	my_thread_end();

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}
/*Worker thread of flusher.
 * Managed by the coordinator thread
 * number of threads are equal to the number of cleaner threds from config
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(pm_flusher_worker)(
/*==========================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ulint i;

#if defined (UNIV_PMEMOBJ_LSB)
	PMEM_FLUSHER* flusher = gb_pmw->plsb->flusher;
#else
	PMEM_FLUSHER* flusher = gb_pmw->pbuf->flusher;
#endif //UNIV_PMEM_LSB

	PMEM_BUF_BLOCK_LIST* plist = NULL;

	my_thread_init();

	mutex_enter(&flusher->mutex);
	flusher->n_workers++;
	os_event_reset(flusher->is_all_closed);
	mutex_exit(&flusher->mutex);

	while (true) {
		//worker thread wait until there is is_requested signal 
retry:
		os_event_wait(flusher->is_req_not_empty);
#if defined(UNIV_PMEMOBJ_BUF_RECOVERY_DEBUG)
		printf("wakeup worker...\n");	
#endif
		//looking for a full list in wait-list and flush it
		mutex_enter(&flusher->mutex);
		if (flusher->n_requested > 0) {

#if defined (UNIV_PMEMOBJ_LSB)
			//Case B: Implement of LSB
			// find the first non-null pointer and do aio flush for the bucket
			for (i = 0; i < flusher->size; i++) {
				PMEM_LSB_HASH_BUCKET* bucket = flusher->bucket_arr[i];
				if (bucket){
					pm_lsb_flush_bucket(gb_pmw->pop, gb_pmw->plsb, bucket);
					flusher->n_requested--;
					os_event_set(flusher->is_req_full);
					flusher->bucket_arr[i] = NULL;
#if defined (UNIV_PMEMOBJ_LSB_DEBUG)
			//printf("LSB [2] pm_flusher_worker flusher->size %zu bucket pointer index %zu\n", flusher->size, i);
#endif
					break;
				}
			}

#else //UNIV_PMEMOBJ_BUF
			//Case A: Implement of PB-NVM
			for (i = 0; i < flusher->size; i++) {
				plist = flusher->flush_list_arr[i];
				if (plist)
				{
					//***this call aio_batch ***
#if defined(UNIV_PMEMOBJ_BUF_RECOVERY_DEBUG)
					printf("\n [2] BEGIN (in flusher thread), pointer id=%zu, list_id =%zu\n", i, plist->list_id);
#endif
					pm_buf_flush_list(gb_pmw->pop, gb_pmw->pbuf, plist);
#if defined(UNIV_PMEMOBJ_BUF_RECOVERY_DEBUG)
					printf("\n [2] END (in flusher thread), pointer id=%zu, list_id =%zu\n", i, plist->list_id);
#endif
					flusher->n_requested--;
					os_event_set(flusher->is_req_full);
					//we can set the pointer to null after the pm_buf_flush_list finished
					flusher->flush_list_arr[i] = NULL;
					break;
				}
			}
#endif //UNIV_PMEMOBJ_LSB

		} //end if flusher->n_requested > 0

		if (flusher->n_requested == 0) {
			if (buf_page_cleaner_is_active) {
				//buf_page_cleaner is running, start waiting
				os_event_reset(flusher->is_req_not_empty);
			}
			else {
				mutex_exit(&flusher->mutex);
				break;
			}
		}
		mutex_exit(&flusher->mutex);
	} //end while thread

	mutex_enter(&flusher->mutex);
	flusher->n_workers--;
	//printf("close a worker, current open workers %zu, n_requested/size = %zu/%zu/%zu is_running = %d\n", flusher->n_workers, flusher->n_requested, flusher->size, flusher->is_running);
	if (flusher->n_workers == 0) {
		printf("The last worker is closing\n");
		//os_event_set(flusher->is_all_closed);
	}
	mutex_exit(&flusher->mutex);

	my_thread_end();

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}


/* VERSION 3
 *This function is called from aio complete (fil_aio_wait) 
 (1) Reset the list
 (2) Flush spaces in this list
 * */
void
pm_handle_finished_block_with_flusher(
		PMEMobjpool*		pop,
		PMEM_WRAPPER*       pmw ,
	   	PMEM_BUF*			buf,
	   	PMEM_BUF_BLOCK*		pblock)
{
#if defined (UNIV_PMEM_SIM_LATENCY)
	uint64_t start_cycle, end_cycle;
#endif	
	PMEM_FLUSHER* flusher;
	ulint i;

	//bool is_lock_prev_list = false;

	//(1) handle the flush_list
	TOID(PMEM_BUF_BLOCK_LIST) flush_list;

	TOID_ASSIGN(flush_list, pblock->list.oid);
	PMEM_BUF_BLOCK_LIST* pflush_list = D_RW(flush_list);

	PMEM_BUF_BLOCK_LIST* pnext_list;
	PMEM_BUF_BLOCK_LIST* pprev_list;


	assert(pflush_list);
		
	pmemobj_rwlock_wrlock(pop, &pflush_list->lock);
	
	if(pblock->sync)
		pflush_list->n_sio_pending--;
	else
		pflush_list->n_aio_pending--;

	if (pflush_list->n_aio_pending + pflush_list->n_sio_pending == 0) {
#if defined (UNIV_PMEMOBJ_BUF_RECOVERY_DEBUG)
		printf("\n [*****[4]  BEGIN finish AIO list %zu hashed_id %zu\n",
		   	pflush_list->list_id, pflush_list->hashed_id);
#endif
		//Now all pages in this list are persistent in disk
		//
#if defined (UNIV_PMEMOBJ_BLOOM)
		//Remove the page's tag from bloom filter
		for (i = 0; i < pflush_list->max_pages; i++){
			uint64_t key = D_RW(D_RW(pflush_list->arr)[i])->id.fold();
			pm_cbf_remove(buf->cbf, key);
		}
#endif //UNIV_PMEMOBJ_BLOOM

		//(0) flush spaces
		pm_buf_flush_spaces_in_list(pop, buf, pflush_list);
		// Reset the param_array
		ulint arr_idx;
		arr_idx = pflush_list->param_arr_index;
		assert(arr_idx >= 0);

		pmemobj_rwlock_wrlock(pop, &buf->param_lock);
		buf->param_arrs[arr_idx].is_free = true;
		pmemobj_rwlock_unlock(pop, &buf->param_lock);

		//(1) Reset blocks in the list
		for (i = 0; i < pflush_list->max_pages; i++) {
			PMEM_BUF_BLOCK*		it = D_RW(D_RW(pflush_list->arr)[i]);

			it->state = PMEM_FREE_BLOCK;
			it->sync = false;

#if defined (UNIV_PMEMOBJ_PERSIST)
			pmemobj_persist(pop, &it->state, sizeof(it->state));
			pmemobj_persist(pop, &it->sync, sizeof(it->sync));
#endif	
			//D_RW(D_RW(pflush_list->arr)[i])->state = PMEM_FREE_BLOCK;
			//D_RW(D_RW(pflush_list->arr)[i])->sync = false;
#if defined (UNIV_PMEM_SIM_LATENCY)
			PMEM_DELAY(start_cycle, end_cycle, 2 * pmw->PMEM_SIM_CPU_CYCLES);
#endif
		}

		pflush_list->cur_pages = 0;
		pflush_list->is_flush = false;
		pflush_list->hashed_id = PMEM_ID_NONE;

#if defined (UNIV_PMEMOBJ_PERSIST)
		pmemobj_persist(pop, &pflush_list->cur_pages, sizeof(pflush_list->cur_pages));
		pmemobj_persist(pop, &pflush_list->is_flush, sizeof(pflush_list->is_flush));
		pmemobj_persist(pop, &pflush_list->hashed_id, sizeof(pflush_list->hashed_id));
#endif

#if defined (UNIV_PMEM_SIM_LATENCY)
		PMEM_DELAY(start_cycle, end_cycle, 3 * pmw->PMEM_SIM_CPU_CYCLES);
#endif
		
		// (2) Remove this list from the doubled-linked list
		//assert( !TOID_IS_NULL(pflush_list->prev_list) );
		
		pnext_list = D_RW(pflush_list->next_list);
		pprev_list = D_RW(pflush_list->prev_list);

		if (pprev_list != NULL &&
			D_RW(pprev_list->next_list) != NULL &&
			D_RW(pprev_list->next_list)->list_id == pflush_list->list_id){

			if (pnext_list == NULL) {
				TOID_ASSIGN(pprev_list->next_list, OID_NULL);
			}
			else {
				TOID_ASSIGN(pprev_list->next_list, (pflush_list->next_list).oid);
			}
		}

		if (pnext_list != NULL &&
				D_RW(pnext_list->prev_list) != NULL &&
				D_RW(pnext_list->prev_list)->list_id == pflush_list->list_id) {
			if (pprev_list == NULL) {
				TOID_ASSIGN(pnext_list->prev_list, OID_NULL);
			}
			else {
			#if defined (UNIV_PMEMOBJ_BUF_RECOVERY_DEBUG)
			printf("[4] !!!!! handle finish, cur_list_id %zu ",
					pflush_list->list_id);
			printf ("[4] !!!!  has next_list_id %zu ", pnext_list->list_id);
			printf ("[4] !!!! has prev_list_id %zu \n", pprev_list->list_id);
			#endif
				TOID_ASSIGN(pnext_list->prev_list, (pflush_list->prev_list).oid);
			}
		}

		TOID_ASSIGN(pflush_list->next_list, OID_NULL);

		TOID_ASSIGN(pflush_list->prev_list, OID_NULL);

#if defined (UNIV_PMEM_SIM_LATENCY)
		PMEM_DELAY(start_cycle, end_cycle, 4 * pmw->PMEM_SIM_CPU_CYCLES);
#endif
		// (3) we return this list to the free_pool
		PMEM_BUF_FREE_POOL* pfree_pool;
		pfree_pool = D_RW(buf->free_pool);

		pmemobj_rwlock_wrlock(pop, &pfree_pool->lock);

		POBJ_LIST_INSERT_TAIL(pop, &pfree_pool->head, flush_list, list_entries);
		pfree_pool->cur_lists++;

#if defined (UNIV_PMEMOBJ_PERSIST)
		pmemobj_persist(pop, &pfree_pool->cur_lists, sizeof(pfree_pool->cur_lists));
#endif
#if defined (UNIV_PMEM_SIM_LATENCY)
		PMEM_DELAY(start_cycle, end_cycle, 2 * pmw->PMEM_SIM_CPU_CYCLES);
#endif
		//wakeup who is waitting for free_pool available
		os_event_set(buf->free_pool_event);
		
#if defined (UNIV_PMEMOBJ_BUF_RECOVERY_DEBUG)
		printf("\n *****[4] END finish AIO List %zu]\n", pflush_list->list_id);
#endif
		pmemobj_rwlock_unlock(pop, &pfree_pool->lock);
	}
	//the list has some unfinished aio	
	pmemobj_rwlock_unlock(pop, &pflush_list->lock);
}

#if defined (UNIV_PMEMOBJ_LSB)
/*
 *Handle finish block in the aio
 Note that this function may has contention between flush threads
 * */
void
pm_lsb_handle_finished_block(
		PMEMobjpool*		pop,
	   	PMEM_LSB*			lsb,
	   	PMEM_BUF_BLOCK*		pblock)
{
	PMEM_FLUSHER* flusher;
	ulint i;

	//(1) handle the lsb_list
	PMEM_BUF_BLOCK_LIST* plsb_list = D_RW(lsb->lsb_list);

	//Unlike PB-NVM, LSB implement lock the lsb list until all pages finish propagation, so we don't need to lock the list
	//pmemobj_rwlock_wrlock(pop, &pflush_list->lock);
	pmemobj_rwlock_wrlock(pop, &lsb->lsb_aio_lock);
	++lsb->n_aio_completed;
	pmemobj_rwlock_unlock(pop, &lsb->lsb_aio_lock);
	
	if (lsb->n_aio_completed == plsb_list->cur_pages)
	//if (lsb->n_aio_completed == lsb->n_aio_submitted)
	{
#if defined (UNIV_PMEMOBJ_LSB_DEBUG)
		printf("LSB [5] pm_lsb_handle_finished_block ALL FINISHED lsb->n_aio_completed/n_aio_submitted  %zu/%zu cur_pages %zu max_pages %zu \n", lsb->n_aio_completed, lsb->n_aio_submitted, plsb_list->cur_pages, plsb_list->max_pages);
#endif
		//(0) flush spaces
		pm_lsb_flush_spaces_in_list(pop, lsb, plsb_list);
		//
		// Reset the param_array
		ulint arr_idx;
		arr_idx = plsb_list->param_arr_index;
		assert(arr_idx >= 0);

		for (i = 0; i < lsb->param_arr_size; ++i){
			lsb->param_arrs[i].is_free = true;
		}
		lsb->cur_free_param = 0;

		//(1) Reset blocks in the list
		for (i = 0; i < plsb_list->max_pages; i++) {
			D_RW(D_RW(plsb_list->arr)[i])->state = PMEM_FREE_BLOCK;
			D_RW(D_RW(plsb_list->arr)[i])->sync = false;
		}
		plsb_list->cur_pages = 0;
		plsb_list->is_flush = false;

		//(2) Reset the hashtable
		pm_lsb_hashtable_reset(pop, lsb);
		lsb->n_aio_submitted = lsb->n_aio_completed = 0;

		// (3) Reset the flusher
		flusher = lsb->flusher;
		mutex_enter(&flusher->mutex);
		for (i = 0; i < flusher->size; ++i) {
			flusher->bucket_arr[i] = NULL;
		}
		flusher->n_requested = 0;
		mutex_exit(&flusher->mutex);

		//(4) wakeup the write thread
		os_event_set(lsb->all_aio_finished);
	}
}
#endif //UNIV_PMEMOBJ_LSB

#endif // UNIV_PMEMOBJ_BUF_FLUSHER

/////////////////////////////////////////////////
//Those functions and related structures are declared in my_pmemobj.h
//
static PMEM_LIST_CLEANER* list_cleaner = NULL;
static bool pm_buf_list_cleaner_is_active = false;
/** Event to synchronise with the flushing. */
os_event_t	pm_buf_flush_event;

#ifdef UNIV_DEBUG
static my_bool pm_list_cleaner_disabled_debug;
#endif /* UNIV_DEBUG */

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t pm_list_cleaner_thread_key;
mysql_pfs_key_t pm_flusher_thread_key;
#endif /* UNIV_PFS_THREAD */



/******************************************************************//**
list_cleaner thread tasked with flushing dirty pages from the PMEM_BUF_BLOCK_LIST
pools. As of now we'll have only one coordinator.
@return a dummy parameter 
Currently, this function is used for tracing only
*/
extern "C"
os_thread_ret_t
DECLARE_THREAD(pm_buf_flush_list_cleaner_coordinator)(
/*===============================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	my_thread_init();

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(pm_list_cleaner_thread_key);
#endif /* UNIV_PFS_THREAD */


	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
		//print out each 10s 
		os_thread_sleep(10000000);
		if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
			break;
		}
#if defined (UNIV_PMEMOBJ_LSB)
		printf("cur lsb_list cur pages/max_pages = %zu/%zu\n", D_RW(gb_pmw->plsb->lsb_list)->cur_pages, D_RW(gb_pmw->plsb->lsb_list)->max_pages);
#elif defined (UNIV_PMEMOBJ_BLOOM)
		printf("cur free list = %zu, cur spec_list = %zu \n",
			   	D_RW(gb_pmw->pbuf->free_pool)->cur_lists,
				D_RW(gb_pmw->pbuf->spec_list)->cur_pages);

		//pm_bloom_stats(gb_pmw->pbuf->bf);
		
		/*print the BloomFilter stat info*/
		//pm_cbf_stats(gb_pmw->pbuf->cbf);
#else
		printf("cur free list = %zu, cur spec_list = %zu\n",
			   	D_RW(gb_pmw->pbuf->free_pool)->cur_lists,
				D_RW(gb_pmw->pbuf->spec_list)->cur_pages);
#endif
	} //end while thread

	printf("pm_buf_flush_list_cleaner_coordinator thread  end\n");
	my_thread_end();

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}
#endif //UNIV_PMEMOBJ_BUF


/////////////////////// END PART LOG IMPLEMENT/////////////
