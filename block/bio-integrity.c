// SPDX-License-Identifier: GPL-2.0
/*
 * bio-integrity.c - bio data integrity extensions
 *
 * Copyright (C) 2007, 2008, 2009 Oracle Corporation
 * Written by: Martin K. Petersen <martin.petersen@oracle.com>
 */

#include <linux/blk-integrity.h>
#include <linux/mempool.h>
#include <linux/export.h>
#include <linux/bio.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include "blk.h"

static struct kmem_cache *bip_slab;
static struct workqueue_struct *kintegrityd_wq;

void blk_flush_integrity(void)
{
	flush_workqueue(kintegrityd_wq);
}

/**
 * bio_integrity_free - Free bio integrity payload
 * @bio:	bio containing bip to be freed
 *
 * Description: Free the integrity portion of a bio.
 */
void bio_integrity_free(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct bio_set *bs = bio->bi_pool;

	if (bs && mempool_initialized(&bs->bio_integrity_pool)) {
		if (bip->bip_vec)
			bvec_free(&bs->bvec_integrity_pool, bip->bip_vec,
				  bip->bip_max_vcnt);
		mempool_free(bip, &bs->bio_integrity_pool);
	} else {
		kfree(bip);
	}
	bio->bi_integrity = NULL;
	bio->bi_opf &= ~REQ_INTEGRITY;
}

/**
 * bio_integrity_alloc - Allocate integrity payload and attach it to bio
 * @bio:	bio to attach integrity metadata to
 * @gfp_mask:	Memory allocation mask
 * @nr_vecs:	Number of integrity metadata scatter-gather elements
 *
 * Description: This function prepares a bio for attaching integrity
 * metadata.  nr_vecs specifies the maximum number of pages containing
 * integrity metadata that can be attached.
 */
struct bio_integrity_payload *bio_integrity_alloc(struct bio *bio,
						  gfp_t gfp_mask,
						  unsigned int nr_vecs)
{
	struct bio_integrity_payload *bip;
	struct bio_set *bs = bio->bi_pool;
	unsigned inline_vecs;

	if (WARN_ON_ONCE(bio_has_crypt_ctx(bio)))
		return ERR_PTR(-EOPNOTSUPP);

	if (!bs || !mempool_initialized(&bs->bio_integrity_pool)) {
		bip = kmalloc(struct_size(bip, bip_inline_vecs, nr_vecs), gfp_mask);
		inline_vecs = nr_vecs;
	} else {
		bip = mempool_alloc(&bs->bio_integrity_pool, gfp_mask);
		inline_vecs = BIO_INLINE_VECS;
	}

	if (unlikely(!bip))
		return ERR_PTR(-ENOMEM);

	memset(bip, 0, sizeof(*bip));

	/* always report as many vecs as asked explicitly, not inline vecs */
	bip->bip_max_vcnt = nr_vecs;
	if (nr_vecs > inline_vecs) {
		bip->bip_vec = bvec_alloc(&bs->bvec_integrity_pool,
					  &bip->bip_max_vcnt, gfp_mask);
		if (!bip->bip_vec)
			goto err;
	} else if (nr_vecs) {
		bip->bip_vec = bip->bip_inline_vecs;
	}

	bip->bip_bio = bio;
	bio->bi_integrity = bip;
	bio->bi_opf |= REQ_INTEGRITY;

	return bip;
err:
	if (bs && mempool_initialized(&bs->bio_integrity_pool))
		mempool_free(bip, &bs->bio_integrity_pool);
	else
		kfree(bip);
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL(bio_integrity_alloc);

static void bio_integrity_unpin_bvec(struct bio_vec *bv, int nr_vecs,
				     bool dirty)
{
	int i;

	for (i = 0; i < nr_vecs; i++) {
		if (dirty && !PageCompound(bv[i].bv_page))
			set_page_dirty_lock(bv[i].bv_page);
		unpin_user_page(bv[i].bv_page);
	}
}

static void bio_integrity_uncopy_user(struct bio_integrity_payload *bip)
{
	unsigned short orig_nr_vecs = bip->bip_max_vcnt - 1;
	struct bio_vec *orig_bvecs = &bip->bip_vec[1];
	struct bio_vec *bounce_bvec = &bip->bip_vec[0];
	size_t bytes = bounce_bvec->bv_len;
	struct iov_iter orig_iter;
	int ret;

	iov_iter_bvec(&orig_iter, ITER_DEST, orig_bvecs, orig_nr_vecs, bytes);
	ret = copy_to_iter(bvec_virt(bounce_bvec), bytes, &orig_iter);
	WARN_ON_ONCE(ret != bytes);

	bio_integrity_unpin_bvec(orig_bvecs, orig_nr_vecs, true);
}

/**
 * bio_integrity_unmap_user - Unmap user integrity payload
 * @bio:	bio containing bip to be unmapped
 *
 * Unmap the user mapped integrity portion of a bio.
 */
void bio_integrity_unmap_user(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);

	if (bip->bip_flags & BIP_COPY_USER) {
		if (bio_data_dir(bio) == READ)
			bio_integrity_uncopy_user(bip);
		kfree(bvec_virt(bip->bip_vec));
		return;
	}

	bio_integrity_unpin_bvec(bip->bip_vec, bip->bip_max_vcnt,
			bio_data_dir(bio) == READ);
}

