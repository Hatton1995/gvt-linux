/*
 * Copyright 2009 Jerome Glisse.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
/*
 * Authors:
 *    Jerome Glisse <glisse@freedesktop.org>
 *    Thomas Hellstrom <thomas-at-tungstengraphics-dot-com>
 *    Dave Airlie
 */
#include <ttm/ttm_bo_api.h>
#include <ttm/ttm_bo_driver.h>
#include <ttm/ttm_placement.h>
#include <ttm/ttm_module.h>
#include <ttm/ttm_page_alloc.h>
#include <drm/drmP.h>
#include <drm/amdgpu_drm.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/swiotlb.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/debugfs.h>
#include "amdgpu.h"
#include "bif/bif_4_1_d.h"

#define DRM_FILE_PAGE_OFFSET (0x100000000ULL >> PAGE_SHIFT)

static int amdgpu_ttm_debugfs_init(struct amdgpu_device *adev);
static void amdgpu_ttm_debugfs_fini(struct amdgpu_device *adev);


/*
 * Global memory.
 */
static int amdgpu_ttm_mem_global_init(struct drm_global_reference *ref)
{
	return ttm_mem_global_init(ref->object);
}

static void amdgpu_ttm_mem_global_release(struct drm_global_reference *ref)
{
	ttm_mem_global_release(ref->object);
}

static int amdgpu_ttm_global_init(struct amdgpu_device *adev)
{
	struct drm_global_reference *global_ref;
	struct amdgpu_ring *ring;
	struct amd_sched_rq *rq;
	int r;

	adev->mman.mem_global_referenced = false;
	global_ref = &adev->mman.mem_global_ref;
	global_ref->global_type = DRM_GLOBAL_TTM_MEM;
	global_ref->size = sizeof(struct ttm_mem_global);
	global_ref->init = &amdgpu_ttm_mem_global_init;
	global_ref->release = &amdgpu_ttm_mem_global_release;
	r = drm_global_item_ref(global_ref);
	if (r) {
		DRM_ERROR("Failed setting up TTM memory accounting "
			  "subsystem.\n");
		goto error_mem;
	}

	adev->mman.bo_global_ref.mem_glob =
		adev->mman.mem_global_ref.object;
	global_ref = &adev->mman.bo_global_ref.ref;
	global_ref->global_type = DRM_GLOBAL_TTM_BO;
	global_ref->size = sizeof(struct ttm_bo_global);
	global_ref->init = &ttm_bo_global_init;
	global_ref->release = &ttm_bo_global_release;
	r = drm_global_item_ref(global_ref);
	if (r) {
		DRM_ERROR("Failed setting up TTM BO subsystem.\n");
		goto error_bo;
	}

	ring = adev->mman.buffer_funcs_ring;
	rq = &ring->sched.sched_rq[AMD_SCHED_PRIORITY_KERNEL];
	r = amd_sched_entity_init(&ring->sched, &adev->mman.entity,
				  rq, amdgpu_sched_jobs);
	if (r) {
		DRM_ERROR("Failed setting up TTM BO move run queue.\n");
		goto error_entity;
	}

	adev->mman.mem_global_referenced = true;

	return 0;

error_entity:
	drm_global_item_unref(&adev->mman.bo_global_ref.ref);
error_bo:
	drm_global_item_unref(&adev->mman.mem_global_ref);
error_mem:
	return r;
}

static void amdgpu_ttm_global_fini(struct amdgpu_device *adev)
{
	if (adev->mman.mem_global_referenced) {
		amd_sched_entity_fini(adev->mman.entity.sched,
				      &adev->mman.entity);
		drm_global_item_unref(&adev->mman.bo_global_ref.ref);
		drm_global_item_unref(&adev->mman.mem_global_ref);
		adev->mman.mem_global_referenced = false;
	}
}

static int amdgpu_invalidate_caches(struct ttm_bo_device *bdev, uint32_t flags)
{
	return 0;
}

static int amdgpu_init_mem_type(struct ttm_bo_device *bdev, uint32_t type,
				struct ttm_mem_type_manager *man)
{
	struct amdgpu_device *adev;

	adev = amdgpu_ttm_adev(bdev);

	switch (type) {
	case TTM_PL_SYSTEM:
		/* System memory */
		man->flags = TTM_MEMTYPE_FLAG_MAPPABLE;
		man->available_caching = TTM_PL_MASK_CACHING;
		man->default_caching = TTM_PL_FLAG_CACHED;
		break;
	case TTM_PL_TT:
		man->func = &amdgpu_gtt_mgr_func;
		man->gpu_offset = adev->mc.gtt_start;
		man->available_caching = TTM_PL_MASK_CACHING;
		man->default_caching = TTM_PL_FLAG_CACHED;
		man->flags = TTM_MEMTYPE_FLAG_MAPPABLE | TTM_MEMTYPE_FLAG_CMA;
		break;
	case TTM_PL_VRAM:
		/* "On-card" video ram */
		man->func = &amdgpu_vram_mgr_func;
		man->gpu_offset = adev->mc.vram_start;
		man->flags = TTM_MEMTYPE_FLAG_FIXED |
			     TTM_MEMTYPE_FLAG_MAPPABLE;
		man->available_caching = TTM_PL_FLAG_UNCACHED | TTM_PL_FLAG_WC;
		man->default_caching = TTM_PL_FLAG_WC;
		break;
	case AMDGPU_PL_GDS:
	case AMDGPU_PL_GWS:
	case AMDGPU_PL_OA:
		/* On-chip GDS memory*/
		man->func = &ttm_bo_manager_func;
		man->gpu_offset = 0;
		man->flags = TTM_MEMTYPE_FLAG_FIXED | TTM_MEMTYPE_FLAG_CMA;
		man->available_caching = TTM_PL_FLAG_UNCACHED;
		man->default_caching = TTM_PL_FLAG_UNCACHED;
		break;
	default:
		DRM_ERROR("Unsupported memory type %u\n", (unsigned)type);
		return -EINVAL;
	}
	return 0;
}

static void amdgpu_evict_flags(struct ttm_buffer_object *bo,
				struct ttm_placement *placement)
{
	struct amdgpu_device *adev = amdgpu_ttm_adev(bo->bdev);
	struct amdgpu_bo *abo;
	static struct ttm_place placements = {
		.fpfn = 0,
		.lpfn = 0,
		.flags = TTM_PL_MASK_CACHING | TTM_PL_FLAG_SYSTEM
	};
	unsigned i;

