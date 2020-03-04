/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "journal.h"
#include <small/region.h>
#include <diag.h>

int
journal_no_write_async(struct journal *journal,
		       struct journal_entry *entry,
		       journal_entry_complete_cb on_complete_cb,
		       void *on_complete_cb_data)
{
	(void)journal;
	(void)entry;
	(void)on_complete_cb;
	(void)on_complete_cb_data;

	say_error("journal: write_async called from invalid context");
	return -1;
}

/**
 * Used to load from a memtx snapshot. LSN is not used,
 * but txn_commit() must work.
 */
static int
dummy_journal_write(struct journal *journal, struct journal_entry *entry)
{
	(void) journal;
	entry->res = 0;
	return 0;
}

static struct journal dummy_journal = {
	.write_async	= journal_no_write_async,
	.write		= dummy_journal_write,
};

struct journal *current_journal = &dummy_journal;

struct journal_entry *
journal_entry_new(size_t n_rows, struct region *region)
{
	struct journal_entry *entry;

	size_t size = (sizeof(struct journal_entry) +
		       sizeof(entry->rows[0]) * n_rows);

	entry = region_aligned_alloc(region, size,
				     alignof(struct journal_entry));
	if (entry == NULL) {
		diag_set(OutOfMemory, size, "region", "struct journal_entry");
		return NULL;
	}

	entry->approx_len = 0;
	entry->n_rows = n_rows;
	entry->res = -1;
	entry->on_complete_cb = NULL;
	entry->on_complete_cb_data = NULL;

	return entry;
}