/**
 * bio_integrity_add_page - Attach integrity metadata
 * @bio:	bio to update
 * @page:	page containing integrity metadata
 * @len:	number of bytes of integrity metadata in page
 * @offset:	start offset within page
 *
 * Description: Attach a page containing integrity metadata to bio.
 */
int bio_integrity_add_page(struct bio *bio, struct page *page,
			   unsigned int len, unsigned int offset)
{
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);
	struct bio_integrity_payload *bip = bio_integrity(bio);

	if (bip->bip_vcnt > 0) {
		struct bio_vec *bv = &bip->bip_vec[bip->bip_vcnt - 1];
		bool same_page = false;

		if (bvec_try_merge_hw_page(q, bv, page, len, offset,
					   &same_page)) {
			bip->bip_iter.bi_size += len;
			return len;
		}

		if (bip->bip_vcnt >=
		    min(bip->bip_max_vcnt, queue_max_integrity_segments(q)))
			return 0;

		/*
		 * If the queue doesn't support SG gaps and adding this segment
		 * would create a gap, disallow it.
		 */
		if (bvec_gap_to_prev(&q->limits, bv, offset))
			return 0;
	}

	bvec_set_page(&bip->bip_vec[bip->bip_vcnt], page, len, offset);
	bip->bip_vcnt++;
	bip->bip_iter.bi_size += len;

	return len;
}
EXPORT_SYMBOL(bio_integrity_add_page);

static int bio_integrity_copy_user(struct bio *bio, struct bio_vec *bvec,
				   int nr_vecs, unsigned int len,
				   unsigned int direction, u32 seed)
{
	bool write = direction == ITER_SOURCE;
	struct bio_integrity_payload *bip;
	struct iov_iter iter;
	void *buf;
	int ret;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (write) {
		iov_iter_bvec(&iter, direction, bvec, nr_vecs, len);
		if (!copy_from_iter_full(buf, len, &iter)) {
			ret = -EFAULT;
			goto free_buf;
		}

		bip = bio_integrity_alloc(bio, GFP_KERNEL, 1);
	} else {
		memset(buf, 0, len);

		/*
		 * We need to preserve the original bvec and the number of vecs
		 * in it for completion handling
		 */
		bip = bio_integrity_alloc(bio, GFP_KERNEL, nr_vecs + 1);
	}

	if (IS_ERR(bip)) {
		ret = PTR_ERR(bip);
		goto free_buf;
	}

	if (write)
		bio_integrity_unpin_bvec(bvec, nr_vecs, false);
	else
		memcpy(&bip->bip_vec[1], bvec, nr_vecs * sizeof(*bvec));

	ret = bio_integrity_add_page(bio, virt_to_page(buf), len,
				     offset_in_page(buf));
	if (ret != len) {
		ret = -ENOMEM;
		goto free_bip;
	}

	bip->bip_flags |= BIP_COPY_USER;
	bip->bip_iter.bi_sector = seed;
	bip->bip_vcnt = nr_vecs;
	return 0;
free_bip:
	bio_integrity_free(bio);
free_buf:
	kfree(buf);
	return ret;
}

