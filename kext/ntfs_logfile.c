/*
 * ntfs_logfile.c - NTFS kernel journal handling.
 *
 * Copyright (c) 2006-2011 Anton Altaparmakov.  All Rights Reserved.
 * Portions Copyright (c) 2006-2011 Apple Inc.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution. 
 * 3. Neither the name of Apple Inc. ("Apple") nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ALTERNATIVELY, provided that this notice and licensing terms are retained in
 * full, this file may be redistributed and/or modified under the terms of the
 * GNU General Public License (GPL) Version 2, in which case the provisions of
 * that version of the GPL will apply to you instead of the license terms
 * above.  You can obtain a copy of the GPL Version 2 at
 * http://developer.apple.com/opensource/licenses/gpl-2.txt.
 */

#include <sys/errno.h>
#include <sys/ucred.h>
#include <sys/ubc.h>

#include <string.h>

#include <libkern/libkern.h>

#include <IOKit/IOLib.h>

#include <kern/debug.h>

#include "ntfs.h"
#include "ntfs_attr.h"
#include "ntfs_debug.h"
#include "ntfs_endian.h"
#include "ntfs_inode.h"
#include "ntfs_layout.h"
#include "ntfs_logfile.h"
#include "ntfs_mst.h"
#include "ntfs_page.h"
#include "ntfs_types.h"
#include "ntfs_volume.h"

/**
 * ntfs_restart_page_header_is_valid - check the page header for consistency
 * @ni:		ntfs inode of $LogFile to which the restart page header belongs
 * @rp:		restart page header to check
 * @pos:	position in @ni at which the restart page header resides
 *
 * Check the restart page header @rp for consistency and return TRUE if it is
 * consistent and FALSE otherwise.
 *
 * This function only needs NTFS_BLOCK_SIZE bytes in @rp, i.e. it does not
 * require the full restart page.
 */
static BOOL ntfs_restart_page_header_is_valid(ntfs_inode *ni,
		RESTART_PAGE_HEADER *rp, s64 pos)
{
	u32 logfile_system_page_size, logfile_log_page_size;
	u16 ra_ofs, usa_count, usa_ofs, usa_end = 0;
	BOOL have_usa = TRUE;

	ntfs_debug("Entering.");
	/*
	 * If the system or log page sizes are smaller than the ntfs block size
	 * or either is not a power of 2 we cannot handle this log file.
	 */
	logfile_system_page_size = le32_to_cpu(rp->system_page_size);
	logfile_log_page_size = le32_to_cpu(rp->log_page_size);
	if (logfile_system_page_size < NTFS_BLOCK_SIZE ||
			logfile_log_page_size < NTFS_BLOCK_SIZE ||
			logfile_system_page_size &
			(logfile_system_page_size - 1) ||
			logfile_log_page_size & (logfile_log_page_size - 1)) {
		ntfs_error(ni->vol->mp, "$LogFile uses unsupported page "
				"size.");
		return FALSE;
	}
	/*
	 * We must be either at !pos (1st restart page) or at pos = system page
	 * size (2nd restart page).
	 */
	if (pos && pos != logfile_system_page_size) {
		ntfs_error(ni->vol->mp, "Found restart area in incorrect "
				"position in $LogFile.");
		return FALSE;
	}
	/* We only know how to handle version 1.1. */
	if (sle16_to_cpu(rp->major_ver) != 1 ||
			sle16_to_cpu(rp->minor_ver) != 1) {
		ntfs_error(ni->vol->mp, "$LogFile version %d.%d is not "
				"supported.  (This driver supports version "
				"1.1 only.)", (int)sle16_to_cpu(rp->major_ver),
				(int)sle16_to_cpu(rp->minor_ver));
		return FALSE;
	}
	/*
	 * If chkdsk has been run the restart page may not be protected by an
	 * update sequence array.
	 */
	if (ntfs_is_chkd_record(rp->magic) && !le16_to_cpu(rp->usa_count)) {
		have_usa = FALSE;
		goto skip_usa_checks;
	}
	/* Verify the size of the update sequence array. */
	usa_count = 1 + (logfile_system_page_size >> NTFS_BLOCK_SIZE_SHIFT);
	if (usa_count != le16_to_cpu(rp->usa_count)) {
		ntfs_error(ni->vol->mp, "$LogFile restart page specifies "
				"inconsistent update sequence array count.");
		return FALSE;
	}
	/* Verify the position of the update sequence array. */
	usa_ofs = le16_to_cpu(rp->usa_ofs);
	usa_end = usa_ofs + usa_count * sizeof(u16);
	if (usa_ofs < sizeof(RESTART_PAGE_HEADER) ||
			usa_end > NTFS_BLOCK_SIZE - sizeof(u16)) {
		ntfs_error(ni->vol->mp, "$LogFile restart page specifies "
				"inconsistent update sequence array offset.");
		return FALSE;
	}
skip_usa_checks:
	/*
	 * Verify the position of the restart area.  It must be:
	 *  - aligned to 8-byte boundary,
	 *  - after the update sequence array, and
	 *  - within the system page size.
	 */
	ra_ofs = le16_to_cpu(rp->restart_area_offset);
	if (ra_ofs & 7 || (have_usa ? ra_ofs < usa_end :
			ra_ofs < sizeof(RESTART_PAGE_HEADER)) ||
			ra_ofs > logfile_system_page_size) {
		ntfs_error(ni->vol->mp, "$LogFile restart page specifies "
				"inconsistent restart area offset.");
		return FALSE;
	}
	/*
	 * Only restart pages modified by chkdsk are allowed to have chkdsk_lsn
	 * set.
	 */
	if (!ntfs_is_chkd_record(rp->magic) && sle64_to_cpu(rp->chkdsk_lsn)) {
		ntfs_error(ni->vol->mp, "$LogFile restart page is not "
				"modified by chkdsk but a chkdsk LSN is "
				"specified.");
		return FALSE;
	}
	ntfs_debug("Done.");
	return TRUE;
}