	if (!amdgpu_ttm_bo_is_amdgpu_bo(bo)) {
		placement->placement = &placements;
		placement->busy_placement = &placements;
		placement->num_placement = 1;
		placement->num_busy_placement = 1;
		return;
	}
	abo = container_of(bo, struct amdgpu_bo, tbo);
	switch (bo->mem.mem_type) {
	case TTM_PL_VRAM:
		if (adev->mman.buffer_funcs_ring->ready == false) {
			amdgpu_ttm_placement_from_domain(abo, AMDGPU_GEM_DOMAIN_CPU);
		} else {
			amdgpu_ttm_placement_from_domain(abo, AMDGPU_GEM_DOMAIN_GTT);
			for (i = 0; i < abo->placement.num_placement; ++i) {
				if (!(abo->placements[i].flags &
				      TTM_PL_FLAG_TT))
					continue;

				if (abo->placements[i].lpfn)
					continue;

				/* set an upper limit to force directly
				 * allocating address space for the BO.
				 */
				abo->placements[i].lpfn =
					adev->mc.gtt_size >> PAGE_SHIFT;
			}
		}
		break;
	case TTM_PL_TT:
	default:
		amdgpu_ttm_placement_from_domain(abo, AMDGPU_GEM_DOMAIN_CPU);
	}
	*placement = abo->placement;
}

static int amdgpu_verify_access(struct ttm_buffer_object *bo, struct file *filp)
{
	struct amdgpu_bo *abo = container_of(bo, struct amdgpu_bo, tbo);

	if (amdgpu_ttm_tt_get_usermm(bo->ttm))
		return -EPERM;
	return drm_vma_node_verify_access(&abo->gem_base.vma_node,
					  filp->private_data);
}

static void amdgpu_move_null(struct ttm_buffer_object *bo,
			     struct ttm_mem_reg *new_mem)
{
	struct ttm_mem_reg *old_mem = &bo->mem;

	BUG_ON(old_mem->mm_node != NULL);
	*old_mem = *new_mem;
	new_mem->mm_node = NULL;
}

static int amdgpu_mm_node_addr(struct ttm_buffer_object *bo,
			       struct drm_mm_node *mm_node,
			       struct ttm_mem_reg *mem,
			       uint64_t *addr)
{
	int r;

	switch (mem->mem_type) {
	case TTM_PL_TT:
		r = amdgpu_ttm_bind(bo, mem);
		if (r)
			return r;

	case TTM_PL_VRAM:
		*addr = mm_node->start << PAGE_SHIFT;
		*addr += bo->bdev->man[mem->mem_type].gpu_offset;
		break;
	default:
		DRM_ERROR("Unknown placement %d\n", mem->mem_type);
		return -EINVAL;
	}

	return 0;
}

static int amdgpu_move_blit(struct ttm_buffer_object *bo,
			    bool evict, bool no_wait_gpu,
			    struct ttm_mem_reg *new_mem,
			    struct ttm_mem_reg *old_mem)
{
	struct amdgpu_device *adev = amdgpu_ttm_adev(bo->bdev);
	struct amdgpu_ring *ring = adev->mman.buffer_funcs_ring;

	struct drm_mm_node *old_mm, *new_mm;
	uint64_t old_start, old_size, new_start, new_size;
	unsigned long num_pages;
	struct dma_fence *fence = NULL;
	int r;

	BUILD_BUG_ON((PAGE_SIZE % AMDGPU_GPU_PAGE_SIZE) != 0);

	if (!ring->ready) {
		DRM_ERROR("Trying to move memory with ring turned off.\n");
		return -EINVAL;
	}

	old_mm = old_mem->mm_node;
	r = amdgpu_mm_node_addr(bo, old_mm, old_mem, &old_start);
	if (r)
		return r;
	old_size = old_mm->size;


	new_mm = new_mem->mm_node;
	r = amdgpu_mm_node_addr(bo, new_mm, new_mem, &new_start);
	if (r)
		return r;
	new_size = new_mm->size;

	num_pages = new_mem->num_pages;
	while (num_pages) {
		unsigned long cur_pages = min(old_size, new_size);
		struct dma_fence *next;

		r = amdgpu_copy_buffer(ring, old_start, new_start,
				       cur_pages * PAGE_SIZE,
				       bo->resv, &next, false);
		if (r)
			goto error;

		dma_fence_put(fence);
		fence = next;

		num_pages -= cur_pages;
		if (!num_pages)
			break;

		old_size -= cur_pages;
		if (!old_size) {
			r = amdgpu_mm_node_addr(bo, ++old_mm, old_mem,
						&old_start);
			if (r)
				goto error;
			old_size = old_mm->size;
		} else {
			old_start += cur_pages * PAGE_SIZE;
		}

		new_size -= cur_pages;
		if (!new_size) {
			r = amdgpu_mm_node_addr(bo, ++new_mm, new_mem,
						&new_start);
			if (r)
				goto error;

			new_size = new_mm->size;
		} else {
			new_start += cur_pages * PAGE_SIZE;
		}
	}

	r = ttm_bo_pipeline_move(bo, fence, evict, new_mem);
	dma_fence_put(fence);
	return r;

error:
	if (fence)
		dma_fence_wait(fence, false);
	dma_fence_put(fence);
	return r;
}

static int amdgpu_move_vram_ram(struct ttm_buffer_object *bo,
				bool evict, bool interruptible,
				bool no_wait_gpu,
				struct ttm_mem_reg *new_mem)
{
	struct amdgpu_device *adev;
	struct ttm_mem_reg *old_mem = &bo->mem;
	struct ttm_mem_reg tmp_mem;
	struct ttm_place placements;
	struct ttm_placement placement;
	int r;

	adev = amdgpu_ttm_adev(bo->bdev);
	tmp_mem = *new_mem;
	tmp_mem.mm_node = NULL;
	placement.num_placement = 1;
	placement.placement = &placements;
	placement.num_busy_placement = 1;
	placement.busy_placement = &placements;
	placements.fpfn = 0;
	placements.lpfn = adev->mc.gtt_size >> PAGE_SHIFT;
	placements.flags = TTM_PL_MASK_CACHING | TTM_PL_FLAG_TT;
	r = ttm_bo_mem_space(bo, &placement, &tmp_mem,
			     interruptible, no_wait_gpu);
	if (unlikely(r)) {
		return r;
	}

	r = ttm_tt_set_placement_caching(bo->ttm, tmp_mem.placement);
	if (unlikely(r)) {
		goto out_cleanup;
	}

	r = ttm_tt_bind(bo->ttm, &tmp_mem);
	if (unlikely(r)) {
		goto out_cleanup;
	}
	r = amdgpu_move_blit(bo, true, no_wait_gpu, &tmp_mem, old_mem);
	if (unlikely(r)) {
		goto out_cleanup;
	}
	r = ttm_bo_move_ttm(bo, interruptible, no_wait_gpu, new_mem);
out_cleanup:
	ttm_bo_mem_put(bo, &tmp_mem);
	return r;
}

static int amdgpu_move_ram_vram(struct ttm_buffer_object *bo,
				bool evict, bool interruptible,
				bool no_wait_gpu,
				struct ttm_mem_reg *new_mem)
{
	struct amdgpu_device *adev;
	struct ttm_mem_reg *old_mem = &bo->mem;
	struct ttm_mem_reg tmp_mem;
	struct ttm_placement placement;
	struct ttm_place placements;
	int r;

