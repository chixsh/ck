/*
 * Copyright 2011-2013 Samy Al Bahra.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _CK_RWLOCK_H
#define _CK_RWLOCK_H

#include <ck_pr.h>
#include <stdbool.h>
#include <stddef.h>

struct ck_rwlock {
	unsigned int writer;
	unsigned int n_readers;
};
typedef struct ck_rwlock ck_rwlock_t;

#define CK_RWLOCK_INITIALIZER {0, 0}

CK_CC_INLINE static void
ck_rwlock_init(struct ck_rwlock *rw)
{

	rw->writer = 0;
	rw->n_readers = 0;
	ck_pr_fence_memory();
	return;
}

CK_CC_INLINE static void
ck_rwlock_write_unlock(ck_rwlock_t *rw)
{

	ck_pr_fence_memory();
	ck_pr_store_uint(&rw->writer, 0);
	return;
}

#ifdef CK_F_PR_RTM
CK_CC_INLINE static void
ck_rwlock_write_unlock_rtm(ck_rwlock_t *rw)
{

	if (ck_pr_load_uint(&rw->writer) == 0) {
		ck_pr_rtm_end();
		return;
	}

	ck_rwlock_write_unlock(rw);
	return;
}
#endif /* CK_F_PR_RTM */

CK_CC_INLINE static void
ck_rwlock_write_downgrade(ck_rwlock_t *rw)
{

	ck_pr_inc_uint(&rw->n_readers);
	ck_rwlock_write_unlock(rw);
	return;
}

#ifdef CK_F_PR_RTM
CK_CC_INLINE static void
ck_rwlock_write_downgrade_rtm(ck_rwlock_t *rw)
{

	if (ck_pr_load_uint(&rw->writer) != 0) {
		ck_rwlock_write_downgrade(rw);
		return;
	}

	/*
	 * Both reader and writer counters are in read-set. A transactional
	 * abort will occur in the presence of another writer. Inner-most
	 * read_unlock call will attempt a transactional commit.
	 */
	return;
}
#endif /* CK_F_PR_RTM */

CK_CC_INLINE static bool
ck_rwlock_write_trylock(ck_rwlock_t *rw)
{

	if (ck_pr_fas_uint(&rw->writer, 1) != 0)
		return false;

	ck_pr_fence_memory();

	if (ck_pr_load_uint(&rw->n_readers) != 0) {
		ck_rwlock_write_unlock(rw);
		return false;
	}

	return true;
}

#ifdef CK_F_PR_RTM
CK_CC_INLINE static bool
ck_rwlock_write_trylock_rtm(ck_rwlock_t *rw)
{
	bool r;

	if (ck_pr_rtm_begin() != CK_PR_RTM_STARTED)
		return false;

	r = ck_pr_load_uint(&rw->writer) != 0;

	ck_pr_fence_load();

	if (r | (ck_pr_load_uint(&rw->n_readers) != 0))
		ck_pr_rtm_abort(0);

	return true;
}
#endif /* CK_F_PR_RTM */

CK_CC_INLINE static void
ck_rwlock_write_lock(ck_rwlock_t *rw)
{

	while (ck_pr_fas_uint(&rw->writer, 1) != 0)
		ck_pr_stall();

	ck_pr_fence_memory();

	while (ck_pr_load_uint(&rw->n_readers) != 0)
		ck_pr_stall();

	return;
}

#ifdef CK_F_PR_RTM
CK_CC_INLINE static void
ck_rwlock_write_lock_rtm(ck_rwlock_t *rw)
{
	bool r;

	if (ck_pr_rtm_begin() != CK_PR_RTM_STARTED) {
		ck_rwlock_write_lock(rw);
		return;
	}

	r = ck_pr_load_uint(&rw->writer) != 0;

	ck_pr_fence_load();

	if (r | (ck_pr_load_uint(&rw->n_readers) != 0))
		ck_pr_rtm_abort(0);

	return;
}
#endif /* CK_F_PR_RTM */

CK_CC_INLINE static bool
ck_rwlock_read_trylock(ck_rwlock_t *rw)
{

	if (ck_pr_load_uint(&rw->writer) != 0)
		return false;

	ck_pr_inc_uint(&rw->n_readers);

	/*
	 * Serialize with respect to concurrent write
	 * lock operation.
	 */
	ck_pr_fence_memory();

	if (ck_pr_load_uint(&rw->writer) == 0) {
		ck_pr_fence_load();
		return true;
	}

	ck_pr_dec_uint(&rw->n_readers);
	return false;
}