/**
 * ntfs_restart_area_is_valid - check the restart area for consistency
 * @ni:		ntfs inode of $LogFile to which the restart page belongs
 * @rp:		restart page whose restart area to check
 *
 * Check the restart area of the restart page @rp for consistency and return
 * TRUE if it is consistent and FALSE otherwise.
 *
 * This function assumes that the restart page header has already been
 * consistency checked.
 *
 * This function only needs NTFS_BLOCK_SIZE bytes in @rp, i.e. it does not
 * require the full restart page.
 */
static BOOL ntfs_restart_area_is_valid(ntfs_inode *ni, RESTART_PAGE_HEADER *rp)
{
	u64 file_size;
	RESTART_AREA *ra;
	u16 ra_ofs, ra_len, ca_ofs;
	u8 fs_bits;

	ntfs_debug("Entering.");
	ra_ofs = le16_to_cpu(rp->restart_area_offset);
	ra = (RESTART_AREA*)((u8*)rp + ra_ofs);
	/*
	 * Everything before ra->file_size must be before the first word
	 * protected by an update sequence number.  This ensures that it is
	 * safe to access ra->client_array_offset.
	 */
	if (ra_ofs + offsetof(RESTART_AREA, file_size) >
			NTFS_BLOCK_SIZE - sizeof(u16)) {
		ntfs_error(ni->vol->mp, "$LogFile restart area specifies "
				"inconsistent file offset.");
		return FALSE;
	}
	/*
	 * Now that we can access ra->client_array_offset, make sure everything
	 * up to the log client array is before the first word protected by an
	 * update sequence number.  This ensures we can access all of the
	 * restart area elements safely.  Also, the client array offset must be
	 * aligned to an 8-byte boundary.
	 */
	ca_ofs = le16_to_cpu(ra->client_array_offset);
	if (((ca_ofs + 7) & ~7) != ca_ofs ||
			ra_ofs + ca_ofs > NTFS_BLOCK_SIZE - sizeof(u16)) {
		ntfs_error(ni->vol->mp, "$LogFile restart area specifies "
				"inconsistent client array offset.");
		return FALSE;
	}
	/*
	 * The restart area must end within the system page size both when
	 * calculated manually and as specified by ra->restart_area_length.
	 * Also, the calculated length must not exceed the specified length.
	 */
	ra_len = ca_ofs + le16_to_cpu(ra->log_clients) *
			sizeof(LOG_CLIENT_RECORD);
	if (ra_ofs + ra_len > le32_to_cpu(rp->system_page_size) ||
			ra_ofs + le16_to_cpu(ra->restart_area_length) >
			le32_to_cpu(rp->system_page_size) ||
			ra_len > le16_to_cpu(ra->restart_area_length)) {
		ntfs_error(ni->vol->mp, "$LogFile restart area is out of "
				"bounds of the system page size specified by "
				"the restart page header and/or the specified "
				"restart area length is inconsistent.");
		return FALSE;
	}
	/*
	 * The ra->client_free_list and ra->client_in_use_list must be either
	 * LOGFILE_NO_CLIENT or less than ra->log_clients or they are
	 * overflowing the client array.
	 */
	if ((ra->client_free_list != LOGFILE_NO_CLIENT &&
			le16_to_cpu(ra->client_free_list) >=
			le16_to_cpu(ra->log_clients)) ||
			(ra->client_in_use_list != LOGFILE_NO_CLIENT &&
			le16_to_cpu(ra->client_in_use_list) >=
			le16_to_cpu(ra->log_clients))) {
		ntfs_error(ni->vol->mp, "$LogFile restart area specifies "
				"overflowing client free and/or in use lists.");
		return FALSE;
	}
	/*
	 * Check ra->seq_number_bits against ra->file_size for consistency.
	 * We cannot just use ffs() because the file size is not a power of 2.
	 */
	file_size = (u64)sle64_to_cpu(ra->file_size);
	fs_bits = 0;
	while (file_size) {
		file_size >>= 1;
		fs_bits++;
	}
	if (le32_to_cpu(ra->seq_number_bits) != (unsigned)(67 - fs_bits)) {
		ntfs_error(ni->vol->mp, "$LogFile restart area specifies "
				"inconsistent sequence number bits.");
		return FALSE;
	}
	/* The log record header length must be a multiple of 8. */
	if (((le16_to_cpu(ra->log_record_header_length) + 7) & ~7) !=
			le16_to_cpu(ra->log_record_header_length)) {
		ntfs_error(ni->vol->mp, "$LogFile restart area specifies "
				"inconsistent log record header length.");
		return FALSE;
	}
	/* Dito for the log page data offset. */
	if (((le16_to_cpu(ra->log_page_data_offset) + 7) & ~7) !=
			le16_to_cpu(ra->log_page_data_offset)) {
		ntfs_error(ni->vol->mp, "$LogFile restart area specifies "
				"inconsistent log page data offset.");
		return FALSE;
	}
	ntfs_debug("Done.");
	return TRUE;
}