	adev = amdgpu_ttm_adev(bo->bdev);
	tmp_mem = *new_mem;
	tmp_mem.mm_node = NULL;
	placement.num_placement = 1;
	placement.placement = &placements;
	placement.num_busy_placement = 1;
	placement.busy_placement = &placements;
	placements.fpfn = 0;
	placements.lpfn = adev->mc.gtt_size >> PAGE_SHIFT;
	placements.flags = TTM_PL_MASK_CACHING | TTM_PL_FLAG_TT;
	r = ttm_bo_mem_space(bo, &placement, &tmp_mem,
			     interruptible, no_wait_gpu);
	if (unlikely(r)) {
		return r;
	}
	r = ttm_bo_move_ttm(bo, interruptible, no_wait_gpu, &tmp_mem);
	if (unlikely(r)) {
		goto out_cleanup;
	}
	r = amdgpu_move_blit(bo, true, no_wait_gpu, new_mem, old_mem);
	if (unlikely(r)) {
		goto out_cleanup;
	}
out_cleanup:
	ttm_bo_mem_put(bo, &tmp_mem);
	return r;
}

static int amdgpu_bo_move(struct ttm_buffer_object *bo,
			bool evict, bool interruptible,
			bool no_wait_gpu,
			struct ttm_mem_reg *new_mem)
{
	struct amdgpu_device *adev;
	struct amdgpu_bo *abo;
	struct ttm_mem_reg *old_mem = &bo->mem;
	int r;

	/* Can't move a pinned BO */
	abo = container_of(bo, struct amdgpu_bo, tbo);
	if (WARN_ON_ONCE(abo->pin_count > 0))
		return -EINVAL;

	adev = amdgpu_ttm_adev(bo->bdev);

	if (old_mem->mem_type == TTM_PL_SYSTEM && bo->ttm == NULL) {
		amdgpu_move_null(bo, new_mem);
		return 0;
	}
	if ((old_mem->mem_type == TTM_PL_TT &&
	     new_mem->mem_type == TTM_PL_SYSTEM) ||
	    (old_mem->mem_type == TTM_PL_SYSTEM &&
	     new_mem->mem_type == TTM_PL_TT)) {
		/* bind is enough */
		amdgpu_move_null(bo, new_mem);
		return 0;
	}
	if (adev->mman.buffer_funcs == NULL ||
	    adev->mman.buffer_funcs_ring == NULL ||
	    !adev->mman.buffer_funcs_ring->ready) {
		/* use memcpy */
		goto memcpy;
	}

	if (old_mem->mem_type == TTM_PL_VRAM &&
	    new_mem->mem_type == TTM_PL_SYSTEM) {
		r = amdgpu_move_vram_ram(bo, evict, interruptible,
					no_wait_gpu, new_mem);
	} else if (old_mem->mem_type == TTM_PL_SYSTEM &&
		   new_mem->mem_type == TTM_PL_VRAM) {
		r = amdgpu_move_ram_vram(bo, evict, interruptible,
					    no_wait_gpu, new_mem);
	} else {
		r = amdgpu_move_blit(bo, evict, no_wait_gpu, new_mem, old_mem);
	}

	if (r) {
memcpy:
		r = ttm_bo_move_memcpy(bo, interruptible, no_wait_gpu, new_mem);
		if (r) {
			return r;
		}
	}

	/* update statistics */
	atomic64_add((u64)bo->num_pages << PAGE_SHIFT, &adev->num_bytes_moved);
	return 0;
}