static int bio_integrity_init_user(struct bio *bio, struct bio_vec *bvec,
				   int nr_vecs, unsigned int len, u32 seed)
{
	struct bio_integrity_payload *bip;

	bip = bio_integrity_alloc(bio, GFP_KERNEL, nr_vecs);
	if (IS_ERR(bip))
		return PTR_ERR(bip);

	memcpy(bip->bip_vec, bvec, nr_vecs * sizeof(*bvec));
	bip->bip_iter.bi_sector = seed;
	bip->bip_iter.bi_size = len;
	bip->bip_vcnt = nr_vecs;
	return 0;
}

static unsigned int bvec_from_pages(struct bio_vec *bvec, struct page **pages,
				    int nr_vecs, ssize_t bytes, ssize_t offset)
{
	unsigned int nr_bvecs = 0;
	int i, j;

	for (i = 0; i < nr_vecs; i = j) {
		size_t size = min_t(size_t, bytes, PAGE_SIZE - offset);
		struct folio *folio = page_folio(pages[i]);

		bytes -= size;
		for (j = i + 1; j < nr_vecs; j++) {
			size_t next = min_t(size_t, PAGE_SIZE, bytes);

			if (page_folio(pages[j]) != folio ||
			    pages[j] != pages[j - 1] + 1)
				break;
			unpin_user_page(pages[j]);
			size += next;
			bytes -= next;
		}

		bvec_set_page(&bvec[nr_bvecs], pages[i], size, offset);
		offset = 0;
		nr_bvecs++;
	}

	return nr_bvecs;
}

int bio_integrity_map_user(struct bio *bio, void __user *ubuf, ssize_t bytes,
			   u32 seed)
{
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);
	unsigned int align = blk_lim_dma_alignment_and_pad(&q->limits);
	struct page *stack_pages[UIO_FASTIOV], **pages = stack_pages;
	struct bio_vec stack_vec[UIO_FASTIOV], *bvec = stack_vec;
	unsigned int direction, nr_bvecs;
	struct iov_iter iter;
	int ret, nr_vecs;
	size_t offset;
	bool copy;

	if (bio_integrity(bio))
		return -EINVAL;
	if (bytes >> SECTOR_SHIFT > queue_max_hw_sectors(q))
		return -E2BIG;

	if (bio_data_dir(bio) == READ)
		direction = ITER_DEST;
	else
		direction = ITER_SOURCE;

	iov_iter_ubuf(&iter, direction, ubuf, bytes);
	nr_vecs = iov_iter_npages(&iter, BIO_MAX_VECS + 1);
	if (nr_vecs > BIO_MAX_VECS)
		return -E2BIG;
	if (nr_vecs > UIO_FASTIOV) {
		bvec = kcalloc(nr_vecs, sizeof(*bvec), GFP_KERNEL);
		if (!bvec)
			return -ENOMEM;
		pages = NULL;
	}

	copy = !iov_iter_is_aligned(&iter, align, align);
	ret = iov_iter_extract_pages(&iter, &pages, bytes, nr_vecs, 0, &offset);
	if (unlikely(ret < 0))
		goto free_bvec;

	nr_bvecs = bvec_from_pages(bvec, pages, nr_vecs, bytes, offset);
	if (pages != stack_pages)
		kvfree(pages);
	if (nr_bvecs > queue_max_integrity_segments(q))
		copy = true;

	if (copy)
		ret = bio_integrity_copy_user(bio, bvec, nr_bvecs, bytes,
					      direction, seed);
	else
		ret = bio_integrity_init_user(bio, bvec, nr_bvecs, bytes, seed);
	if (ret)
		goto release_pages;
	if (bvec != stack_vec)
		kfree(bvec);

	return 0;