/**
 * ntfs_log_client_array_is_consistent - consistency check the log client array
 * @ni:		ntfs inode of $LogFile to which the restart page belongs
 * @rp:		restart page whose log client array to check
 *
 * Check the log client array of the restart page @rp for consistency and
 * return TRUE if it is consistent and FALSE otherwise.
 *
 * This function assumes that the restart page header and the restart area have
 * already been consistency checked.
 *
 * Unlike ntfs_restart_page_header_is_valid() and ntfs_restart_area_is_valid(),
 * this function needs @rp->system_page_size bytes in @rp, i.e. it requires the
 * full restart page and the page must be multi sector transfer deprotected.
 */
static BOOL ntfs_log_client_array_is_consistent(ntfs_inode *ni,
		RESTART_PAGE_HEADER *rp)
{
	RESTART_AREA *ra;
	LOG_CLIENT_RECORD *ca, *cr;
	u16 nr_clients, idx;
	BOOL in_free_list, idx_is_first;

	ntfs_debug("Entering.");
	ra = (RESTART_AREA*)((u8*)rp + le16_to_cpu(rp->restart_area_offset));
	ca = (LOG_CLIENT_RECORD*)((u8*)ra +
			le16_to_cpu(ra->client_array_offset));
	/*
	 * Check the ra->client_free_list first and then check the
	 * ra->client_in_use_list.  Check each of the log client records in
	 * each of the lists and check that the array does not overflow the
	 * ra->log_clients value.  Also keep track of the number of records
	 * visited as there cannot be more than ra->log_clients records and
	 * that way we detect eventual loops in within a list.
	 */
	nr_clients = le16_to_cpu(ra->log_clients);
	idx = le16_to_cpu(ra->client_free_list);
	in_free_list = TRUE;
check_list:
	for (idx_is_first = TRUE; idx != LOGFILE_NO_CLIENT_CPU; nr_clients--,
			idx = le16_to_cpu(cr->next_client)) {
		if (!nr_clients || idx >= le16_to_cpu(ra->log_clients))
			goto err;
		/* Set @cr to the current log client record. */
		cr = ca + idx;
		/* The first log client record must not have a prev_client. */
		if (idx_is_first) {
			if (cr->prev_client != LOGFILE_NO_CLIENT)
				goto err;
			idx_is_first = FALSE;
		}
	}
	/* Switch to and check the in use list if we just did the free list. */
	if (in_free_list) {
		in_free_list = FALSE;
		idx = le16_to_cpu(ra->client_in_use_list);
		goto check_list;
	}
	ntfs_debug("Done.");
	return TRUE;
err:
	ntfs_error(ni->vol->mp, "$LogFile log client array is corrupt.");
	return FALSE;
}