static int amdgpu_ttm_io_mem_reserve(struct ttm_bo_device *bdev, struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];
	struct amdgpu_device *adev = amdgpu_ttm_adev(bdev);

	mem->bus.addr = NULL;
	mem->bus.offset = 0;
	mem->bus.size = mem->num_pages << PAGE_SHIFT;
	mem->bus.base = 0;
	mem->bus.is_iomem = false;
	if (!(man->flags & TTM_MEMTYPE_FLAG_MAPPABLE))
		return -EINVAL;
	switch (mem->mem_type) {
	case TTM_PL_SYSTEM:
		/* system memory */
		return 0;
	case TTM_PL_TT:
		break;
	case TTM_PL_VRAM:
		if (mem->start == AMDGPU_BO_INVALID_OFFSET)
			return -EINVAL;

		mem->bus.offset = mem->start << PAGE_SHIFT;
		/* check if it's visible */
		if ((mem->bus.offset + mem->bus.size) > adev->mc.visible_vram_size)
			return -EINVAL;
		mem->bus.base = adev->mc.aper_base;
		mem->bus.is_iomem = true;
#ifdef __alpha__
		/*
		 * Alpha: use bus.addr to hold the ioremap() return,
		 * so we can modify bus.base below.
		 */
		if (mem->placement & TTM_PL_FLAG_WC)
			mem->bus.addr =
				ioremap_wc(mem->bus.base + mem->bus.offset,
					   mem->bus.size);
		else
			mem->bus.addr =
				ioremap_nocache(mem->bus.base + mem->bus.offset,
						mem->bus.size);
		if (!mem->bus.addr)
			return -ENOMEM;

		/*
		 * Alpha: Use just the bus offset plus
		 * the hose/domain memory base for bus.base.
		 * It then can be used to build PTEs for VRAM
		 * access, as done in ttm_bo_vm_fault().
		 */
		mem->bus.base = (mem->bus.base & 0x0ffffffffUL) +
			adev->ddev->hose->dense_mem_base;
#endif
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void amdgpu_ttm_io_mem_free(struct ttm_bo_device *bdev, struct ttm_mem_reg *mem)
{
}

/*
 * TTM backend functions.
 */
struct amdgpu_ttm_gup_task_list {
	struct list_head	list;
	struct task_struct	*task;
};

struct amdgpu_ttm_tt {
	struct ttm_dma_tt	ttm;
	struct amdgpu_device	*adev;
	u64			offset;
	uint64_t		userptr;
	struct mm_struct	*usermm;
	uint32_t		userflags;
	spinlock_t              guptasklock;
	struct list_head        guptasks;
	atomic_t		mmu_invalidations;
	struct list_head        list;
};

int amdgpu_ttm_tt_get_user_pages(struct ttm_tt *ttm, struct page **pages)
{
	struct amdgpu_ttm_tt *gtt = (void *)ttm;
	unsigned int flags = 0;
	unsigned pinned = 0;
	int r;

	if (!(gtt->userflags & AMDGPU_GEM_USERPTR_READONLY))
		flags |= FOLL_WRITE;

	if (gtt->userflags & AMDGPU_GEM_USERPTR_ANONONLY) {
		/* check that we only use anonymous memory
		   to prevent problems with writeback */
		unsigned long end = gtt->userptr + ttm->num_pages * PAGE_SIZE;
		struct vm_area_struct *vma;

		vma = find_vma(gtt->usermm, gtt->userptr);
		if (!vma || vma->vm_file || vma->vm_end < end)
			return -EPERM;
	}

	do {
		unsigned num_pages = ttm->num_pages - pinned;
		uint64_t userptr = gtt->userptr + pinned * PAGE_SIZE;
		struct page **p = pages + pinned;
		struct amdgpu_ttm_gup_task_list guptask;

		guptask.task = current;
		spin_lock(&gtt->guptasklock);
		list_add(&guptask.list, &gtt->guptasks);
		spin_unlock(&gtt->guptasklock);

		r = get_user_pages(userptr, num_pages, flags, p, NULL);

		spin_lock(&gtt->guptasklock);
		list_del(&guptask.list);
		spin_unlock(&gtt->guptasklock);

		if (r < 0)
			goto release_pages;

		pinned += r;

	} while (pinned < ttm->num_pages);

	return 0;

release_pages:
	release_pages(pages, pinned, 0);
	return r;
}

/* prepare the sg table with the user pages */
static int amdgpu_ttm_tt_pin_userptr(struct ttm_tt *ttm)
{
	struct amdgpu_device *adev = amdgpu_ttm_adev(ttm->bdev);
	struct amdgpu_ttm_tt *gtt = (void *)ttm;
	unsigned nents;
	int r;

	int write = !(gtt->userflags & AMDGPU_GEM_USERPTR_READONLY);
	enum dma_data_direction direction = write ?
		DMA_BIDIRECTIONAL : DMA_TO_DEVICE;

	r = sg_alloc_table_from_pages(ttm->sg, ttm->pages, ttm->num_pages, 0,
				      ttm->num_pages << PAGE_SHIFT,
				      GFP_KERNEL);
	if (r)
		goto release_sg;

	r = -ENOMEM;
	nents = dma_map_sg(adev->dev, ttm->sg->sgl, ttm->sg->nents, direction);
	if (nents != ttm->sg->nents)
		goto release_sg;

	drm_prime_sg_to_page_addr_arrays(ttm->sg, ttm->pages,
					 gtt->ttm.dma_address, ttm->num_pages);

	return 0;

release_sg:
	kfree(ttm->sg);
	return r;
}

static void amdgpu_ttm_tt_unpin_userptr(struct ttm_tt *ttm)
{
	struct amdgpu_device *adev = amdgpu_ttm_adev(ttm->bdev);
	struct amdgpu_ttm_tt *gtt = (void *)ttm;
	struct sg_page_iter sg_iter;

	int write = !(gtt->userflags & AMDGPU_GEM_USERPTR_READONLY);
	enum dma_data_direction direction = write ?
		DMA_BIDIRECTIONAL : DMA_TO_DEVICE;

	/* double check that we don't free the table twice */
	if (!ttm->sg->sgl)
		return;

	/* free the sg table and pages again */
	dma_unmap_sg(adev->dev, ttm->sg->sgl, ttm->sg->nents, direction);

	for_each_sg_page(ttm->sg->sgl, &sg_iter, ttm->sg->nents, 0) {
		struct page *page = sg_page_iter_page(&sg_iter);
		if (!(gtt->userflags & AMDGPU_GEM_USERPTR_READONLY))
			set_page_dirty(page);

		mark_page_accessed(page);
		put_page(page);
	}

	sg_free_table(ttm->sg);
}

static int amdgpu_ttm_backend_bind(struct ttm_tt *ttm,
				   struct ttm_mem_reg *bo_mem)
{
	struct amdgpu_ttm_tt *gtt = (void*)ttm;
	int r;

	if (gtt->userptr) {
		r = amdgpu_ttm_tt_pin_userptr(ttm);
		if (r) {
			DRM_ERROR("failed to pin userptr\n");
			return r;
		}
	}
	if (!ttm->num_pages) {
		WARN(1, "nothing to bind %lu pages for mreg %p back %p!\n",
		     ttm->num_pages, bo_mem, ttm);
	}

	if (bo_mem->mem_type == AMDGPU_PL_GDS ||
	    bo_mem->mem_type == AMDGPU_PL_GWS ||
	    bo_mem->mem_type == AMDGPU_PL_OA)
		return -EINVAL;

	return 0;
}

bool amdgpu_ttm_is_bound(struct ttm_tt *ttm)
{
	struct amdgpu_ttm_tt *gtt = (void *)ttm;

	return gtt && !list_empty(&gtt->list);
}

int amdgpu_ttm_bind(struct ttm_buffer_object *bo, struct ttm_mem_reg *bo_mem)
{
	struct ttm_tt *ttm = bo->ttm;
	struct amdgpu_ttm_tt *gtt = (void *)bo->ttm;
	uint64_t flags;
	int r;

	if (!ttm || amdgpu_ttm_is_bound(ttm))
		return 0;

	r = amdgpu_gtt_mgr_alloc(&bo->bdev->man[TTM_PL_TT], bo,
				 NULL, bo_mem);
	if (r) {
		DRM_ERROR("Failed to allocate GTT address space (%d)\n", r);
		return r;
	}

	flags = amdgpu_ttm_tt_pte_flags(gtt->adev, ttm, bo_mem);
	gtt->offset = (u64)bo_mem->start << PAGE_SHIFT;
	r = amdgpu_gart_bind(gtt->adev, gtt->offset, ttm->num_pages,
		ttm->pages, gtt->ttm.dma_address, flags);

	if (r) {
		DRM_ERROR("failed to bind %lu pages at 0x%08llX\n",
			  ttm->num_pages, gtt->offset);
		return r;
	}
	spin_lock(&gtt->adev->gtt_list_lock);
	list_add_tail(&gtt->list, &gtt->adev->gtt_list);
	spin_unlock(&gtt->adev->gtt_list_lock);
	return 0;
}

int amdgpu_ttm_recover_gart(struct amdgpu_device *adev)
{
	struct amdgpu_ttm_tt *gtt, *tmp;
	struct ttm_mem_reg bo_mem;
	uint32_t flags;
	int r;

	bo_mem.mem_type = TTM_PL_TT;
	spin_lock(&adev->gtt_list_lock);
	list_for_each_entry_safe(gtt, tmp, &adev->gtt_list, list) {
		flags = amdgpu_ttm_tt_pte_flags(gtt->adev, &gtt->ttm.ttm, &bo_mem);
		r = amdgpu_gart_bind(adev, gtt->offset, gtt->ttm.ttm.num_pages,
				     gtt->ttm.ttm.pages, gtt->ttm.dma_address,
				     flags);
		if (r) {
			spin_unlock(&adev->gtt_list_lock);
			DRM_ERROR("failed to bind %lu pages at 0x%08llX\n",
				  gtt->ttm.ttm.num_pages, gtt->offset);
			return r;
		}
	}
	spin_unlock(&adev->gtt_list_lock);
	return 0;
}

static int amdgpu_ttm_backend_unbind(struct ttm_tt *ttm)
{
	struct amdgpu_ttm_tt *gtt = (void *)ttm;

	if (gtt->userptr)
		amdgpu_ttm_tt_unpin_userptr(ttm);

	if (!amdgpu_ttm_is_bound(ttm))
		return 0;

	/* unbind shouldn't be done for GDS/GWS/OA in ttm_bo_clean_mm */
	if (gtt->adev->gart.ready)
		amdgpu_gart_unbind(gtt->adev, gtt->offset, ttm->num_pages);

	spin_lock(&gtt->adev->gtt_list_lock);
	list_del_init(&gtt->list);
	spin_unlock(&gtt->adev->gtt_list_lock);

	return 0;
}

static void amdgpu_ttm_backend_destroy(struct ttm_tt *ttm)
{
	struct amdgpu_ttm_tt *gtt = (void *)ttm;

	ttm_dma_tt_fini(&gtt->ttm);
	kfree(gtt);
}

static struct ttm_backend_func amdgpu_backend_func = {
	.bind = &amdgpu_ttm_backend_bind,
	.unbind = &amdgpu_ttm_backend_unbind,
	.destroy = &amdgpu_ttm_backend_destroy,
};

static struct ttm_tt *amdgpu_ttm_tt_create(struct ttm_bo_device *bdev,
				    unsigned long size, uint32_t page_flags,
				    struct page *dummy_read_page)
{
	struct amdgpu_device *adev;
	struct amdgpu_ttm_tt *gtt;

	adev = amdgpu_ttm_adev(bdev);

	gtt = kzalloc(sizeof(struct amdgpu_ttm_tt), GFP_KERNEL);
	if (gtt == NULL) {
		return NULL;
	}
	gtt->ttm.ttm.func = &amdgpu_backend_func;
	gtt->adev = adev;
	if (ttm_dma_tt_init(&gtt->ttm, bdev, size, page_flags, dummy_read_page)) {
		kfree(gtt);
		return NULL;
	}
	INIT_LIST_HEAD(&gtt->list);
	return &gtt->ttm.ttm;
}

static int amdgpu_ttm_tt_populate(struct ttm_tt *ttm)
{
	struct amdgpu_device *adev;
	struct amdgpu_ttm_tt *gtt = (void *)ttm;
	unsigned i;
	int r;
	bool slave = !!(ttm->page_flags & TTM_PAGE_FLAG_SG);

	if (ttm->state != tt_unpopulated)
		return 0;

	if (gtt && gtt->userptr) {
		ttm->sg = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
		if (!ttm->sg)
			return -ENOMEM;

		ttm->page_flags |= TTM_PAGE_FLAG_SG;
		ttm->state = tt_unbound;
		return 0;
	}

	if (slave && ttm->sg) {
		drm_prime_sg_to_page_addr_arrays(ttm->sg, ttm->pages,
						 gtt->ttm.dma_address, ttm->num_pages);
		ttm->state = tt_unbound;
		return 0;
	}

	adev = amdgpu_ttm_adev(ttm->bdev);

#ifdef CONFIG_SWIOTLB
	if (swiotlb_nr_tbl()) {
		return ttm_dma_populate(&gtt->ttm, adev->dev);
	}
#endif

	r = ttm_pool_populate(ttm);
	if (r) {
		return r;
	}

	for (i = 0; i < ttm->num_pages; i++) {
		gtt->ttm.dma_address[i] = pci_map_page(adev->pdev, ttm->pages[i],
						       0, PAGE_SIZE,
						       PCI_DMA_BIDIRECTIONAL);
		if (pci_dma_mapping_error(adev->pdev, gtt->ttm.dma_address[i])) {
			while (i--) {
				pci_unmap_page(adev->pdev, gtt->ttm.dma_address[i],
					       PAGE_SIZE, PCI_DMA_BIDIRECTIONAL);
				gtt->ttm.dma_address[i] = 0;
			}
			ttm_pool_unpopulate(ttm);
			return -EFAULT;
		}
	}
	return 0;
}

static void amdgpu_ttm_tt_unpopulate(struct ttm_tt *ttm)
{
	struct amdgpu_device *adev;
	struct amdgpu_ttm_tt *gtt = (void *)ttm;
	unsigned i;
	bool slave = !!(ttm->page_flags & TTM_PAGE_FLAG_SG);

	if (gtt && gtt->userptr) {
		kfree(ttm->sg);
		ttm->page_flags &= ~TTM_PAGE_FLAG_SG;
		return;
	}

	if (slave)
		return;

	adev = amdgpu_ttm_adev(ttm->bdev);

#ifdef CONFIG_SWIOTLB
	if (swiotlb_nr_tbl()) {
		ttm_dma_unpopulate(&gtt->ttm, adev->dev);
		return;
	}
#endif

	for (i = 0; i < ttm->num_pages; i++) {
		if (gtt->ttm.dma_address[i]) {
			pci_unmap_page(adev->pdev, gtt->ttm.dma_address[i],
				       PAGE_SIZE, PCI_DMA_BIDIRECTIONAL);
		}
	}

	ttm_pool_unpopulate(ttm);
}

int amdgpu_ttm_tt_set_userptr(struct ttm_tt *ttm, uint64_t addr,
			      uint32_t flags)
{
	struct amdgpu_ttm_tt *gtt = (void *)ttm;

	if (gtt == NULL)
		return -EINVAL;

	gtt->userptr = addr;
	gtt->usermm = current->mm;
	gtt->userflags = flags;
	spin_lock_init(&gtt->guptasklock);
	INIT_LIST_HEAD(&gtt->guptasks);
	atomic_set(&gtt->mmu_invalidations, 0);

	return 0;
}

struct mm_struct *amdgpu_ttm_tt_get_usermm(struct ttm_tt *ttm)
{
	struct amdgpu_ttm_tt *gtt = (void *)ttm;

	if (gtt == NULL)
		return NULL;

	return gtt->usermm;
}

bool amdgpu_ttm_tt_affect_userptr(struct ttm_tt *ttm, unsigned long start,
				  unsigned long end)
{
	struct amdgpu_ttm_tt *gtt = (void *)ttm;
	struct amdgpu_ttm_gup_task_list *entry;
	unsigned long size;

	if (gtt == NULL || !gtt->userptr)
		return false;

	size = (unsigned long)gtt->ttm.ttm.num_pages * PAGE_SIZE;
	if (gtt->userptr > end || gtt->userptr + size <= start)
		return false;

	spin_lock(&gtt->guptasklock);
	list_for_each_entry(entry, &gtt->guptasks, list) {
		if (entry->task == current) {
			spin_unlock(&gtt->guptasklock);
			return false;
		}
	}
	spin_unlock(&gtt->guptasklock);

	atomic_inc(&gtt->mmu_invalidations);

	return true;
}

bool amdgpu_ttm_tt_userptr_invalidated(struct ttm_tt *ttm,
				       int *last_invalidated)
{
	struct amdgpu_ttm_tt *gtt = (void *)ttm;
	int prev_invalidated = *last_invalidated;

	*last_invalidated = atomic_read(&gtt->mmu_invalidations);
	return prev_invalidated != *last_invalidated;
}

bool amdgpu_ttm_tt_is_readonly(struct ttm_tt *ttm)
{
	struct amdgpu_ttm_tt *gtt = (void *)ttm;

	if (gtt == NULL)
		return false;

	return !!(gtt->userflags & AMDGPU_GEM_USERPTR_READONLY);
}

uint64_t amdgpu_ttm_tt_pte_flags(struct amdgpu_device *adev, struct ttm_tt *ttm,
				 struct ttm_mem_reg *mem)
{
	uint64_t flags = 0;

	if (mem && mem->mem_type != TTM_PL_SYSTEM)
		flags |= AMDGPU_PTE_VALID;

	if (mem && mem->mem_type == TTM_PL_TT) {
		flags |= AMDGPU_PTE_SYSTEM;

		if (ttm->caching_state == tt_cached)
			flags |= AMDGPU_PTE_SNOOPED;
	}

	flags |= adev->gart.gart_pte_flags;
	flags |= AMDGPU_PTE_READABLE;

	if (!amdgpu_ttm_tt_is_readonly(ttm))
		flags |= AMDGPU_PTE_WRITEABLE;

	return flags;
}

static bool amdgpu_ttm_bo_eviction_valuable(struct ttm_buffer_object *bo,
					    const struct ttm_place *place)
{
	if (bo->mem.mem_type == TTM_PL_VRAM &&
	    bo->mem.start == AMDGPU_BO_INVALID_OFFSET) {
		unsigned long num_pages = bo->mem.num_pages;
		struct drm_mm_node *node = bo->mem.mm_node;

		/* Check each drm MM node individually */
		while (num_pages) {
			if (place->fpfn < (node->start + node->size) &&
			    !(place->lpfn && place->lpfn <= node->start))
				return true;

			num_pages -= node->size;
			++node;
		}

		return false;
	}

	return ttm_bo_eviction_valuable(bo, place);
}

static struct ttm_bo_driver amdgpu_bo_driver = {
	.ttm_tt_create = &amdgpu_ttm_tt_create,
	.ttm_tt_populate = &amdgpu_ttm_tt_populate,
	.ttm_tt_unpopulate = &amdgpu_ttm_tt_unpopulate,
	.invalidate_caches = &amdgpu_invalidate_caches,
	.init_mem_type = &amdgpu_init_mem_type,
	.eviction_valuable = amdgpu_ttm_bo_eviction_valuable,
	.evict_flags = &amdgpu_evict_flags,
	.move = &amdgpu_bo_move,
	.verify_access = &amdgpu_verify_access,
	.move_notify = &amdgpu_bo_move_notify,
	.fault_reserve_notify = &amdgpu_bo_fault_reserve_notify,
	.io_mem_reserve = &amdgpu_ttm_io_mem_reserve,
	.io_mem_free = &amdgpu_ttm_io_mem_free,
};

int amdgpu_ttm_init(struct amdgpu_device *adev)
{
	int r;

	r = amdgpu_ttm_global_init(adev);
	if (r) {
		return r;
	}
	/* No others user of address space so set it to 0 */
	r = ttm_bo_device_init(&adev->mman.bdev,
			       adev->mman.bo_global_ref.ref.object,
			       &amdgpu_bo_driver,
			       adev->ddev->anon_inode->i_mapping,
			       DRM_FILE_PAGE_OFFSET,
			       adev->need_dma32);
	if (r) {
		DRM_ERROR("failed initializing buffer object driver(%d).\n", r);
		return r;
	}
	adev->mman.initialized = true;
	r = ttm_bo_init_mm(&adev->mman.bdev, TTM_PL_VRAM,
				adev->mc.real_vram_size >> PAGE_SHIFT);
	if (r) {
		DRM_ERROR("Failed initializing VRAM heap.\n");
		return r;
	}
	/* Change the size here instead of the init above so only lpfn is affected */
	amdgpu_ttm_set_active_vram_size(adev, adev->mc.visible_vram_size);

	r = amdgpu_bo_create(adev, 256 * 1024, PAGE_SIZE, true,
			     AMDGPU_GEM_DOMAIN_VRAM,
			     AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
			     AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS,
			     NULL, NULL, &adev->stollen_vga_memory);
	if (r) {
		return r;
	}
	r = amdgpu_bo_reserve(adev->stollen_vga_memory, false);
	if (r)
		return r;
	r = amdgpu_bo_pin(adev->stollen_vga_memory, AMDGPU_GEM_DOMAIN_VRAM, NULL);
	amdgpu_bo_unreserve(adev->stollen_vga_memory);
	if (r) {
		amdgpu_bo_unref(&adev->stollen_vga_memory);
		return r;
	}
	DRM_INFO("amdgpu: %uM of VRAM memory ready\n",
		 (unsigned) (adev->mc.real_vram_size / (1024 * 1024)));
	r = ttm_bo_init_mm(&adev->mman.bdev, TTM_PL_TT,
				adev->mc.gtt_size >> PAGE_SHIFT);
	if (r) {
		DRM_ERROR("Failed initializing GTT heap.\n");
		return r;
	}
	DRM_INFO("amdgpu: %uM of GTT memory ready.\n",
		 (unsigned)(adev->mc.gtt_size / (1024 * 1024)));

	adev->gds.mem.total_size = adev->gds.mem.total_size << AMDGPU_GDS_SHIFT;
	adev->gds.mem.gfx_partition_size = adev->gds.mem.gfx_partition_size << AMDGPU_GDS_SHIFT;
	adev->gds.mem.cs_partition_size = adev->gds.mem.cs_partition_size << AMDGPU_GDS_SHIFT;
	adev->gds.gws.total_size = adev->gds.gws.total_size << AMDGPU_GWS_SHIFT;
	adev->gds.gws.gfx_partition_size = adev->gds.gws.gfx_partition_size << AMDGPU_GWS_SHIFT;
	adev->gds.gws.cs_partition_size = adev->gds.gws.cs_partition_size << AMDGPU_GWS_SHIFT;
	adev->gds.oa.total_size = adev->gds.oa.total_size << AMDGPU_OA_SHIFT;
	adev->gds.oa.gfx_partition_size = adev->gds.oa.gfx_partition_size << AMDGPU_OA_SHIFT;
	adev->gds.oa.cs_partition_size = adev->gds.oa.cs_partition_size << AMDGPU_OA_SHIFT;
	/* GDS Memory */
	if (adev->gds.mem.total_size) {
		r = ttm_bo_init_mm(&adev->mman.bdev, AMDGPU_PL_GDS,
				   adev->gds.mem.total_size >> PAGE_SHIFT);
		if (r) {
			DRM_ERROR("Failed initializing GDS heap.\n");
			return r;
		}
	}

	/* GWS */
	if (adev->gds.gws.total_size) {
		r = ttm_bo_init_mm(&adev->mman.bdev, AMDGPU_PL_GWS,
				   adev->gds.gws.total_size >> PAGE_SHIFT);
		if (r) {
			DRM_ERROR("Failed initializing gws heap.\n");
			return r;
		}
	}

	/* OA */
	if (adev->gds.oa.total_size) {
		r = ttm_bo_init_mm(&adev->mman.bdev, AMDGPU_PL_OA,
				   adev->gds.oa.total_size >> PAGE_SHIFT);
		if (r) {
			DRM_ERROR("Failed initializing oa heap.\n");
			return r;
		}
	}

	r = amdgpu_ttm_debugfs_init(adev);
	if (r) {
		DRM_ERROR("Failed to init debugfs\n");
		return r;
	}
	return 0;
}

void amdgpu_ttm_fini(struct amdgpu_device *adev)
{
	int r;

	if (!adev->mman.initialized)
		return;
	amdgpu_ttm_debugfs_fini(adev);
	if (adev->stollen_vga_memory) {
		r = amdgpu_bo_reserve(adev->stollen_vga_memory, false);
		if (r == 0) {
			amdgpu_bo_unpin(adev->stollen_vga_memory);
			amdgpu_bo_unreserve(adev->stollen_vga_memory);
		}
		amdgpu_bo_unref(&adev->stollen_vga_memory);
	}
	ttm_bo_clean_mm(&adev->mman.bdev, TTM_PL_VRAM);
	ttm_bo_clean_mm(&adev->mman.bdev, TTM_PL_TT);
	if (adev->gds.mem.total_size)
		ttm_bo_clean_mm(&adev->mman.bdev, AMDGPU_PL_GDS);
	if (adev->gds.gws.total_size)
		ttm_bo_clean_mm(&adev->mman.bdev, AMDGPU_PL_GWS);
	if (adev->gds.oa.total_size)
		ttm_bo_clean_mm(&adev->mman.bdev, AMDGPU_PL_OA);
	ttm_bo_device_release(&adev->mman.bdev);
	amdgpu_gart_fini(adev);
	amdgpu_ttm_global_fini(adev);
	adev->mman.initialized = false;
	DRM_INFO("amdgpu: ttm finalized\n");
}

/* this should only be called at bootup or when userspace
 * isn't running */
void amdgpu_ttm_set_active_vram_size(struct amdgpu_device *adev, u64 size)
{
	struct ttm_mem_type_manager *man;

	if (!adev->mman.initialized)
		return;

	man = &adev->mman.bdev.man[TTM_PL_VRAM];
	/* this just adjusts TTM size idea, which sets lpfn to the correct value */
	man->size = size >> PAGE_SHIFT;
}

int amdgpu_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *file_priv;
	struct amdgpu_device *adev;

	if (unlikely(vma->vm_pgoff < DRM_FILE_PAGE_OFFSET))
		return -EINVAL;

	file_priv = filp->private_data;
	adev = file_priv->minor->dev->dev_private;
	if (adev == NULL)
		return -EINVAL;

	return ttm_bo_mmap(filp, vma, &adev->mman.bdev);
}