release_pages:
	bio_integrity_unpin_bvec(bvec, nr_bvecs, false);
free_bvec:
	if (bvec != stack_vec)
		kfree(bvec);
	return ret;
}

/**
 * bio_integrity_prep - Prepare bio for integrity I/O
 * @bio:	bio to prepare
 *
 * Description:  Checks if the bio already has an integrity payload attached.
 * If it does, the payload has been generated by another kernel subsystem,
 * and we just pass it through. Otherwise allocates integrity payload.
 * The bio must have data direction, target device and start sector set priot
 * to calling.  In the WRITE case, integrity metadata will be generated using
 * the block device's integrity function.  In the READ case, the buffer
 * will be prepared for DMA and a suitable end_io handler set up.
 */
bool bio_integrity_prep(struct bio *bio)
{
	struct bio_integrity_payload *bip;
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	unsigned int len;
	void *buf;
	gfp_t gfp = GFP_NOIO;

	if (!bi)
		return true;

	if (!bio_sectors(bio))
		return true;

	/* Already protected? */
	if (bio_integrity(bio))
		return true;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		if (bi->flags & BLK_INTEGRITY_NOVERIFY)
			return true;
		break;
	case REQ_OP_WRITE:
		if (bi->flags & BLK_INTEGRITY_NOGENERATE)
			return true;

		/*
		 * Zero the memory allocated to not leak uninitialized kernel
		 * memory to disk for non-integrity metadata where nothing else
		 * initializes the memory.
		 */
		if (bi->csum_type == BLK_INTEGRITY_CSUM_NONE)
			gfp |= __GFP_ZERO;
		break;
	default:
		return true;
	}

	/* Allocate kernel buffer for protection data */
	len = bio_integrity_bytes(bi, bio_sectors(bio));
	buf = kmalloc(len, gfp);
	if (unlikely(buf == NULL)) {
		goto err_end_io;
	}

	bip = bio_integrity_alloc(bio, GFP_NOIO, 1);
	if (IS_ERR(bip)) {
		kfree(buf);
		goto err_end_io;
	}

	bip->bip_flags |= BIP_BLOCK_INTEGRITY;
	bip_set_seed(bip, bio->bi_iter.bi_sector);

	if (bi->csum_type == BLK_INTEGRITY_CSUM_IP)
		bip->bip_flags |= BIP_IP_CHECKSUM;

	if (bio_integrity_add_page(bio, virt_to_page(buf), len,
			offset_in_page(buf)) < len) {
		printk(KERN_ERR "could not attach integrity payload\n");
		goto err_end_io;
	}

	/* Auto-generate integrity metadata if this is a write */
	if (bio_data_dir(bio) == WRITE)
		blk_integrity_generate(bio);
	else
		bip->bio_iter = bio->bi_iter;
	return true;

err_end_io:
	bio->bi_status = BLK_STS_RESOURCE;
	bio_endio(bio);
	return false;
}
EXPORT_SYMBOL(bio_integrity_prep);

/**
 * bio_integrity_verify_fn - Integrity I/O completion worker
 * @work:	Work struct stored in bio to be verified
 *
 * Description: This workqueue function is called to complete a READ
 * request.  The function verifies the transferred integrity metadata
 * and then calls the original bio end_io function.
 */
static void bio_integrity_verify_fn(struct work_struct *work)
{
	struct bio_integrity_payload *bip =
		container_of(work, struct bio_integrity_payload, bip_work);
	struct bio *bio = bip->bip_bio;

	blk_integrity_verify(bio);

	kfree(bvec_virt(bip->bip_vec));
	bio_integrity_free(bio);
	bio_endio(bio);
}

/**
 * __bio_integrity_endio - Integrity I/O completion function
 * @bio:	Protected bio
 *
 * Description: Completion for integrity I/O
 *
 * Normally I/O completion is done in interrupt context.  However,
 * verifying I/O integrity is a time-consuming task which must be run
 * in process context.	This function postpones completion
 * accordingly.
 */