/**
 * ntfs_restart_page_load - load and check the restart page for consistency
 * @ni:		ntfs inode of $LogFile to which the restart page belongs
 * @rp:		restart page to check
 * @pos:	position in @ni at which the restart page resides
 * @wrp:	[OUT] copy of the multi sector transfer deprotected restart page
 * @lsn:	[OUT] set to the current logfile lsn on success
 *
 * Check the restart page @rp for consistency and return 0 if it is consistent
 * and errno otherwise.  The restart page may have been modified by chkdsk in
 * which case its magic is CHKD instead of RSTR.
 *
 * This function only needs NTFS_BLOCK_SIZE bytes in @rp, i.e. it does not
 * require the full restart page.
 *
 * If @wrp is not NULL, on success, *@wrp will point to a buffer containing a
 * copy of the complete multi sector transfer deprotected page.  On failure,
 * *@wrp is undefined.
 *
 * Simillarly, if @lsn is not NULL, on success *@lsn will be set to the current
 * logfile lsn according to this restart page.  On failure, *@lsn is undefined.
 *
 * The following error codes are defined:
 *	EINVAL	- The restart page is inconsistent.
 *	ENOMEM	- Not enough memory to load the restart page.
 *	EIO	- Failed to read from $LogFile.
 */
static errno_t ntfs_restart_page_load(ntfs_inode *ni, RESTART_PAGE_HEADER *rp,
		s64 pos, RESTART_PAGE_HEADER **wrp, LSN *lsn)
{
	RESTART_AREA *ra;
	RESTART_PAGE_HEADER *trp;
	unsigned size;
	errno_t err;

	ntfs_debug("Entering.");
	/* Check the restart page header for consistency. */
	if (!ntfs_restart_page_header_is_valid(ni, rp, pos)) {
		/* Error output already done inside the function. */
		return EINVAL;
	}
	/* Check the restart area for consistency. */
	if (!ntfs_restart_area_is_valid(ni, rp)) {
		/* Error output already done inside the function. */
		return EINVAL;
	}
	ra = (RESTART_AREA*)((u8*)rp + le16_to_cpu(rp->restart_area_offset));
	/*
	 * Allocate a buffer to store the whole restart page so we can multi
	 * sector transfer deprotect it.
	 */
	trp = IOMalloc(le32_to_cpu(rp->system_page_size));
	if (!trp) {
		ntfs_error(ni->vol->mp, "Failed to allocate memory for "
				"$LogFile restart page buffer.");
		return ENOMEM;
	}
	/*
	 * Read the whole of the restart page into the buffer.  If it fits
	 * completely inside @rp, just copy it from there.  Otherwise map all
	 * the required pages and copy the data from them.
	 */
	size = PAGE_SIZE - ((unsigned)pos & PAGE_MASK);
	if (size >= le32_to_cpu(rp->system_page_size))
		memcpy(trp, rp, le32_to_cpu(rp->system_page_size));
	else {
		upl_t upl;
		upl_page_info_array_t pl;
		u8 *kaddr;
		unsigned have_read, to_read;

		/* First copy what we already have in @rp. */
		memcpy(trp, rp, size);
		/* Copy the remaining data one page at a time. */
		have_read = size;
		to_read = le32_to_cpu(rp->system_page_size) - size;
		do {
			pos += size;
			if ((unsigned)pos & PAGE_MASK)
				panic("%s(): pos + size is not PAGE_SIZE "
						"aligned\n", __FUNCTION__);
			err = ntfs_page_map(ni, pos, &upl, &pl, &kaddr, FALSE);
			if (err) {
				ntfs_error(ni->vol->mp, "Error reading "
						"$LogFile.");
				if (err != EIO && err != ENOMEM)
					err = EIO;
				goto err;
			}
			size = PAGE_SIZE;
			if (size > to_read)
				size = to_read;
			memcpy((u8*)trp + have_read, kaddr, size);
			ntfs_page_unmap(ni, upl, pl, FALSE);
			have_read += size;
			to_read -= size;
		} while (to_read > 0);
	}
	/*
	 * Perform the multi sector transfer deprotection on the buffer if the
	 * restart page is protected.
	 */
	if ((!ntfs_is_chkd_record(trp->magic) || le16_to_cpu(trp->usa_count)) &&
			ntfs_mst_fixup_post_read((NTFS_RECORD*)trp,
			le32_to_cpu(rp->system_page_size))) {
		/*
		 * A multi sector tranfer error was detected.  We only need to
		 * abort if the restart page contents exceed the multi sector
		 * transfer fixup of the first sector.
		 */
		if (le16_to_cpu(rp->restart_area_offset) +
				le16_to_cpu(ra->restart_area_length) >
				NTFS_BLOCK_SIZE - sizeof(u16)) {
			ntfs_error(ni->vol->mp, "Multi sector transfer error "
					"detected in $LogFile restart page.");
			err = EINVAL;
			goto err;
		}
	}
	/*
	 * If the restart page is modified by chkdsk or there are no active
	 * logfile clients, the logfile is consistent.  Otherwise, need to
	 * check the log client records for consistency, too.
	 */
	err = 0;
	if (ntfs_is_rstr_record(rp->magic) &&
			ra->client_in_use_list != LOGFILE_NO_CLIENT) {
		if (!ntfs_log_client_array_is_consistent(ni, trp)) {
			err = EINVAL;
			goto err;
		}
	}
	if (lsn) {
		if (ntfs_is_rstr_record(rp->magic))
			*lsn = sle64_to_cpu(ra->current_lsn);
		else /* if (ntfs_is_chkd_record(rp->magic)) */
			*lsn = sle64_to_cpu(rp->chkdsk_lsn);
	}
	ntfs_debug("Done.");
	if (wrp)
		*wrp = trp;
	else {
err:
		IOFree(trp, le32_to_cpu(trp->system_page_size));
	}
	return err;
}