int amdgpu_copy_buffer(struct amdgpu_ring *ring,
		       uint64_t src_offset,
		       uint64_t dst_offset,
		       uint32_t byte_count,
		       struct reservation_object *resv,
		       struct dma_fence **fence, bool direct_submit)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_job *job;

	uint32_t max_bytes;
	unsigned num_loops, num_dw;
	unsigned i;
	int r;

	max_bytes = adev->mman.buffer_funcs->copy_max_bytes;
	num_loops = DIV_ROUND_UP(byte_count, max_bytes);
	num_dw = num_loops * adev->mman.buffer_funcs->copy_num_dw;

	/* for IB padding */
	while (num_dw & 0x7)
		num_dw++;

	r = amdgpu_job_alloc_with_ib(adev, num_dw * 4, &job);
	if (r)
		return r;

	if (resv) {
		r = amdgpu_sync_resv(adev, &job->sync, resv,
				     AMDGPU_FENCE_OWNER_UNDEFINED);
		if (r) {
			DRM_ERROR("sync failed (%d).\n", r);
			goto error_free;
		}
	}

	for (i = 0; i < num_loops; i++) {
		uint32_t cur_size_in_bytes = min(byte_count, max_bytes);

		amdgpu_emit_copy_buffer(adev, &job->ibs[0], src_offset,
					dst_offset, cur_size_in_bytes);

		src_offset += cur_size_in_bytes;
		dst_offset += cur_size_in_bytes;
		byte_count -= cur_size_in_bytes;
	}

	amdgpu_ring_pad_ib(ring, &job->ibs[0]);
	WARN_ON(job->ibs[0].length_dw > num_dw);
	if (direct_submit) {
		r = amdgpu_ib_schedule(ring, job->num_ibs, job->ibs,
				       NULL, fence);
		job->fence = dma_fence_get(*fence);
		if (r)
			DRM_ERROR("Error scheduling IBs (%d)\n", r);
		amdgpu_job_free(job);
	} else {
		r = amdgpu_job_submit(job, ring, &adev->mman.entity,
				      AMDGPU_FENCE_OWNER_UNDEFINED, fence);
		if (r)
			goto error_free;
	}

	return r;