bool __bio_integrity_endio(struct bio *bio)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	struct bio_integrity_payload *bip = bio_integrity(bio);

	if (bio_op(bio) == REQ_OP_READ && !bio->bi_status && bi->csum_type) {
		INIT_WORK(&bip->bip_work, bio_integrity_verify_fn);
		queue_work(kintegrityd_wq, &bip->bip_work);
		return false;
	}

	kfree(bvec_virt(bip->bip_vec));
	bio_integrity_free(bio);
	return true;
}

/**
 * bio_integrity_advance - Advance integrity vector
 * @bio:	bio whose integrity vector to update
 * @bytes_done:	number of data bytes that have been completed
 *
 * Description: This function calculates how many integrity bytes the
 * number of completed data bytes correspond to and advances the
 * integrity vector accordingly.
 */
void bio_integrity_advance(struct bio *bio, unsigned int bytes_done)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	unsigned bytes = bio_integrity_bytes(bi, bytes_done >> 9);

	bip->bip_iter.bi_sector += bio_integrity_intervals(bi, bytes_done >> 9);
	bvec_iter_advance(bip->bip_vec, &bip->bip_iter, bytes);
}

/**
 * bio_integrity_trim - Trim integrity vector
 * @bio:	bio whose integrity vector to update
 *
 * Description: Used to trim the integrity vector in a cloned bio.
 */
void bio_integrity_trim(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	bip->bip_iter.bi_size = bio_integrity_bytes(bi, bio_sectors(bio));
}
EXPORT_SYMBOL(bio_integrity_trim);

/**
 * bio_integrity_clone - Callback for cloning bios with integrity metadata
 * @bio:	New bio
 * @bio_src:	Original bio
 * @gfp_mask:	Memory allocation mask
 *
 * Description:	Called to allocate a bip when cloning a bio
 */
int bio_integrity_clone(struct bio *bio, struct bio *bio_src,
			gfp_t gfp_mask)
{
	struct bio_integrity_payload *bip_src = bio_integrity(bio_src);
	struct bio_integrity_payload *bip;

	BUG_ON(bip_src == NULL);

	bip = bio_integrity_alloc(bio, gfp_mask, 0);
	if (IS_ERR(bip))
		return PTR_ERR(bip);

	bip->bip_vec = bip_src->bip_vec;
	bip->bip_iter = bip_src->bip_iter;
	bip->bip_flags = bip_src->bip_flags & ~BIP_BLOCK_INTEGRITY;

	return 0;
}

int bioset_integrity_create(struct bio_set *bs, int pool_size)
{
	if (mempool_initialized(&bs->bio_integrity_pool))
		return 0;

	if (mempool_init_slab_pool(&bs->bio_integrity_pool,
				   pool_size, bip_slab))
		return -1;

	if (biovec_init_pool(&bs->bvec_integrity_pool, pool_size)) {
		mempool_exit(&bs->bio_integrity_pool);
		return -1;
	}

	return 0;
}
EXPORT_SYMBOL(bioset_integrity_create);

void bioset_integrity_free(struct bio_set *bs)
{
	mempool_exit(&bs->bio_integrity_pool);
	mempool_exit(&bs->bvec_integrity_pool);
}

void __init bio_integrity_init(void)
{
	/*
	 * kintegrityd won't block much but may burn a lot of CPU cycles.
	 * Make it highpri CPU intensive wq with max concurrency of 1.
	 */
	kintegrityd_wq = alloc_workqueue("kintegrityd", WQ_MEM_RECLAIM |
					 WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);
	if (!kintegrityd_wq)
		panic("Failed to create kintegrityd\n");

	bip_slab = kmem_cache_create("bio_integrity_payload",
				     sizeof(struct bio_integrity_payload) +
				     sizeof(struct bio_vec) * BIO_INLINE_VECS,
				     0, SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);
}
