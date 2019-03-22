/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.

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
@file mtr/mtr0mtr.cc
Mini-transaction buffer

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#include "mtr0mtr.h"

#include "buf0buf.h"
#include "buf0flu.h"
#include "fsp0sysspace.h"
#include "page0types.h"
#include "mtr0log.h"
#include "log0log.h"
#include "row0trunc.h"

#include "log0recv.h"

#ifdef UNIV_NONINL
#include "mtr0mtr.ic"
#endif /* UNIV_NONINL */

#if defined (UNIV_TRACE_FLUSH_TIME)
extern volatile int64 gb_write_log_time;
extern volatile int64 gb_n_write_log;
#endif

#if defined (UNIV_PMEMOBJ_PART_PL) || defined (UNIV_PMEMOBJ_WAL_ELR)
#include "my_pmemobj.h"
extern PMEM_WRAPPER* gb_pmw; 
#endif /* UNIV_PMEMOBJ_PART_PL */
/** Iterate over a memo block in reverse. */
template <typename Functor>
struct Iterate {

	/** Release specific object */
	explicit Iterate(Functor& functor)
		:
		m_functor(functor)
	{
		/* Do nothing */
	}

	/** @return false if the functor returns false. */
	bool operator()(mtr_buf_t::block_t* block)
	{
		const mtr_memo_slot_t*	start =
			reinterpret_cast<const mtr_memo_slot_t*>(
				block->begin());

		mtr_memo_slot_t*	slot =
			reinterpret_cast<mtr_memo_slot_t*>(
				block->end());

		ut_ad(!(block->used() % sizeof(*slot)));

		while (slot-- != start) {

			if (!m_functor(slot)) {
				return(false);
			}
		}

		return(true);
	}

	Functor&	m_functor;
};

/** Find specific object */
struct Find {

	/** Constructor */
	Find(const void* object, ulint type)
		:
		m_slot(),
		m_type(type),
		m_object(object)
	{
		ut_a(object != NULL);
	}

	/** @return false if the object was found. */
	bool operator()(mtr_memo_slot_t* slot)
	{
		if (m_object == slot->object && m_type == slot->type) {
			m_slot = slot;
			return(false);
		}

		return(true);
	}

	/** Slot if found */
	mtr_memo_slot_t*m_slot;

	/** Type of the object to look for */
	ulint		m_type;

	/** The object instance to look for */
	const void*	m_object;
};

/** Find a page frame */
struct FindPage
{
	/** Constructor
	@param[in]	ptr	pointer to within a page frame
	@param[in]	flags	MTR_MEMO flags to look for */
	FindPage(const void* ptr, ulint flags)
		: m_ptr(ptr), m_flags(flags), m_slot(NULL)
	{
		/* We can only look for page-related flags. */
		ut_ad(!(flags & ~(MTR_MEMO_PAGE_S_FIX
				  | MTR_MEMO_PAGE_X_FIX
				  | MTR_MEMO_PAGE_SX_FIX
				  | MTR_MEMO_BUF_FIX
				  | MTR_MEMO_MODIFY)));
	}

	/** Visit a memo entry.
	@param[in]	slot	memo entry to visit
	@retval	false	if a page was found
	@retval	true	if the iteration should continue */
	bool operator()(mtr_memo_slot_t* slot)
	{
		ut_ad(m_slot == NULL);

		if (!(m_flags & slot->type) || slot->object == NULL) {
			return(true);
		}

		buf_block_t* block = reinterpret_cast<buf_block_t*>(
			slot->object);

		if (m_ptr < block->frame
		    || m_ptr >= block->frame + block->page.size.logical()) {
			return(true);
		}

		m_slot = slot;
		return(false);
	}

	/** @return the slot that was found */
	mtr_memo_slot_t* get_slot() const
	{
		ut_ad(m_slot != NULL);
		return(m_slot);
	}
	/** @return the block that was found */
	buf_block_t* get_block() const
	{
		return(reinterpret_cast<buf_block_t*>(get_slot()->object));
	}
private:
	/** Pointer inside a page frame to look for */
	const void*const	m_ptr;
	/** MTR_MEMO flags to look for */
	const ulint		m_flags;
	/** The slot corresponding to m_ptr */
	mtr_memo_slot_t*	m_slot;
};

/** Release latches and decrement the buffer fix count.
@param slot	memo slot */
static
void
memo_slot_release(mtr_memo_slot_t* slot)
{
	switch (slot->type) {
	case MTR_MEMO_BUF_FIX:
	case MTR_MEMO_PAGE_S_FIX:
	case MTR_MEMO_PAGE_SX_FIX:
	case MTR_MEMO_PAGE_X_FIX: {

		buf_block_t*	block;

		block = reinterpret_cast<buf_block_t*>(slot->object);

		buf_block_unfix(block);
		buf_page_release_latch(block, slot->type);
		break;
	}

	case MTR_MEMO_S_LOCK:
		rw_lock_s_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		break;

	case MTR_MEMO_SX_LOCK:
		rw_lock_sx_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		break;

	case MTR_MEMO_X_LOCK:
		rw_lock_x_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		break;

#ifdef UNIV_DEBUG
	default:
		ut_ad(slot->type == MTR_MEMO_MODIFY);
#endif /* UNIV_DEBUG */
	}

	slot->object = NULL;
}

/** Unfix a page, do not release the latches on the page.
@param slot	memo slot */
static
void
memo_block_unfix(mtr_memo_slot_t* slot)
{
	switch (slot->type) {
	case MTR_MEMO_BUF_FIX:
	case MTR_MEMO_PAGE_S_FIX:
	case MTR_MEMO_PAGE_X_FIX:
	case MTR_MEMO_PAGE_SX_FIX: {
		buf_block_unfix(reinterpret_cast<buf_block_t*>(slot->object));
		break;
	}

	case MTR_MEMO_S_LOCK:
	case MTR_MEMO_X_LOCK:
	case MTR_MEMO_SX_LOCK:
		break;
#ifdef UNIV_DEBUG
	default:
#endif /* UNIV_DEBUG */
		break;
	}
}
/** Release latches represented by a slot.
@param slot	memo slot */
static
void
memo_latch_release(mtr_memo_slot_t* slot)
{
	switch (slot->type) {
	case MTR_MEMO_BUF_FIX:
	case MTR_MEMO_PAGE_S_FIX:
	case MTR_MEMO_PAGE_SX_FIX:
	case MTR_MEMO_PAGE_X_FIX: {
		buf_block_t*	block;

		block = reinterpret_cast<buf_block_t*>(slot->object);

		memo_block_unfix(slot);

		buf_page_release_latch(block, slot->type);

		slot->object = NULL;
		break;
	}

	case MTR_MEMO_S_LOCK:
		rw_lock_s_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		slot->object = NULL;
		break;

	case MTR_MEMO_X_LOCK:
		rw_lock_x_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		slot->object = NULL;
		break;

	case MTR_MEMO_SX_LOCK:
		rw_lock_sx_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
		slot->object = NULL;
		break;

#ifdef UNIV_DEBUG
	default:
		ut_ad(slot->type == MTR_MEMO_MODIFY);

		slot->object = NULL;
#endif /* UNIV_DEBUG */
	}
}

/** Release the latches acquired by the mini-transaction. */
struct ReleaseLatches {

	/** @return true always. */
	bool operator()(mtr_memo_slot_t* slot) const
	{
		if (slot->object != NULL) {
			memo_latch_release(slot);
		}

		return(true);
	}
};

/** Release the latches and blocks acquired by the mini-transaction. */
struct ReleaseAll {
	/** @return true always. */
	bool operator()(mtr_memo_slot_t* slot) const
	{
		if (slot->object != NULL) {
			memo_slot_release(slot);
		}

		return(true);
	}
};

/** Check that all slots have been handled. */
struct DebugCheck {
	/** @return true always. */
	bool operator()(const mtr_memo_slot_t* slot) const
	{
		ut_a(slot->object == NULL);
		return(true);
	}
};

/** Release a resource acquired by the mini-transaction. */
struct ReleaseBlocks {
	/** Release specific object */
	ReleaseBlocks(lsn_t start_lsn, lsn_t end_lsn, FlushObserver* observer)
		:
		m_end_lsn(end_lsn),
		m_start_lsn(start_lsn),
		m_flush_observer(observer)
	{
		/* Do nothing */
	}