error_free:
	amdgpu_job_free(job);
	return r;
}

int amdgpu_fill_buffer(struct amdgpu_bo *bo,
		       uint32_t src_data,
		       struct reservation_object *resv,
		       struct dma_fence **fence)
{
	struct amdgpu_device *adev = amdgpu_ttm_adev(bo->tbo.bdev);
	uint32_t max_bytes = adev->mman.buffer_funcs->fill_max_bytes;
	struct amdgpu_ring *ring = adev->mman.buffer_funcs_ring;

	struct drm_mm_node *mm_node;
	unsigned long num_pages;
	unsigned int num_loops, num_dw;

	struct amdgpu_job *job;
	int r;

	if (!ring->ready) {
		DRM_ERROR("Trying to clear memory with ring turned off.\n");
		return -EINVAL;
	}

	num_pages = bo->tbo.num_pages;
	mm_node = bo->tbo.mem.mm_node;
	num_loops = 0;
	while (num_pages) {
		uint32_t byte_count = mm_node->size << PAGE_SHIFT;

		num_loops += DIV_ROUND_UP(byte_count, max_bytes);
		num_pages -= mm_node->size;
		++mm_node;
	}
	num_dw = num_loops * adev->mman.buffer_funcs->fill_num_dw;

	/* for IB padding */
	num_dw += 64;