/**
 * ntfs_logfile_check - check the $LogFile journal for consistency
 * @ni:		ntfs inode of loaded $LogFile journal to check
 * @rp:		[OUT] on success this is a copy of the current restart page
 *
 * Check the $LogFile journal for consistency and return 0 if it is consistent
 * and EINVAL if not.  On success, the current restart page is returned in
 * *@rp.  Caller must call IOFreeData(*@rp, le32_to_cpu(*@rp->system_page_size)
 * when finished with it.
 *
 * On error the error code (not EINVAL) is returned.
 *
 * At present we only check the two restart pages and ignore the log record
 * pages.
 *
 * Note that the MstProtected flag is not set on the $LogFile inode and hence
 * when reading pages they are not deprotected.  This is because we do not know
 * if the $LogFile was created on a system with a different page size to ours
 * yet and mst deprotection would fail if our page size is smaller.
 */
errno_t ntfs_logfile_check(ntfs_inode *ni, RESTART_PAGE_HEADER **rp)
{
	s64 size, pos, ppos;
	LSN rstr1_lsn, rstr2_lsn;
	ntfs_volume *vol;
	RESTART_PAGE_HEADER *rstr1_ph, *rstr2_ph;
	upl_t upl;
	upl_page_info_array_t pl;
	u8 *paddr, *kaddr;
	unsigned log_page_size, log_page_mask;
	errno_t err;
	BOOL logfile_is_empty = TRUE;
	u8 log_page_bits;

	ntfs_debug("Entering.");
	vol = ni->vol;
	/*
	 * If the $LogFile is empty we will return success but we will not be
	 * returning anything in *@rp thus set it to NULL now to cover for all
	 * possible cases of us returning without allocating *@rp.
	 */
	if (rp)
		*rp = NULL;
	/* An empty $LogFile must have been clean before it got emptied. */
	if (NVolLogFileEmpty(vol))
		goto is_empty;
	err = vnode_get(ni->vn);
	if (err) {
		if (err == EINVAL)
			err = EIO;
		ntfs_error(vol->mp, "Failed to get vnode for $LogFile.");
		return err;
	}
	lck_rw_lock_shared(&ni->lock);
	lck_spin_lock(&ni->size_lock);
	size = ni->data_size;
	lck_spin_unlock(&ni->size_lock);
	/* Make sure the file does not exceed the maximum allowed size. */
	if (size > (s64)NtfsMaxLogFileSize)
		size = NtfsMaxLogFileSize;
	/*
	 * Truncate size to a multiple of the page cache size or the default
	 * log page size if the page cache size is between the default log page
	 * log page size if the page cache size is between the default log page
	 * size and twice that.
	 */
	if (PAGE_SIZE >= NtfsDefaultLogPageSize &&
			PAGE_SIZE <= NtfsDefaultLogPageSize * 2)
		log_page_size = NtfsDefaultLogPageSize;
	else
		log_page_size = PAGE_SIZE;
	log_page_mask = log_page_size - 1;
	log_page_bits = ffs(log_page_size) - 1;
	size &= ~(s64)(log_page_size - 1);
	/*
	 * Ensure the log file is big enough to store at least the two restart
	 * pages and the minimum number of log record pages.
	 */
	if (size < log_page_size * 2 || (size - log_page_size * 2) >>
			log_page_bits < NtfsMinLogRecordPages) {
		ntfs_error(vol->mp, "$LogFile is too small.");
		lck_rw_unlock_shared(&ni->lock);
		(void)vnode_put(ni->vn);
		return EINVAL;
	}
	/*
	 * Read through the file looking for a restart page.  Since the restart
	 * page header is at the beginning of a page we only need to search at
	 * what could be the beginning of a page (for each page size) rather
	 * than scanning the whole file byte by byte.  If all potential places
	 * contain empty and uninitialzed records, the log file can be assumed
	 * to be empty.
	 */
	upl = NULL;
	rstr1_ph = rstr2_ph = NULL;
	for (pos = ppos = 0; pos < size; pos <<= 1) {
		if (!upl || (s64)(pos & ~PAGE_MASK_64) != ppos) {
			if (upl)
				ntfs_page_unmap(ni, upl, pl, FALSE);
			ppos = pos;
			err = ntfs_page_map(ni, pos, &upl, &pl, &paddr, FALSE);
			if (err) {
				if (err != EIO && err != ENOMEM)
					err = EIO;
				ntfs_error(vol->mp, "Error reading $LogFile.");
				goto err;
			}
		}
		kaddr = paddr + (pos & PAGE_MASK_64);
		/*
		 * A non-empty block means the logfile is not empty while an
		 * empty block after a non-empty block has been encountered
		 * means we are done.
		 */
		if (!ntfs_is_empty_recordp((le32*)kaddr))
			logfile_is_empty = FALSE;
		else {
			if (logfile_is_empty) {
				/*
				 * All records so far have been empty,
				 * continue.
				 */
				if (!pos)
					pos = NTFS_BLOCK_SIZE / 2;
				continue;
			}
			/*
			 * This is the first empty record and at least one
			 * non-empty record has been found previously.  We are
			 * done.
			 */
			break;
		}
		/*
		 * A log record page means there cannot be a restart page after
		 * this so no need to continue searching.
		 */
		if (ntfs_is_rcrd_recordp((le32*)kaddr))
			break;
		/* If not a (modified by chkdsk) restart page, continue. */
		if (!ntfs_is_rstr_recordp((le32*)kaddr) &&
				!ntfs_is_chkd_recordp((le32*)kaddr)) {
			if (!pos)
				pos = NTFS_BLOCK_SIZE / 2;
			continue;
		}
		/*
		 * Check the (modified by chkdsk) restart page for consistency
		 * and get a copy of the complete multi sector transfer
		 * deprotected restart page.
		 */
		err = ntfs_restart_page_load(ni,
				(RESTART_PAGE_HEADER*)kaddr, pos,
				!rstr1_ph ? &rstr1_ph : &rstr2_ph,
				!rstr1_ph ? &rstr1_lsn : &rstr2_lsn);
		if (!err) {
			/*
			 * If we have now found the first (modified by chkdsk)
			 * restart page, continue looking for the second one.
			 */
			if (!pos) {
				pos = NTFS_BLOCK_SIZE / 2;
				continue;
			}
			/*
			 * We have now found the second (modified by chkdsk)
			 * restart page, so we can stop looking.
			 */
			break;
		}
		/*
		 * Error output already done inside the function.  Note, we do
		 * not abort if the restart page was invalid as we might still
		 * find a valid one further in the file.
		 */
		if (err != EINVAL) {
			ntfs_page_unmap(ni, upl, pl, FALSE);
			goto err;
		}
		/* Continue looking. */
		if (!pos)
			pos = NTFS_BLOCK_SIZE / 2;
	}
	if (upl)
		ntfs_page_unmap(ni, upl, pl, FALSE);
	lck_rw_unlock_shared(&ni->lock);
	(void)vnode_put(ni->vn);
	if (logfile_is_empty) {
		NVolSetLogFileEmpty(vol);
is_empty:
		ntfs_debug("Done.  ($LogFile is empty.)");
		return 0;
	}
	if (!rstr1_ph) {
		if (rstr2_ph)
			panic("%s(): !rstr1_ph but rstr2_ph\n", __FUNCTION__);
		ntfs_error(vol->mp, "Did not find any restart pages in "
				"$LogFile and it was not empty.");
		return EINVAL;
	}
	/* If both restart pages were found, use the more recent one. */
	if (rstr2_ph) {
		/*
		 * If the second restart area is more recent, switch to it.
		 * Otherwise just throw it away.
		 */
		if (rstr2_lsn > rstr1_lsn) {
			ntfs_debug("Using second restart page as it is more "
					"recent.");
			IOFree(rstr1_ph, le32_to_cpu(rstr1_ph->system_page_size));
			rstr1_ph = rstr2_ph;
			/* rstr1_lsn = rstr2_lsn; */
		} else {
			ntfs_debug("Using first restart page as it is more "
					"recent.");
			IOFree(rstr2_ph, le32_to_cpu(rstr2_ph->system_page_size));
		}
		rstr2_ph = NULL;
	}
	/* All consistency checks passed. */
	if (rp)
		*rp = rstr1_ph;
	else
		IOFree(rstr1_ph, le32_to_cpu(rstr1_ph->system_page_size));
	ntfs_debug("Done.");
	return 0;
err:
	lck_rw_unlock_shared(&ni->lock);
	(void)vnode_put(ni->vn);
	if (rstr1_ph)
		IOFree(rstr1_ph, le32_to_cpu(rstr1_ph->system_page_size));
	return err;
}