	/** Add the modified page to the buffer flush list. */
	void add_dirty_page_to_flush_list(mtr_memo_slot_t* slot) const
	{
		ut_ad(m_end_lsn > 0);
		ut_ad(m_start_lsn > 0);

		buf_block_t*	block;

		block = reinterpret_cast<buf_block_t*>(slot->object);

#if defined (UNIV_PMEMOBJ_PL) || defined (UNIV_SKIPLOG)
		//simulate buf_flush_note_modification()
		mutex_enter(&block->mutex);
		block->page.newest_modification = m_end_lsn;
		/* Don't allow to set flush observer from non-null to null,
		   or from one observer to another. */
		ut_ad(block->page.flush_observer == NULL
				|| block->page.flush_observer == m_flush_observer);
		block->page.flush_observer = m_flush_observer;

		if (block->page.oldest_modification == 0) {

			buf_pool_t*	buf_pool = buf_pool_from_block(block);

			//simulate buf_flush_insert_into_flush_list()
			lsn_t lsn = m_start_lsn;
			buf_flush_list_mutex_enter(buf_pool);

			/* If we are in the recovery then we need to update the flush
			   red-black tree as well. */
			if (buf_pool->flush_rbt != NULL) {
				buf_flush_list_mutex_exit(buf_pool);
				buf_flush_insert_sorted_into_flush_list(buf_pool, block, lsn);
			}
			else {
				ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
				ut_ad(!block->page.in_flush_list);

				ut_d(block->page.in_flush_list = TRUE);
				block->page.oldest_modification = m_start_lsn;

				UT_LIST_ADD_FIRST(buf_pool->flush_list, &block->page);
				//simulate incr_flush_list_size_in_bytes
				buf_pool->stat.flush_list_bytes += block->page.size.physical();

				buf_flush_list_mutex_exit(buf_pool);
			}
			//end simulate buf_flush_insert_into_flush_list()
		} else {
			ut_ad(block->page.oldest_modification <= m_start_lsn);
		}

		buf_page_mutex_exit(block);

		srv_stats.buf_pool_write_requests.inc();
		//END simulate buf_flush_note_modification()

#else // original
		buf_flush_note_modification(block, m_start_lsn,
					    m_end_lsn, m_flush_observer);
#endif //UNIV_PMEMOBJ_PL || UNIV_SKIPLOG 
	}

	/** @return true always. */
	bool operator()(mtr_memo_slot_t* slot) const
	{
		if (slot->object != NULL) {

			if (slot->type == MTR_MEMO_PAGE_X_FIX
			    || slot->type == MTR_MEMO_PAGE_SX_FIX) {

				add_dirty_page_to_flush_list(slot);

			} else if (slot->type == MTR_MEMO_BUF_FIX) {

				buf_block_t*	block;
				block = reinterpret_cast<buf_block_t*>(
					slot->object);
				if (block->made_dirty_with_no_latch) {
					add_dirty_page_to_flush_list(slot);
					block->made_dirty_with_no_latch = false;
				}
			}
		}

		return(true);
	}

	/** Mini-transaction REDO start LSN */
	lsn_t		m_end_lsn;

	/** Mini-transaction REDO end LSN */
	lsn_t		m_start_lsn;

	/** Flush observer */
	FlushObserver*	m_flush_observer;
};

class mtr_t::Command {
public:
	/** Constructor.
	Takes ownership of the mtr->m_impl, is responsible for deleting it.
	@param[in,out]	mtr	mini-transaction */
	explicit Command(mtr_t* mtr)
		:
		m_locks_released()
	{
		init(mtr);
	}

	void init(mtr_t* mtr)
	{
		m_impl = &mtr->m_impl;
		m_sync = mtr->m_sync;
	}

	/** Destructor */
	~Command()
	{
		ut_ad(m_impl == 0);
	}

	/** Write the redo log record, add dirty pages to the flush list and
	release the resources. */
	void execute();

	/** Release the blocks used in this mini-transaction. */
	void release_blocks();

	/** Release the latches acquired by the mini-transaction. */
	void release_latches();

	/** Release both the latches and blocks used in the mini-transaction. */
	void release_all();

	/** Release the resources */
	void release_resources();

	/** Append the redo log records to the redo log buffer.
	@param[in]	len	number of bytes to write */
	void finish_write(ulint len);

private:
	/** Prepare to write the mini-transaction log to the redo log buffer.
	@return number of bytes to write in finish_write() */
	ulint prepare_write();

	/** true if it is a sync mini-transaction. */
	bool			m_sync;

	/** The mini-transaction state. */
	mtr_t::Impl*		m_impl;

	/** Set to 1 after the user thread releases the latches. The log
	writer thread must wait for this to be set to 1. */
	volatile ulint		m_locks_released;

	/** Start lsn of the possible log entry for this mtr */
	lsn_t			m_start_lsn;

	/** End lsn of the possible log entry for this mtr */
	lsn_t			m_end_lsn;
};

/** Check if a mini-transaction is dirtying a clean page.
@return true if the mtr is dirtying a clean page. */
bool
mtr_t::is_block_dirtied(const buf_block_t* block)
{
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->page.buf_fix_count > 0);

	/* It is OK to read oldest_modification because no
	other thread can be performing a write of it and it
	is only during write that the value is reset to 0. */
	return(block->page.oldest_modification == 0);
}

/** Write the block contents to the REDO log */
struct mtr_write_log_t {
	/** Append a block to the redo log buffer.
	@return whether the appending should continue */
	bool operator()(const mtr_buf_t::block_t* block) const
	{
		log_write_low(block->begin(), block->used());
		return(true);
	}
};

/** Append records to the system-wide redo log buffer.
@param[in]	log	redo log records */
void
mtr_write_log(
	const mtr_buf_t*	log)
{
	const ulint	len = log->size();
	mtr_write_log_t	write_log;

	DBUG_PRINT("ib_log",
		   (ULINTPF " extra bytes written at " LSN_PF,
		    len, log_sys->lsn));

	log_reserve_and_open(len);
	log->for_each_block(write_log);
	log_close();
}
#if defined (UNIV_PMEMOBJ_PL)
uint64_t 
mtr_t::pmemlog_get_trx_id() {
	trx_t* trx;

	trx = m_impl.m_parent_trx;

	if (trx != NULL)
		return trx->id;
	else
		return 0;
	
}
#endif 
/** Start a mini-transaction.
@param sync		true if it is a synchronous mini-transaction
@param read_only	true if read only mini-transaction */
void
mtr_t::start(bool sync, bool read_only)
{
	UNIV_MEM_INVALID(this, sizeof(*this));

	UNIV_MEM_INVALID(&m_impl, sizeof(m_impl));

	m_sync =  sync;

	m_commit_lsn = 0;

	new(&m_impl.m_log) mtr_buf_t();
	new(&m_impl.m_memo) mtr_buf_t();

	m_impl.m_mtr = this;
	m_impl.m_log_mode = MTR_LOG_ALL;
	m_impl.m_inside_ibuf = false;
	m_impl.m_modifications = false;
	m_impl.m_made_dirty = false;
	m_impl.m_n_log_recs = 0;
	m_impl.m_state = MTR_STATE_ACTIVE;
	ut_d(m_impl.m_user_space_id = TRX_SYS_SPACE);
	m_impl.m_user_space = NULL;
	m_impl.m_undo_space = NULL;
	m_impl.m_sys_space = NULL;
	m_impl.m_flush_observer = NULL;
#if defined (UNIV_PMEMOBJ_PL)
	m_impl.m_parent_trx = NULL;
	m_impl.m_trx_id = 0;
	m_impl.key_arr = (uint64_t*) calloc(512, sizeof(uint64_t));
	m_impl.LSN_arr = (uint64_t*) calloc(512, sizeof(uint64_t));
	m_impl.space_arr = (uint64_t*) calloc(512, sizeof(uint64_t));
	m_impl.page_arr = (uint64_t*) calloc(512, sizeof(uint64_t));
	m_impl.size_arr = (uint64_t*) calloc(512, sizeof(uint64_t));
	m_impl.type_arr = (uint16_t*) calloc(512, sizeof(uint16_t));
	m_impl.off_arr = (uint16_t*) calloc(512, sizeof(uint16_t));
	m_impl.len_off_arr = (uint16_t*) calloc(512, sizeof(uint16_t));
	
	//ulint max_init_size = 8192;	
	ulint max_init_size = 4096;	

	m_impl.buf = (byte*) calloc(max_init_size, sizeof(byte));
	m_impl.cur_off = 0;
	m_impl.max_buf_size = max_init_size;

#endif //UNIV_PMEMOBJ_PL

	ut_d(m_impl.m_magic_n = MTR_MAGIC_N);
}