	r = amdgpu_job_alloc_with_ib(adev, num_dw * 4, &job);
	if (r)
		return r;

	if (resv) {
		r = amdgpu_sync_resv(adev, &job->sync, resv,
				     AMDGPU_FENCE_OWNER_UNDEFINED);
		if (r) {
			DRM_ERROR("sync failed (%d).\n", r);
			goto error_free;
		}
	}

	num_pages = bo->tbo.num_pages;
	mm_node = bo->tbo.mem.mm_node;

	while (num_pages) {
		uint32_t byte_count = mm_node->size << PAGE_SHIFT;
		uint64_t dst_addr;

		r = amdgpu_mm_node_addr(&bo->tbo, mm_node,
					&bo->tbo.mem, &dst_addr);
		if (r)
			return r;

		while (byte_count) {
			uint32_t cur_size_in_bytes = min(byte_count, max_bytes);

			amdgpu_emit_fill_buffer(adev, &job->ibs[0], src_data,
						dst_addr, cur_size_in_bytes);

			dst_addr += cur_size_in_bytes;
			byte_count -= cur_size_in_bytes;
		}

		num_pages -= mm_node->size;
		++mm_node;
	}

	amdgpu_ring_pad_ib(ring, &job->ibs[0]);
	WARN_ON(job->ibs[0].length_dw > num_dw);
	r = amdgpu_job_submit(job, ring, &adev->mman.entity,
			      AMDGPU_FENCE_OWNER_UNDEFINED, fence);
	if (r)
		goto error_free;