/**
 * ntfs_logfile_is_clean - check in the journal of the volume is clean
 * @ni:		ntfs inode of loaded $LogFile journal to check
 * @rp:		copy of the current restart page
 *
 * Analyze the $LogFile journal and return TRUE if it indicates the volume was
 * shutdown cleanly and FALSE if not.
 *
 * At present we only look at the two restart pages and ignore the log record
 * pages.  This is a little bit crude in that there will be a very small number
 * of cases where we think that a volume is dirty when in fact it is clean.
 * This should only affect volumes that have not been shutdown cleanly but did
 * not have any pending, non-check-pointed i/o, i.e. they were completely idle
 * at least for the five seconds preceeding the unclean shutdown.
 *
 * This function assumes that the $LogFile journal has already been consistency
 * checked by a call to ntfs_check_logfile() and in particular if the $LogFile
 * is empty this function requires that NVolLogFileEmpty() is TRUE otherwise an
 * empty volume will be reported as dirty.
 */
BOOL ntfs_logfile_is_clean(ntfs_inode *ni, const RESTART_PAGE_HEADER *rp)
{
	ntfs_volume *vol = ni->vol;
	RESTART_AREA *ra;

	ntfs_debug("Entering.");
	/* An empty $LogFile must have been clean before it got emptied. */
	if (NVolLogFileEmpty(vol)) {
		ntfs_debug("Done.  ($LogFile is empty.)");
		return TRUE;
	}
	if (!rp)
		panic("%s(): !rp\n", __FUNCTION__);
	if (!ntfs_is_rstr_record(rp->magic) &&
			!ntfs_is_chkd_record(rp->magic)) {
		ntfs_error(vol->mp, "Restart page buffer is invalid.  This is "
				"probably a bug in that the $LogFile should "
				"have been consistency checked before calling "
				"this function.");
		return FALSE;
	}
	ra = (RESTART_AREA*)((u8*)rp + le16_to_cpu(rp->restart_area_offset));
	/*
	 * If the $LogFile has active clients, i.e. it is open, and we do not
	 * have the RESTART_VOLUME_IS_CLEAN bit set in the restart area flags,
	 * we assume there was an unclean shutdown.
	 */
	if (ra->client_in_use_list != LOGFILE_NO_CLIENT &&
			!(ra->flags & RESTART_VOLUME_IS_CLEAN)) {
		ntfs_debug("Done.  $LogFile indicates a dirty shutdown.");
		return FALSE;
	}
	/* $LogFile indicates a clean shutdown. */
	ntfs_debug("Done.  $LogFile indicates a clean shutdown.");
	return TRUE;
}