/** Release the resources */
void
mtr_t::Command::release_resources()
{
	ut_ad(m_impl->m_magic_n == MTR_MAGIC_N);

	/* Currently only used in commit */
	ut_ad(m_impl->m_state == MTR_STATE_COMMITTING);

#ifdef UNIV_DEBUG
	DebugCheck		release;
	Iterate<DebugCheck>	iterator(release);

	m_impl->m_memo.for_each_block_in_reverse(iterator);
#endif /* UNIV_DEBUG */

	/* Reset the mtr buffers */
	m_impl->m_log.erase();

	m_impl->m_memo.erase();
#if defined (UNIV_PMEMOBJ_PL)
	free(m_impl->key_arr);
	free(m_impl->LSN_arr);
	free(m_impl->space_arr);
	free(m_impl->page_arr);
	free(m_impl->size_arr);
	free(m_impl->type_arr);
	free(m_impl->off_arr);
	free(m_impl->len_off_arr);
	free(m_impl->buf);
#endif //UNIV_PMEMOBJ_PL

	m_impl->m_state = MTR_STATE_COMMITTED;

	m_impl = 0;
}

/** Commit a mini-transaction. */
void
mtr_t::commit()
{
	ut_ad(is_active());
	ut_ad(!is_inside_ibuf());
	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	m_impl.m_state = MTR_STATE_COMMITTING;

	/* This is a dirty read, for debugging. */
	ut_ad(!recv_no_log_write);

	Command	cmd(this);

	if (m_impl.m_modifications
	    && (m_impl.m_n_log_recs > 0
		|| m_impl.m_log_mode == MTR_LOG_NO_REDO)) {

		ut_ad(!srv_read_only_mode
		      || m_impl.m_log_mode == MTR_LOG_NO_REDO);

		cmd.execute();
	} else {
		cmd.release_all();
		cmd.release_resources();
	}
}

/** Commit a mini-transaction that did not modify any pages,
but generated some redo log on a higher level, such as
MLOG_FILE_NAME records and a MLOG_CHECKPOINT marker.
The caller must invoke log_mutex_enter() and log_mutex_exit().
This is to be used at log_checkpoint().
@param[in]	checkpoint_lsn		the LSN of the log checkpoint
@param[in]	write_mlog_checkpoint	Write MLOG_CHECKPOINT marker
					if it is enabled. */
void
mtr_t::commit_checkpoint(
	lsn_t	checkpoint_lsn,
	bool	write_mlog_checkpoint)
{
	ut_ad(log_mutex_own());
	ut_ad(is_active());
	ut_ad(!is_inside_ibuf());
	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	ut_ad(get_log_mode() == MTR_LOG_ALL);
	ut_ad(!m_impl.m_made_dirty);
	ut_ad(m_impl.m_memo.size() == 0);
	ut_ad(!srv_read_only_mode);
	ut_d(m_impl.m_state = MTR_STATE_COMMITTING);
	ut_ad(write_mlog_checkpoint || m_impl.m_n_log_recs > 1);

	/* This is a dirty read, for debugging. */
	ut_ad(!recv_no_log_write);

	switch (m_impl.m_n_log_recs) {
	case 0:
		break;
	case 1:
		*m_impl.m_log.front()->begin() |= MLOG_SINGLE_REC_FLAG;
		break;
	default:
#if defined(UNIV_PMEMOBJ_PART_PL)
		mlog_catenate_ulint(
			m_impl.m_mtr, MLOG_MULTI_REC_END, MLOG_1BYTE);
#else
		mlog_catenate_ulint(
			&m_impl.m_log, MLOG_MULTI_REC_END, MLOG_1BYTE);
#endif
	}

	if (write_mlog_checkpoint) {
		byte*	ptr = m_impl.m_log.push<byte*>(SIZE_OF_MLOG_CHECKPOINT);
#if SIZE_OF_MLOG_CHECKPOINT != 9
# error SIZE_OF_MLOG_CHECKPOINT != 9
#endif
		*ptr = MLOG_CHECKPOINT;
		mach_write_to_8(ptr + 1, checkpoint_lsn);
	}

	Command	cmd(this);
	cmd.finish_write(m_impl.m_log.size());
	cmd.release_resources();

	if (write_mlog_checkpoint) {
		DBUG_PRINT("ib_log",
			   ("MLOG_CHECKPOINT(" LSN_PF ") written at " LSN_PF,
			    checkpoint_lsn, log_sys->lsn));
	}
}

#ifdef UNIV_DEBUG
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a MLOG_FILE_NAME record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool
mtr_t::is_named_space(ulint space) const
{
	ut_ad(!m_impl.m_sys_space
	      || m_impl.m_sys_space->id == TRX_SYS_SPACE);
	ut_ad(!m_impl.m_undo_space
	      || m_impl.m_undo_space->id != TRX_SYS_SPACE);
	ut_ad(!m_impl.m_user_space
	      || m_impl.m_user_space->id != TRX_SYS_SPACE);
	ut_ad(!m_impl.m_sys_space
	      || m_impl.m_sys_space != m_impl.m_user_space);
	ut_ad(!m_impl.m_sys_space
	      || m_impl.m_sys_space != m_impl.m_undo_space);
	ut_ad(!m_impl.m_user_space
	      || m_impl.m_user_space != m_impl.m_undo_space);

	switch (get_log_mode()) {
	case MTR_LOG_NONE:
	case MTR_LOG_NO_REDO:
		return(true);
	case MTR_LOG_ALL:
	case MTR_LOG_SHORT_INSERTS:
		return(m_impl.m_user_space_id == space
		       || is_predefined_tablespace(space));
	}

	ut_error;
	return(false);
}
#endif /* UNIV_DEBUG */

/** Acquire a tablespace X-latch.
NOTE: use mtr_x_lock_space().
@param[in]	space_id	tablespace ID
@param[in]	file		file name from where called
@param[in]	line		line number in file
@return the tablespace object (never NULL) */
fil_space_t*
mtr_t::x_lock_space(ulint space_id, const char* file, ulint line)
{
	fil_space_t*	space;

	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	ut_ad(is_active());

	if (space_id == TRX_SYS_SPACE) {
		space = m_impl.m_sys_space;

		if (!space) {
			space = m_impl.m_sys_space = fil_space_get(space_id);
		}
	} else if ((space = m_impl.m_user_space) && space_id == space->id) {
	} else if ((space = m_impl.m_undo_space) && space_id == space->id) {
	} else if (get_log_mode() == MTR_LOG_NO_REDO) {
		space = fil_space_get(space_id);
		ut_ad(space->purpose == FIL_TYPE_TEMPORARY
		      || space->purpose == FIL_TYPE_IMPORT
		      || space->redo_skipped_count > 0
		      || srv_is_tablespace_truncated(space->id));
	} else {
		/* called from trx_rseg_create() */
		space = m_impl.m_undo_space = fil_space_get(space_id);
	}

	ut_ad(space);
	ut_ad(space->id == space_id);
	x_lock(&space->latch, file, line);
	ut_ad(space->purpose == FIL_TYPE_TEMPORARY
	      || space->purpose == FIL_TYPE_IMPORT
	      || space->purpose == FIL_TYPE_TABLESPACE);
	return(space);
}

/** Look up the system tablespace. */
void
mtr_t::lookup_sys_space()
{
	ut_ad(!m_impl.m_sys_space);
	m_impl.m_sys_space = fil_space_get(TRX_SYS_SPACE);
	ut_ad(m_impl.m_sys_space);
}