#ifdef CK_F_PR_RTM
CK_CC_INLINE static bool
ck_rwlock_read_trylock_rtm(ck_rwlock_t *rw)
{

	if (ck_pr_rtm_begin() != CK_PR_RTM_STARTED)
		return false;

	if (ck_pr_load_uint(&rw->writer) == 0)
		return true;

	ck_pr_rtm_abort(0);
	return false;
}
#endif /* CK_F_PR_RTM */

CK_CC_INLINE static void
ck_rwlock_read_lock(ck_rwlock_t *rw)
{

	for (;;) {
		while (ck_pr_load_uint(&rw->writer) != 0)
			ck_pr_stall();

		ck_pr_inc_uint(&rw->n_readers);

		/*
		 * Serialize with respect to concurrent write
		 * lock operation.
		 */
		ck_pr_fence_atomic_load();

		if (ck_pr_load_uint(&rw->writer) == 0)
			break;

		ck_pr_dec_uint(&rw->n_readers);
	}

	/* Acquire semantics are necessary. */
	ck_pr_fence_load();
	return;
}

#ifdef CK_F_PR_RTM
CK_CC_INLINE static void
ck_rwlock_read_lock_rtm(ck_rwlock_t *rw)
{

	if (ck_pr_rtm_begin() == CK_PR_RTM_STARTED) {
		if (ck_pr_load_uint(&rw->writer) != 0)
			ck_pr_rtm_abort(0);

		return;
	}

	ck_rwlock_read_lock(rw);
	return;
}
#endif /* CK_F_PR_RTM */

CK_CC_INLINE static void
ck_rwlock_read_unlock(ck_rwlock_t *rw)
{

	ck_pr_fence_memory();
	ck_pr_dec_uint(&rw->n_readers);
	return;
}

#ifdef CK_F_PR_RTM
CK_CC_INLINE static void
ck_rwlock_read_unlock_rtm(ck_rwlock_t *rw)
{

	if (ck_pr_load_uint(&rw->n_readers) == 0) {
		ck_pr_rtm_end();
	} else {
		ck_rwlock_read_unlock(rw);
	}

	return;
}
#endif /* CK_F_PR_RTM */

/*
 * Recursive writer reader-writer lock implementation.
 */
struct ck_rwlock_recursive {
	struct ck_rwlock rw;
	unsigned int wc;
};
typedef struct ck_rwlock_recursive ck_rwlock_recursive_t;

#define CK_RWLOCK_RECURSIVE_INITIALIZER {CK_RWLOCK_INITIALIZER, 0}

CK_CC_INLINE static void
ck_rwlock_recursive_write_lock(ck_rwlock_recursive_t *rw, unsigned int tid)
{
	unsigned int o;

	o = ck_pr_load_uint(&rw->rw.writer);
	if (o == tid)
		goto leave;

	while (ck_pr_cas_uint(&rw->rw.writer, 0, tid) == false)
		ck_pr_stall();

	ck_pr_fence_memory();

	while (ck_pr_load_uint(&rw->rw.n_readers) != 0)
		ck_pr_stall();

leave:
	rw->wc++;
	return;
}

CK_CC_INLINE static bool
ck_rwlock_recursive_write_trylock(ck_rwlock_recursive_t *rw, unsigned int tid)
{
	unsigned int o;

	o = ck_pr_load_uint(&rw->rw.writer);
	if (o == tid)
		goto leave;

	if (ck_pr_cas_uint(&rw->rw.writer, 0, tid) == false)
		return false;

	ck_pr_fence_memory();

	if (ck_pr_load_uint(&rw->rw.n_readers) != 0) {
		ck_pr_store_uint(&rw->rw.writer, 0);
		return false;
	}

leave:
	rw->wc++;
	return true;
}

CK_CC_INLINE static void
ck_rwlock_recursive_write_unlock(ck_rwlock_recursive_t *rw)
{

	if (--rw->wc == 0) {
		ck_pr_fence_memory();
		ck_pr_store_uint(&rw->rw.writer, 0);
	}

	return;
}

CK_CC_INLINE static void
ck_rwlock_recursive_read_lock(ck_rwlock_recursive_t *rw)
{

	ck_rwlock_read_lock(&rw->rw);
	return;
}

CK_CC_INLINE static bool
ck_rwlock_recursive_read_trylock(ck_rwlock_recursive_t *rw)
{

	return ck_rwlock_read_trylock(&rw->rw);
}

CK_CC_INLINE static void
ck_rwlock_recursive_read_unlock(ck_rwlock_recursive_t *rw)
{

	ck_rwlock_read_unlock(&rw->rw);
	return;
}

#endif /* _CK_RWLOCK_H */