	return 0;

error_free:
	amdgpu_job_free(job);
	return r;
}

#if defined(CONFIG_DEBUG_FS)

static int amdgpu_mm_dump_table(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	unsigned ttm_pl = *(int *)node->info_ent->data;
	struct drm_device *dev = node->minor->dev;
	struct amdgpu_device *adev = dev->dev_private;
	struct drm_mm *mm = (struct drm_mm *)adev->mman.bdev.man[ttm_pl].priv;
	struct ttm_bo_global *glob = adev->mman.bdev.glob;
	struct drm_printer p = drm_seq_file_printer(m);

	spin_lock(&glob->lru_lock);
	drm_mm_print(mm, &p);
	spin_unlock(&glob->lru_lock);
	if (ttm_pl == TTM_PL_VRAM)
		seq_printf(m, "man size:%llu pages, ram usage:%lluMB, vis usage:%lluMB\n",
			   adev->mman.bdev.man[ttm_pl].size,
			   (u64)atomic64_read(&adev->vram_usage) >> 20,
			   (u64)atomic64_read(&adev->vram_vis_usage) >> 20);
	return 0;
}

static int ttm_pl_vram = TTM_PL_VRAM;
static int ttm_pl_tt = TTM_PL_TT;

static const struct drm_info_list amdgpu_ttm_debugfs_list[] = {
	{"amdgpu_vram_mm", amdgpu_mm_dump_table, 0, &ttm_pl_vram},
	{"amdgpu_gtt_mm", amdgpu_mm_dump_table, 0, &ttm_pl_tt},
	{"ttm_page_pool", ttm_page_alloc_debugfs, 0, NULL},
#ifdef CONFIG_SWIOTLB
	{"ttm_dma_page_pool", ttm_dma_page_alloc_debugfs, 0, NULL}
#endif
};

static ssize_t amdgpu_ttm_vram_read(struct file *f, char __user *buf,
				    size_t size, loff_t *pos)
{
	struct amdgpu_device *adev = file_inode(f)->i_private;
	ssize_t result = 0;
	int r;

	if (size & 0x3 || *pos & 0x3)
		return -EINVAL;

	while (size) {
		unsigned long flags;
		uint32_t value;

		if (*pos >= adev->mc.mc_vram_size)
			return result;

		spin_lock_irqsave(&adev->mmio_idx_lock, flags);
		WREG32(mmMM_INDEX, ((uint32_t)*pos) | 0x80000000);
		WREG32(mmMM_INDEX_HI, *pos >> 31);
		value = RREG32(mmMM_DATA);
		spin_unlock_irqrestore(&adev->mmio_idx_lock, flags);

		r = put_user(value, (uint32_t *)buf);
		if (r)
			return r;

		result += 4;
		buf += 4;
		*pos += 4;
		size -= 4;
	}

	return result;
}

static const struct file_operations amdgpu_ttm_vram_fops = {
	.owner = THIS_MODULE,
	.read = amdgpu_ttm_vram_read,
	.llseek = default_llseek
};

#ifdef CONFIG_DRM_AMDGPU_GART_DEBUGFS

static ssize_t amdgpu_ttm_gtt_read(struct file *f, char __user *buf,
				   size_t size, loff_t *pos)
{
	struct amdgpu_device *adev = file_inode(f)->i_private;
	ssize_t result = 0;
	int r;

	while (size) {
		loff_t p = *pos / PAGE_SIZE;
		unsigned off = *pos & ~PAGE_MASK;
		size_t cur_size = min_t(size_t, size, PAGE_SIZE - off);
		struct page *page;
		void *ptr;

		if (p >= adev->gart.num_cpu_pages)
			return result;

		page = adev->gart.pages[p];
		if (page) {
			ptr = kmap(page);
			ptr += off;

			r = copy_to_user(buf, ptr, cur_size);
			kunmap(adev->gart.pages[p]);
		} else
			r = clear_user(buf, cur_size);

		if (r)
			return -EFAULT;

		result += cur_size;
		buf += cur_size;
		*pos += cur_size;
		size -= cur_size;
	}

	return result;
}

static const struct file_operations amdgpu_ttm_gtt_fops = {
	.owner = THIS_MODULE,
	.read = amdgpu_ttm_gtt_read,
	.llseek = default_llseek
};

#endif

#endif

static int amdgpu_ttm_debugfs_init(struct amdgpu_device *adev)
{
#if defined(CONFIG_DEBUG_FS)
	unsigned count;

	struct drm_minor *minor = adev->ddev->primary;
	struct dentry *ent, *root = minor->debugfs_root;

	ent = debugfs_create_file("amdgpu_vram", S_IFREG | S_IRUGO, root,
				  adev, &amdgpu_ttm_vram_fops);
	if (IS_ERR(ent))
		return PTR_ERR(ent);
	i_size_write(ent->d_inode, adev->mc.mc_vram_size);
	adev->mman.vram = ent;

#ifdef CONFIG_DRM_AMDGPU_GART_DEBUGFS
	ent = debugfs_create_file("amdgpu_gtt", S_IFREG | S_IRUGO, root,
				  adev, &amdgpu_ttm_gtt_fops);
	if (IS_ERR(ent))
		return PTR_ERR(ent);
	i_size_write(ent->d_inode, adev->mc.gtt_size);
	adev->mman.gtt = ent;

#endif
	count = ARRAY_SIZE(amdgpu_ttm_debugfs_list);

#ifdef CONFIG_SWIOTLB
	if (!swiotlb_nr_tbl())
		--count;
#endif

	return amdgpu_debugfs_add_files(adev, amdgpu_ttm_debugfs_list, count);
#else

	return 0;
#endif
}

static void amdgpu_ttm_debugfs_fini(struct amdgpu_device *adev)
{
#if defined(CONFIG_DEBUG_FS)

	debugfs_remove(adev->mman.vram);
	adev->mman.vram = NULL;

#ifdef CONFIG_DRM_AMDGPU_GART_DEBUGFS
	debugfs_remove(adev->mman.gtt);
	adev->mman.gtt = NULL;
#endif

#endif
}