/** Look up the user tablespace.
@param[in]	space_id	tablespace ID */
void
mtr_t::lookup_user_space(ulint space_id)
{
	ut_ad(space_id != TRX_SYS_SPACE);
	ut_ad(m_impl.m_user_space_id == space_id);
	ut_ad(!m_impl.m_user_space);
	m_impl.m_user_space = fil_space_get(space_id);
	ut_ad(m_impl.m_user_space);
}

/** Set the tablespace associated with the mini-transaction
(needed for generating a MLOG_FILE_NAME record)
@param[in]	space	user or system tablespace */
void
mtr_t::set_named_space(fil_space_t* space)
{
	ut_ad(m_impl.m_user_space_id == TRX_SYS_SPACE);
	ut_d(m_impl.m_user_space_id = space->id);
	if (space->id == TRX_SYS_SPACE) {
		ut_ad(m_impl.m_sys_space == NULL
		      || m_impl.m_sys_space == space);
		m_impl.m_sys_space = space;
	} else {
		m_impl.m_user_space = space;
	}
}

/** Release an object in the memo stack.
@return true if released */
bool
mtr_t::memo_release(const void* object, ulint type)
{
	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	ut_ad(is_active());

	/* We cannot release a page that has been written to in the
	middle of a mini-transaction. */
	ut_ad(!m_impl.m_modifications || type != MTR_MEMO_PAGE_X_FIX);

	Find		find(object, type);
	Iterate<Find>	iterator(find);

	if (!m_impl.m_memo.for_each_block_in_reverse(iterator)) {
		memo_slot_release(find.m_slot);
		return(true);
	}

	return(false);
}

/** Release a page latch.
@param[in]	ptr	pointer to within a page frame
@param[in]	type	object type: MTR_MEMO_PAGE_X_FIX, ... */
void
mtr_t::release_page(const void* ptr, mtr_memo_type_t type)
{
	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	ut_ad(is_active());

	/* We cannot release a page that has been written to in the
	middle of a mini-transaction. */
	ut_ad(!m_impl.m_modifications || type != MTR_MEMO_PAGE_X_FIX);

	FindPage		find(ptr, type);
	Iterate<FindPage>	iterator(find);

	if (!m_impl.m_memo.for_each_block_in_reverse(iterator)) {
		memo_slot_release(find.get_slot());
		return;
	}

	/* The page was not found! */
	ut_ad(0);
}

/** Prepare to write the mini-transaction log to the redo log buffer.
@return number of bytes to write in finish_write() */
ulint
mtr_t::Command::prepare_write()
{
	switch (m_impl->m_log_mode) {
	case MTR_LOG_SHORT_INSERTS:
		ut_ad(0);
		/* fall through (write no redo log) */
	case MTR_LOG_NO_REDO:
	case MTR_LOG_NONE:
		ut_ad(m_impl->m_log.size() == 0);
		log_mutex_enter();
		m_end_lsn = m_start_lsn = log_sys->lsn;
		return(0);
	case MTR_LOG_ALL:
		break;
	}

	ulint	len	= m_impl->m_log.size();
	ulint	n_recs	= m_impl->m_n_log_recs;
	ut_ad(len > 0);
	ut_ad(n_recs > 0);

	if (len > log_sys->buf_size / 2) {
		log_buffer_extend((len + 1) * 2);
	}

	ut_ad(m_impl->m_n_log_recs == n_recs);

	fil_space_t*	space = m_impl->m_user_space;

	if (space != NULL && is_system_or_undo_tablespace(space->id)) {
		/* Omit MLOG_FILE_NAME for predefined tablespaces. */
		space = NULL;
	}

	log_mutex_enter();

	if (fil_names_write_if_was_clean(space, m_impl->m_mtr)) {
		/* This mini-transaction was the first one to modify
		this tablespace since the latest checkpoint, so
		some MLOG_FILE_NAME records were appended to m_log. */
		ut_ad(m_impl->m_n_log_recs > n_recs);
#if defined(UNIV_PMEMOBJ_PART_PL)
		mlog_catenate_ulint(
			m_impl->m_mtr, MLOG_MULTI_REC_END, MLOG_1BYTE);
#else
		mlog_catenate_ulint(
			&m_impl->m_log, MLOG_MULTI_REC_END, MLOG_1BYTE);
#endif
		len = m_impl->m_log.size();
	} else {
		/* This was not the first time of dirtying a
		tablespace since the latest checkpoint. */

		ut_ad(n_recs == m_impl->m_n_log_recs);

		if (n_recs <= 1) {
			ut_ad(n_recs == 1);

			/* Flag the single log record as the
			only record in this mini-transaction. */
			*m_impl->m_log.front()->begin()
				|= MLOG_SINGLE_REC_FLAG;
		} else {
			/* Because this mini-transaction comprises
			multiple log records, append MLOG_MULTI_REC_END
			at the end. */

#if defined(UNIV_PMEMOBJ_PART_PL)
			mlog_catenate_ulint(
				m_impl->m_mtr, MLOG_MULTI_REC_END,
				MLOG_1BYTE);
#else
			mlog_catenate_ulint(
				&m_impl->m_log, MLOG_MULTI_REC_END,
				MLOG_1BYTE);
#endif
			len++;
		}
	}

	/* check and attempt a checkpoint if exceeding capacity */
	log_margin_checkpoint_age(len);

	return(len);
}

/** Append the redo log records to the redo log buffer
@param[in] len	number of bytes to write */
void
mtr_t::Command::finish_write(
	ulint	len)
{
	ut_ad(m_impl->m_log_mode == MTR_LOG_ALL);
	ut_ad(log_mutex_own());
	ut_ad(m_impl->m_log.size() == len);
	ut_ad(len > 0);

	if (m_impl->m_log.is_small()) {
		const mtr_buf_t::block_t*	front = m_impl->m_log.front();
		ut_ad(len <= front->used());

		m_end_lsn = log_reserve_and_write_fast(
			front->begin(), len, &m_start_lsn);

		if (m_end_lsn > 0) {
			return;
		}
	}

	/* Open the database log for log_write_low */
	m_start_lsn = log_reserve_and_open(len);

	mtr_write_log_t	write_log;
	m_impl->m_log.for_each_block(write_log);

	m_end_lsn = log_close();
}

/** Release the latches and blocks acquired by this mini-transaction */
void
mtr_t::Command::release_all()
{
	ReleaseAll release;
	Iterate<ReleaseAll> iterator(release);

	m_impl->m_memo.for_each_block_in_reverse(iterator);

	/* Note that we have released the latches. */
	m_locks_released = 1;
}

/** Release the latches acquired by this mini-transaction */
void
mtr_t::Command::release_latches()
{
	ReleaseLatches release;
	Iterate<ReleaseLatches> iterator(release);

	m_impl->m_memo.for_each_block_in_reverse(iterator);

	/* Note that we have released the latches. */
	m_locks_released = 1;
}

/** Release the blocks used in this mini-transaction */
void
mtr_t::Command::release_blocks()
{
	ReleaseBlocks release(m_start_lsn, m_end_lsn, m_impl->m_flush_observer);
	Iterate<ReleaseBlocks> iterator(release);

	m_impl->m_memo.for_each_block_in_reverse(iterator);
}

/** Write the redo log record, add dirty pages to the flush list and release
the resources. */
#if defined (UNIV_PMEMOBJ_PL) || defined (UNIV_SKIPLOG)
#if defined (UNIV_PMEMOBJ_PART_PL)