/**
 * ntfs_logfile_empty - empty the contents of the $LogFile journal
 * @ni:		ntfs inode of loaded $LogFile journal to empty
 *
 * Empty the contents of the $LogFile journal @ni.
 *
 * Return 0 on success and errno on error.
 *
 * This function assumes that the $LogFile journal has already been consistency
 * checked by a call to ntfs_logfile_check() and that ntfs_logfile_is_clean()
 * has been used to ensure that the $LogFile is clean.
 */
errno_t ntfs_logfile_empty(ntfs_inode *ni)
{
	ntfs_volume *vol = ni->vol;

	ntfs_debug("Entering.");
	if (!NVolLogFileEmpty(vol)) {
		s64 data_size;
		errno_t err;

		err = vnode_get(ni->vn);
		if (err) {
			ntfs_error(vol->mp, "Failed to get vnode for "
					"$LogFile.");
			return err;
		}
		lck_rw_lock_shared(&ni->lock);
		lck_spin_lock(&ni->size_lock);
		data_size = ni->data_size;
		lck_spin_unlock(&ni->size_lock);
		err = ntfs_attr_set(ni, 0, data_size, 0xff);
		lck_rw_unlock_shared(&ni->lock);
		(void)vnode_put(ni->vn);
		if (err) {
			ntfs_error(vol->mp, "Failed to fill $LogFile with "
					"0xff bytes (error code %d).", err);
			return err;
		}
		/* Set the flag so we do not have to do it again on remount. */
		NVolSetLogFileEmpty(vol);
	}
	ntfs_debug("Done.");
	return 0;
}