void
mtr_t::Command::execute()
{
#if defined (UNIV_TRACE_FLUSH_TIME)
	ulint start_time = ut_time_us(NULL);
#endif

	ulint			i;
	ulint			len;
	fil_space_t*	space;

	mlog_id_t type;
	mlog_id_t check_type;

	byte* begin_ptr;
	byte* ptr;
	byte* temp_ptr;
	byte* end_ptr;

	ulint parsed_len;
	ulint check_len;

	ulint parsed_lsn;
	ulint check_lsn;

	ulint n_parsed;
	ulint space_no, page_no;
	byte* body;

	ulint check_space, check_page;

	uint16_t	n_recs;
	uint16_t	prev_off;
	uint16_t	prev_len_off;
	uint16_t	rec_size;

	trx_t*			trx;
	mtr_t*			mtr;
	
	mtr = m_impl->m_mtr;

	trx = m_impl->m_parent_trx;
	//len	= m_impl->m_log.size();
	len	= mtr->get_cur_off();
	n_recs	= m_impl->m_n_log_recs;

	begin_ptr = mtr->get_buf();

	/////////////////////////////////////////////////
	// begin simulate Command::prepare_write()
	/////////////////////////////////////////////////
	/*simulate the lsn 
	 * start lsn is the smallest lsn in the LSN_arr
	 * end_lsn is the largest lsn in the LSN_arr
	 * */

	switch (m_impl->m_log_mode) {
	case MTR_LOG_SHORT_INSERTS:
		ut_ad(0);
		/* fall through (write no redo log) */
	case MTR_LOG_NO_REDO:
	case MTR_LOG_NONE:
		len =0;
	case MTR_LOG_ALL:
		break;
	}

	if (len == 0){
		//m_end_lsn = m_start_lsn = log_sys->lsn;
		m_end_lsn = m_start_lsn = ut_time_us(NULL);
		goto skip_prepare;
	}
	
	m_start_lsn = m_impl->LSN_arr[0] + 1;
	//m_end_lsn = m_start_lsn + len;
	m_end_lsn = m_impl->LSN_arr[n_recs - 1] + 1;

	ut_ad(m_start_lsn <= m_end_lsn);
	

	if (false)
		goto skip_enclose;

	//(1) Enclose with MLOG_
	space = m_impl->m_user_space;

	if (space != NULL && is_system_or_undo_tablespace(space->id)) {
		/* Omit MLOG_FILE_NAME for predefined tablespaces. */
		space = NULL;
	}

	//simulate fil_names_write_if_was_clean()
	bool was_clean;

	if (space == NULL){
		was_clean = false;
	}
	else{
		was_clean = space->max_lsn == 0;
		space->max_lsn = m_end_lsn;
		if (was_clean) {
			printf("===>|| mtr::exec write MLOG_FILE* for space id %zu\n", space->id);
			//call fil_names_write(space, mtr) that wirte MLOG_FILE_NAME REDO log of the first page to mtr heap
			fil_names_dirty_and_write(space, m_impl->m_mtr);
		}
	}
	//end simulate fil_names_write_if_was_clean()
	
	//we don't appned MLOG_MULTI_REC_END in PPL	
	
//	if (was_clean) {
//		/* This mini-transaction was the first one to modify
//		this tablespace since the latest checkpoint, so
//		some MLOG_FILE_NAME records were appended to m_log. */
//		ut_ad(m_impl->m_n_log_recs > n_recs);
//
//		mlog_catenate_ulint(
//			m_impl->m_mtr, MLOG_MULTI_REC_END, MLOG_1BYTE);
//		//update len
//		len = m_impl->m_log.size();
//	}
//	else {
//		/* This was not the first time of dirtying a
//		tablespace since the latest checkpoint. */
//
//		ut_ad(n_recs == m_impl->m_n_log_recs);
//
//		if (n_recs <= 1) {
//			ut_ad(n_recs == 1);
//
//			/* Flag the single log record as the
//			only record in this mini-transaction. */
//			*m_impl->m_log.front()->begin()
//				|= MLOG_SINGLE_REC_FLAG;
//		} else {
//			/* Because this mini-transaction comprises
//			multiple log records, append MLOG_MULTI_REC_END
//			at the end. */
//
//		mlog_catenate_ulint(
//			m_impl->m_mtr, MLOG_MULTI_REC_END, MLOG_1BYTE);
//			len++;
//		}
//	}

skip_enclose:
	//in enclose, some new log recs may appended, update the n_recs
	n_recs	= m_impl->m_n_log_recs;
	len	= mtr->get_cur_off();
	

	//(2) Compute "rec_len" for the last log rec 
	prev_off = mtr->get_off_at(n_recs - 1);

	rec_size = len - prev_off ;

	assert(rec_size > 0);

	prev_len_off = mtr->get_len_off_at(n_recs - 1); 

	mach_write_to_2(begin_ptr + prev_len_off, rec_size);
	mtr->add_size_at(rec_size, n_recs - 1);
	
	//(3) Check
	n_parsed = 0;
	i = 0;	
	ptr = mtr->get_buf();
	
	end_ptr = mtr->open_buf(0);	
	while (ptr < end_ptr){
		if (*ptr == MLOG_MULTI_REC_END){
			ptr++;
			continue;
		}


		assert(i < n_recs);

		check_type = (mlog_id_t) m_impl->type_arr[i];
		check_space = m_impl->space_arr[i];
		check_page = m_impl->page_arr[i]; 
		check_len = m_impl->size_arr[i];

		temp_ptr = mlog_parse_initial_log_record(ptr, end_ptr, &type, &space_no, &page_no);

		if (check_type != type ||
			check_space != space_no ||
			check_page != page_no ||
			type >= MLOG_BIGGEST_TYPE
			){

			printf("ERROR: parsed type %zu space %zu page %zu are differ to CHECK type %zu space %zu page %zu\n", type, space_no, page_no, check_type, check_space, check_page);
			assert(0);

		}
		//now check rec_len field
		parsed_len = mach_read_from_2(temp_ptr);
		
		if (parsed_len != check_len){
			printf("ERROR: parsed len %zu differ to check len %zu\n ", parsed_len, check_len);
			assert(0);
		}
		temp_ptr += 2;

		parsed_lsn = mach_read_from_8(temp_ptr);
		temp_ptr += 8;
		assert(parsed_lsn == m_impl->LSN_arr[i]);

		//check for MLOG_COMP_LIST_END_COPY_CREATED (type == 45) 
		if (type == 45){
			//read the log_data_len to check whether it != 0
			// parse 2 + 2 + (n * 2) bytes
			dict_index_t*   index = NULL;
			byte* temp_ptr2 = mlog_parse_index(temp_ptr, end_ptr, 1, &index);
			ulint log_data_len = mach_read_from_4(temp_ptr2);
			//if (log_data_len == 0){
			//	printf("mtr::exec ERROR log_data_len is ZERO mtr %zu log_ptr %zu type %zu space %zu page %zu\n", mtr, temp_ptr2, type, space_no, page_no);
			//	assert(log_data_len);
			//}
			//else{
			//	printf("mtr::exec OK log_data_len  %zu mtr %zu log_ptr %zu type %zu space %zu page %zu\n", log_data_len, mtr, temp_ptr2, type, space_no, page_no);

			//}

			temp_ptr2 += 4;

			if (log_data_len + (temp_ptr2 - ptr) != check_len){
				printf("mtr::exec ERROR  mtr %zu log_ptr %zu  type 45 error, the_first %zu + log_data_len %zu differ to check_len %zu\n", mtr, (temp_ptr2 - 4), (temp_ptr2-ptr), log_data_len, check_len);
				assert(0);
			}
			else{
				//printf("mtr::exec OK mtr %zu type %zu space %zu page %zu the first %zu log_data_len %zu \n", mtr, type, space_no, page_no, (temp_ptr2-ptr), log_data_len);
			}
		}
		ptr += parsed_len;
		i++;
	}//end while
	
		
	//(4) Add to PPL log
	if (len > 0){
		if (trx != NULL && type > 8){

			//fix node->trx->id == 0 even though node->trx_id != 0 in row_purge()
			trx->pm_log_block_id = pm_ppl_write(
					gb_pmw->pop,
					gb_pmw->ppl,
					trx->id,
					begin_ptr,
					//&m_impl->m_log,
					len,
					n_recs,
					m_impl->key_arr,
					m_impl->LSN_arr,
					m_impl->size_arr,
					trx->pm_log_block_id);
		}
		else if (trx == NULL){
			assert(type > 0 && type <= 8);
			//all type <= 8 is treat as trx_id 0
			pm_ppl_write(
					gb_pmw->pop,
					gb_pmw->ppl,
					0,
					begin_ptr,
					//&m_impl->m_log,
					len,
					n_recs,
					m_impl->key_arr,
					m_impl->LSN_arr,
					m_impl->size_arr,
					-2);
		}
	}//end if(len > 0)
skip_prepare:
	//(?) add the block to the flush list 
	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_enter();
	}
	m_impl->m_mtr->m_commit_lsn = m_end_lsn;

	//update pageLSN in release_blocks()
	release_blocks();

	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_exit();
	}

	release_latches();

	release_resources();
#if defined (UNIV_TRACE_FLUSH_TIME)
	//trace the time spending for write log rec to log buf
	ulint end_time = ut_time_us(NULL);
	ulint exec_time = end_time - start_time;
	//my_atomic_add64(&gb_write_log_time, exec_time);
	__sync_fetch_and_add(&gb_write_log_time, exec_time);
	__sync_fetch_and_add(&gb_n_write_log, 1);
#endif
}

//void
//mtr_t::Command::execute_old()
//{
//#if defined (UNIV_TRACE_FLUSH_TIME)
//	ulint start_time = ut_time_us(NULL);
//#endif
//	ulint			len;
//	ulint			n_recs;
//	fil_space_t*	space;
//
//	mlog_id_t type;
//	mlog_id_t check_type;
//	byte* ptr;
//	byte* end_ptr;
//	
//	ulint block_len;
//	ulint sum_len;
//
//	ulint block_no;
//
//	ulint parsed_len;
//	ulint check_len;
//	ulint n_parsed;
//	ulint space_no, page_no;
//	byte* body;
//
//	trx_t*			trx;
//	
//	mtr_buf_t::block_t*	front = m_impl->m_log.front();
//	mtr_buf_t::block_t* block = front;
//
//	len	= m_impl->m_log.size();
//	n_recs	= m_impl->m_n_log_recs;
//
//	trx = m_impl->m_parent_trx;
//	
//	if (len == 0){
//		//m_end_lsn = m_start_lsn = log_sys->lsn;
//		m_end_lsn = m_start_lsn = ut_time_us(NULL);
//		goto skip_prepare;
//	}
//
//	ptr = (byte*) block->begin();	
//	type = (mlog_id_t)((ulint)*ptr & ~MLOG_SINGLE_REC_FLAG);
//
//	if (true)
//		goto skip_test;
////tdnguyen test
//
//	n_parsed = 0;	
//	block_no = 0;
//
//	//for each block in dynamic buffer	
//	printf("BEGIN mtr with m_start_lsn %zu n_recs %zu len %zu \n", m_start_lsn, n_recs, len);
//	while (block != NULL){	
//		byte* start_log_ptr = (byte*) block->begin(); 	
//		block_len = block->used();
//
//		assert(block_len > 0);
//
//		ptr = start_log_ptr;
//		end_ptr = ptr + block_len;
//		type = (mlog_id_t)((ulint)*ptr & ~MLOG_SINGLE_REC_FLAG);
//		sum_len = 0;
//		printf("=== \tBEGIN Check block %zu len %zu \n", block_no, block_len);
//
//		//parse log recs in this block
//		while (ptr < end_ptr){
//			if (sum_len == block_len){
//				//done this block
//				break;
//			}
//			if (sum_len > block_len){
//				printf("TEST ERRROR sum_len  %zu > block_len %zu \n", sum_len, block_len);
//				assert(0);
//			}
//
//			//(1) get what we save in mtr data structures
//			check_type = (mlog_id_t) m_impl->type_arr[n_parsed];
//
//			if (n_parsed == n_recs - 1){
//				check_len = len - m_impl->size_arr[n_parsed];
//			}
//			else {
//				check_len = m_impl->size_arr[n_parsed + 1] - m_impl->size_arr[n_parsed];
//			}
//			//(A) Only parse the header
//			//mlog_parse_initial_log_record(ptr, end_ptr, &type, &space_no, &page_no);
//			//parsed_len = check_len;
//
//			//(B) Use our parse
//				parsed_len = pm_ppl_recv_parse_log_rec(
//						gb_pmw->pop, gb_pmw->ppl, NULL,
//						&type, ptr, end_ptr, &space_no, &page_no, false, &body);
//			if (type != check_type)
//			{
//				printf("mtr::exec ERROR check type  %zu differ to parsed type %zu type %zu space %zu page %z \n", check_type, type, space_no, page_no);
//				assert(0);
//			}
//
//			if (parsed_len != check_len){
//				printf("mtr::exec ERROR parsed_len %zu differ to checked_len %zu type %zu space %zu page %zu\n", parsed_len, check_len, type, space_no, page_no);
//				//assert(0);
//			}
//			sum_len += parsed_len;
//			
//			ptr = ptr + parsed_len;
//			n_parsed++;
//		}//end parse
//
//		printf("=== END Check block %zu len %zu \n", block_no, block_len);
//
//		block = m_impl->m_log.next(block); 
//		block_no++;
//	} // end for each block
//	printf("END mtr with m_start_lsn %zu n_recs %zu len %zu \n", m_start_lsn, n_recs, len);
//	
//	//if (trx == NULL){
//	//	if (len > 0){
//	//		//check what we miss from capture log
//	//		if (type > 8){
//	//			printf("mtr::exec trx is NULL while len > 0 n_recs %zu type %zu \n", n_recs, type);
//	//		}
//	//	}
//	//}
//	//else{
//	//	//test trx_id == 0
//	//	if (trx->id == 0 && len > 0){
//	//		printf("mtr::exec trx->id is ZERO while trx_id is %zu len > 0 n_recs %zu type %zu \n", m_impl->m_trx_id,  n_recs, type);
//
//	//	}
//	//}
//	//end tdnguyen test
//skip_test:	
//	ut_ad(m_impl->m_n_log_recs == n_recs);
//
//	/////////////////////////////////////////////////
//	// begin simulate Command::prepare_write()
//	/////////////////////////////////////////////////
//	/*simulate the lsn 
//	 * start lsn is the smallest lsn in the LSN_arr
//	 * end_lsn is the largest lsn in the LSN_arr
//	 * */
//	m_start_lsn = m_impl->LSN_arr[0] + 1;
//	//m_end_lsn = m_start_lsn + len;
//	m_end_lsn = m_impl->LSN_arr[n_recs - 1] + 1;
//
//	ut_ad(m_start_lsn <= m_end_lsn);
//
//	if (true)
//		goto skip_enclose;
//
//	space = m_impl->m_user_space;
//
//	if (space != NULL && is_system_or_undo_tablespace(space->id)) {
//		/* Omit MLOG_FILE_NAME for predefined tablespaces. */
//		space = NULL;
//	}
//
//	//simulate fil_names_write_if_was_clean()
//	bool was_clean;
//
//	if (space == NULL){
//		was_clean = false;
//	}
//	else{
//		was_clean = space->max_lsn == 0;
//		space->max_lsn = m_end_lsn;
//		if (was_clean) {
//			//call fil_names_write(space, mtr) that wirte MLOG_FILE_NAME REDO log of the first page to mtr heap
//			fil_names_dirty_and_write(space, m_impl->m_mtr);
//		}
//	}
//	//end simulate fil_names_write_if_was_clean()
//	if (was_clean) {
//		/* This mini-transaction was the first one to modify
//		this tablespace since the latest checkpoint, so
//		some MLOG_FILE_NAME records were appended to m_log. */
//		ut_ad(m_impl->m_n_log_recs > n_recs);
//#if defined(UNIV_PMEMOBJ_PART_PL)
//		mlog_catenate_ulint(
//			m_impl->m_mtr, MLOG_MULTI_REC_END, MLOG_1BYTE);
//#else
//		mlog_catenate_ulint(
//			&m_impl->m_log, MLOG_MULTI_REC_END, MLOG_1BYTE);
//#endif
//		//update len
//		len = m_impl->m_log.size();
//	}
//	else {
//		/* This was not the first time of dirtying a
//		tablespace since the latest checkpoint. */
//
//		ut_ad(n_recs == m_impl->m_n_log_recs);
//
//		if (n_recs <= 1) {
//			ut_ad(n_recs == 1);
//
//			/* Flag the single log record as the
//			only record in this mini-transaction. */
//			*m_impl->m_log.front()->begin()
//				|= MLOG_SINGLE_REC_FLAG;
//		} else {
//			/* Because this mini-transaction comprises
//			multiple log records, append MLOG_MULTI_REC_END
//			at the end. */
//
//#if defined(UNIV_PMEMOBJ_PART_PL)
//		mlog_catenate_ulint(
//			m_impl->m_mtr, MLOG_MULTI_REC_END, MLOG_1BYTE);
//#else
//			mlog_catenate_ulint(
//				&m_impl->m_log, MLOG_MULTI_REC_END,
//				MLOG_1BYTE);
//#endif
//			len++;
//		}
//	}
//
//	/* check and attempt a checkpoint if exceeding capacity */
//	//log_margin_checkpoint_age(len);
//	// We don't need this because when a log buffer in NVDIMM is full we write it to disk
//
//	// end simulate Command::prepare_write()
//	
//	/////////////////////////////////////////////////
//	// begin simulate Command::finish_write()
//	/////////////////////////////////////////////////
//skip_enclose:	
//	//if (len > 0){
//	//	if (trx != NULL && type > 8){
//
//	//		//fix node->trx->id == 0 even though node->trx_id != 0 in row_purge()
//	//		trx->pm_log_block_id = pm_ppl_write(
//	//				gb_pmw->pop,
//	//				gb_pmw->ppl,
//	//				trx->id,
//	//				//start_log_ptr,
//	//				&m_impl->m_log,
//	//				len,
//	//				n_recs,
//	//				m_impl->key_arr,
//	//				m_impl->LSN_arr,
//	//				m_impl->size_arr,
//	//				trx->pm_log_block_id);
//	//	}
//	//	else if (trx == NULL){
//	//		assert(type > 0 && type <= 8);
//	//		//all type <= 8 is treat as trx_id 0
//	//		pm_ppl_write(
//	//				gb_pmw->pop,
//	//				gb_pmw->ppl,
//	//				0,
//	//				//start_log_ptr,
//	//				&m_impl->m_log,
//	//				len,
//	//				n_recs,
//	//				m_impl->key_arr,
//	//				m_impl->LSN_arr,
//	//				m_impl->size_arr,
//	//				-2);
//	//	}
//	//}//end if(len > 0)
//skip_prepare:
//	//(2) add the block to the flush list 
//	if (m_impl->m_made_dirty) {
//		log_flush_order_mutex_enter();
//	}
//	m_impl->m_mtr->m_commit_lsn = m_end_lsn;
//
//	//update pageLSN in release_blocks()
//	release_blocks();
//
//	if (m_impl->m_made_dirty) {
//		log_flush_order_mutex_exit();
//	}
//
//	release_latches();
//
//	release_resources();
//#if defined (UNIV_TRACE_FLUSH_TIME)
//	//trace the time spending for write log rec to log buf
//	ulint end_time = ut_time_us(NULL);
//	ulint exec_time = end_time - start_time;
//	//my_atomic_add64(&gb_write_log_time, exec_time);
//	__sync_fetch_and_add(&gb_write_log_time, exec_time);
//	__sync_fetch_and_add(&gb_n_write_log, 1);
//#endif
//}
#else //old method

// In PL-NVM, we keep log records in our data structure
// This function just release the resource without writing any logs
// We save the overhead of : (1) log_mutex_enter(), 
// (2) log_flush_order_mutex(), and (3) log memcpy()
	void
mtr_t::Command::execute()
{
	ut_ad(m_impl->m_log_mode != MTR_LOG_NONE);


	/*(1) We make our own start_lsn and end_lsn here	
	 * m_start_lsn is the current time in seconds
	 * m_end_lsn = m_start_lsn + lenght of the log record
	 * release_blocks() -> add_dirty_page_to_flush_list()
	 */
	ulint   len = m_impl->m_log.size();	
	//ulint cur_time = ut_time_ms();
	
	ulint cur_time = ut_time_us(NULL);
	m_start_lsn = cur_time;
	m_end_lsn = m_start_lsn + len;

	//test, (2) add the block to the flush list 
	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_enter();
	}
	m_impl->m_mtr->m_commit_lsn = m_end_lsn;

	release_blocks();

	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_exit();
	}
	//end test
	release_latches();

	release_resources();
}
#endif //UNIV_PMEMOBJ_PART_PL
#elif defined (UNIV_PMEMOBJ_WAL) && defined (UNIV_PMEMOBJ_WAL_ELR)
	//Early lock release
void
mtr_t::Command::execute()
{
	ulint len;
	ulint str_len;
	ulint len_tem;
	ulint data_len;
	ulint n_recs;

	byte* start_cpy;
	ulint len_cpy;

	log_t*	log	= log_sys;
	byte*	log_block;

	fil_space_t*	space;

	const mtr_buf_t::block_t*	front = m_impl->m_log.front();
	byte* start_log_ptr = (byte*) front->begin(); 	
	byte* str = start_log_ptr;

	ut_ad(m_impl->m_log_mode != MTR_LOG_NONE);

	////Simulate prepare_write()
	switch (m_impl->m_log_mode) {
	case MTR_LOG_SHORT_INSERTS:
		ut_ad(0);
		/* fall through (write no redo log) */
	case MTR_LOG_NO_REDO:
	case MTR_LOG_NONE:
		ut_ad(m_impl->m_log.size() == 0);
		log_mutex_enter();
		m_end_lsn = m_start_lsn = log_sys->lsn;
		//return(0);
		len = 0;
		break;
	case MTR_LOG_ALL:
		break;
	}
	
	if (m_impl->m_log_mode == MTR_LOG_ALL) {

		len	= m_impl->m_log.size();
		n_recs	= m_impl->m_n_log_recs;
		ut_ad(len > 0);
		ut_ad(n_recs > 0);

		if (len > log_sys->buf_size / 2) {
			log_buffer_extend((len + 1) * 2);
		}

		ut_ad(m_impl->m_n_log_recs == n_recs);

		space = m_impl->m_user_space;

		if (space != NULL && is_system_or_undo_tablespace(space->id)) {
			/* Omit MLOG_FILE_NAME for predefined tablespaces. */
			space = NULL;
		}

		log_mutex_enter();

		if (fil_names_write_if_was_clean(space, m_impl->m_mtr)) {
			/* This mini-transaction was the first one to modify
			   this tablespace since the latest checkpoint, so
			   some MLOG_FILE_NAME records were appended to m_log. */
			ut_ad(m_impl->m_n_log_recs > n_recs);
#if defined(UNIV_PMEMOBJ_PART_PL)
		mlog_catenate_ulint(
			m_impl->m_mtr, MLOG_MULTI_REC_END, MLOG_1BYTE);
#else
			mlog_catenate_ulint(
					&m_impl->m_log, MLOG_MULTI_REC_END, MLOG_1BYTE);
#endif
			len = m_impl->m_log.size();
		} else {
			/* This was not the first time of dirtying a
			   tablespace since the latest checkpoint. */

			ut_ad(n_recs == m_impl->m_n_log_recs);

			if (n_recs <= 1) {
				ut_ad(n_recs == 1);

				/* Flag the single log record as the
				   only record in this mini-transaction. */
				*m_impl->m_log.front()->begin()
					|= MLOG_SINGLE_REC_FLAG;
			} else {
				/* Because this mini-transaction comprises
				   multiple log records, append MLOG_MULTI_REC_END
				   at the end. */

#if defined(UNIV_PMEMOBJ_PART_PL)
		mlog_catenate_ulint(
			m_impl->m_mtr, MLOG_MULTI_REC_END, MLOG_1BYTE);
#else
				mlog_catenate_ulint(
						&m_impl->m_log, MLOG_MULTI_REC_END,
						MLOG_1BYTE);
#endif
				len++;
			}
		}

		/* check and attempt a checkpoint if exceeding capacity */
		log_margin_checkpoint_age(len);
	} // end if (m_impl->m_log_mode == MTR_LOG_ALL)

	////End simulate prepare_write()
	
	if (len > 0){
		//simulate finish_write()	
		ut_ad(m_impl->m_log_mode == MTR_LOG_ALL);
		ut_ad(log_mutex_own());
		ut_ad(m_impl->m_log.size() == len);
		ut_ad(len > 0);

		if (m_impl->m_log.is_small()) {
			const mtr_buf_t::block_t*	front = m_impl->m_log.front();
			ut_ad(len <= front->used());

			m_end_lsn = log_reserve_and_write_fast(
					front->begin(), len, &m_start_lsn);

			if (m_end_lsn > 0) {
				goto skip_write;
			}
		}
		/* Open the database log for log_write_low
		 * This function also check for flush log buffer if lsn + len > log buffer capacity
		 * 
		 * */
		m_start_lsn = log_reserve_and_open(len);

		//simulate log_write_low(), we do the same as log_write_low() except skip memcpy()

		//assign as parameter pass
		str_len = len;	
		str = start_log_ptr;

part_loop:
		data_len = (log->buf_free % OS_FILE_LOG_BLOCK_SIZE) + str_len;
		if (data_len <= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {

			/* The string fits within the current log block */

			len_tem = str_len;
		} else {
			data_len = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE;

			len_tem = OS_FILE_LOG_BLOCK_SIZE
				- (log->buf_free % OS_FILE_LOG_BLOCK_SIZE)
				- LOG_BLOCK_TRL_SIZE;
		}

		//save the pointer and the len to be copied later
		start_cpy = log->buf + log->buf_free;
		len_cpy = len_tem;

		str_len -= len_tem;
		str = str + len_tem;

		log_block = static_cast<byte*>(
				ut_align_down(
					log->buf + log->buf_free, OS_FILE_LOG_BLOCK_SIZE));

		log_block_set_data_len(log_block, data_len);

		if (data_len == OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
			/* This block became full */
			log_block_set_data_len(log_block, OS_FILE_LOG_BLOCK_SIZE);
			log_block_set_checkpoint_no(log_block,
					log_sys->next_checkpoint_no);
			len_tem += LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE;

			log->lsn += len_tem;

			/* Initialize the next block header */
			log_block_init(log_block + OS_FILE_LOG_BLOCK_SIZE, log->lsn);
		} else {
			log->lsn += len_tem;
		}

		log->buf_free += len_tem;

		ut_ad(log->buf_free <= log->buf_size);

		if (str_len > 0) {
			goto part_loop;
		}
#if defined(UNIV_PMEMOBJ_LOG) || defined(UNIV_PMEMOBJ_WAL)
		// update the lsn and buf_free
		gb_pmw->plogbuf->lsn = log->lsn;
		gb_pmw->plogbuf->buf_free = log->buf_free;	
#endif /*UNIV_PMEMOBJ_LOG */
		srv_stats.log_write_requests.inc();

		//end simulate log_write_low() ///

		m_end_lsn = log_close();

	} //end if len > 0
skip_write:
		//////  end simulate finish_write() ////////////

	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_enter();
	}

	/* It is now safe to release the log mutex because the
	flush_order mutex will ensure that we are the first one
	to insert into the flush list. */
	log_mutex_exit();

	//now we do the memcpy
	TX_BEGIN(gb_pmw->pop) {
		TX_MEMCPY(start_cpy, start_log_ptr, len_cpy);
	} TX_ONABORT {
	} TX_END
	gb_pmw->plogbuf->need_recv = true;

	m_impl->m_mtr->m_commit_lsn = m_end_lsn;

	release_blocks();

	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_exit();
	}

	release_latches();

	release_resources();
}
#else //original
void
mtr_t::Command::execute()
{
#if defined (UNIV_TRACE_FLUSH_TIME)
	ulint start_time = ut_time_us(NULL);
#endif
	ut_ad(m_impl->m_log_mode != MTR_LOG_NONE);

	if (const ulint len = prepare_write()) {
		finish_write(len);
	}

	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_enter();
	}

	/* It is now safe to release the log mutex because the
	flush_order mutex will ensure that we are the first one
	to insert into the flush list. */
	log_mutex_exit();

	m_impl->m_mtr->m_commit_lsn = m_end_lsn;

	release_blocks();

	if (m_impl->m_made_dirty) {
		log_flush_order_mutex_exit();
	}

	release_latches();

	release_resources();
#if defined (UNIV_TRACE_FLUSH_TIME)
	//trace the time spending for write log rec to log buf
	ulint end_time = ut_time_us(NULL);
	ulint exec_time = end_time - start_time;
	//my_atomic_add64(&gb_write_log_time, exec_time);
	__sync_fetch_and_add(&gb_write_log_time, exec_time);
	__sync_fetch_and_add(&gb_n_write_log, 1);
#endif
}
#endif // UNIV_PMEMOBJ_PL

/** Release the free extents that was reserved using
fsp_reserve_free_extents().  This is equivalent to calling
fil_space_release_free_extents().  This is intended for use
with index pages.
@param[in]	n_reserved	number of reserved extents */
void
mtr_t::release_free_extents(ulint n_reserved)
{
	fil_space_t*	space;

	ut_ad(m_impl.m_undo_space == NULL);

	if (m_impl.m_user_space != NULL) {

		ut_ad(m_impl.m_user_space->id
		      == m_impl.m_user_space_id);
		ut_ad(memo_contains(get_memo(), &m_impl.m_user_space->latch,
				    MTR_MEMO_X_LOCK));

		space = m_impl.m_user_space;
	} else {

		ut_ad(m_impl.m_sys_space->id == TRX_SYS_SPACE);
		ut_ad(memo_contains(get_memo(), &m_impl.m_sys_space->latch,
				    MTR_MEMO_X_LOCK));

		space = m_impl.m_sys_space;
	}

	space->release_free_extents(n_reserved);
}

#ifdef UNIV_DEBUG
/** Check if memo contains the given item.
@return	true if contains */
bool
mtr_t::memo_contains(
	mtr_buf_t*	memo,
	const void*	object,
	ulint		type)
{
	Find		find(object, type);
	Iterate<Find>	iterator(find);

	return(!memo->for_each_block_in_reverse(iterator));
}

/** Debug check for flags */
struct FlaggedCheck {
	FlaggedCheck(const void* ptr, ulint flags)
		:
		m_ptr(ptr),
		m_flags(flags)
	{
		// Do nothing
	}

	bool operator()(const mtr_memo_slot_t* slot) const
	{
		if (m_ptr == slot->object && (m_flags & slot->type)) {
			return(false);
		}

		return(true);
	}

	const void*	m_ptr;
	ulint		m_flags;
};

/** Check if memo contains the given item.
@param object		object to search
@param flags		specify types of object (can be ORred) of
			MTR_MEMO_PAGE_S_FIX ... values
@return true if contains */
bool
mtr_t::memo_contains_flagged(const void* ptr, ulint flags) const
{
	ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
	ut_ad(is_committing() || is_active());

	FlaggedCheck		check(ptr, flags);
	Iterate<FlaggedCheck>	iterator(check);

	return(!m_impl.m_memo.for_each_block_in_reverse(iterator));
}

/** Check if memo contains the given page.
@param[in]	ptr	pointer to within buffer frame
@param[in]	flags	specify types of object with OR of
			MTR_MEMO_PAGE_S_FIX... values
@return	the block
@retval	NULL	if not found */
buf_block_t*
mtr_t::memo_contains_page_flagged(
	const byte*	ptr,
	ulint		flags) const
{
	FindPage		check(ptr, flags);
	Iterate<FindPage>	iterator(check);

	return(m_impl.m_memo.for_each_block_in_reverse(iterator)
	       ? NULL : check.get_block());
}

/** Mark the given latched page as modified.
@param[in]	ptr	pointer to within buffer frame */
void
mtr_t::memo_modify_page(const byte* ptr)
{
	buf_block_t*	block = memo_contains_page_flagged(
		ptr, MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX);
	ut_ad(block != NULL);

	if (!memo_contains(get_memo(), block, MTR_MEMO_MODIFY)) {
		memo_push(block, MTR_MEMO_MODIFY);
	}
}

/** Print info of an mtr handle. */
void
mtr_t::print() const
{
	ib::info() << "Mini-transaction handle: memo size "
		<< m_impl.m_memo.size() << " bytes log size "
		<< get_log()->size() << " bytes";
}

#endif /* UNIV_DEBUG */
