/*
 * Copyright (c) 1998 Matthew Dillon,
 * Copyright (c) 1994 John S. Dyson
 * Copyright (c) 1990 University of Utah.
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *				New Swap System
 *				Matthew Dillon
 *
 * Radix Bitmap 'blists'.
 *
 *	- The new swapper uses the new radix bitmap code.  This should scale
 *	  to arbitrarily small or arbitrarily large swap spaces and an almost
 *	  arbitrary degree of fragmentation.
 *
 * Features:
 *
 *	- on the fly reallocation of swap during putpages.  The new system
 *	  does not try to keep previously allocated swap blocks for dirty
 *	  pages.  
 *
 *	- on the fly deallocation of swap
 *
 *	- No more garbage collection required.  Unnecessarily allocated swap
 *	  blocks only exist for dirty vm_page_t's now and these are already
 *	  cycled (in a high-load system) by the pager.  We also do on-the-fly
 *	  removal of invalidated swap blocks when a page is destroyed
 *	  or renamed.
 *
 * from: Utah $Hdr: swap_pager.c 1.4 91/04/30$
 *
 *	@(#)swap_pager.c	8.9 (Berkeley) 3/21/94
 *	@(#)vm_swap.c	8.5 (Berkeley) 2/17/94
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_mac.h"
#include "opt_swap.h"
#include "opt_vm.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/mac.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/sysproto.h>
#include <sys/blist.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/vmmeter.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_pageout.h>
#include <vm/vm_param.h>
#include <vm/swap_pager.h>
#include <vm/vm_extern.h>
#include <vm/uma.h>

#ifndef NSWAPDEV
#define NSWAPDEV	4
#endif

/*
 * SWB_NPAGES must be a power of 2.  It may be set to 1, 2, 4, 8, or 16
 * pages per allocation.  We recommend you stick with the default of 8.
 * The 16-page limit is due to the radix code (kern/subr_blist.c).
 */
#ifndef MAX_PAGEOUT_CLUSTER
#define MAX_PAGEOUT_CLUSTER 16
#endif

#if !defined(SWB_NPAGES)
#define SWB_NPAGES	MAX_PAGEOUT_CLUSTER
#endif

/*
 * Piecemeal swap metadata structure.  Swap is stored in a radix tree.
 *
 * If SWB_NPAGES is 8 and sizeof(char *) == sizeof(daddr_t), our radix
 * is basically 8.  Assuming PAGE_SIZE == 4096, one tree level represents
 * 32K worth of data, two levels represent 256K, three levels represent
 * 2 MBytes.   This is acceptable.
 *
 * Overall memory utilization is about the same as the old swap structure.
 */
#define SWCORRECT(n) (sizeof(void *) * (n) / sizeof(daddr_t))
#define SWAP_META_PAGES		(SWB_NPAGES * 2)
#define SWAP_META_MASK		(SWAP_META_PAGES - 1)

typedef	int32_t	swblk_t;	/* swap offset */

struct swblock {
	struct swblock	*swb_hnext;
	vm_object_t	swb_object;
	vm_pindex_t	swb_index;
	int		swb_count;
	daddr_t		swb_pages[SWAP_META_PAGES];
};

static struct swdevt swdevt[NSWAPDEV];
static int nswap;		/* first block after the interleaved devs */
int vm_swap_size;
static int swdev_syscall_active = 0; /* serialize swap(on|off) */

static int swapdev_strategy(struct vop_strategy_args *ap);
static struct vnode *swapdev_vp;

#define SWM_FREE	0x02	/* free, period			*/
#define SWM_POP		0x04	/* pop out			*/

int swap_pager_full;		/* swap space exhaustion (task killing) */
static int swap_pager_almost_full; /* swap space exhaustion (w/ hysteresis)*/
static int nsw_rcount;		/* free read buffers			*/
static int nsw_wcount_sync;	/* limit write buffers / synchronous	*/
static int nsw_wcount_async;	/* limit write buffers / asynchronous	*/
static int nsw_wcount_async_max;/* assigned maximum			*/
static int nsw_cluster_max;	/* maximum VOP I/O allowed		*/

static struct blist *swapblist;
static struct swblock **swhash;
static int swhash_mask;
static int swap_async_max = 4;	/* maximum in-progress async I/O's	*/
static struct sx sw_alloc_sx;


SYSCTL_INT(_vm, OID_AUTO, swap_async_max,
        CTLFLAG_RW, &swap_async_max, 0, "Maximum running async swap ops");

#define BLK2DEVIDX(blk) (NSWAPDEV > 1 ? blk / dmmax % NSWAPDEV : 0)

/*
 * "named" and "unnamed" anon region objects.  Try to reduce the overhead
 * of searching a named list by hashing it just a little.
 */

#define NOBJLISTS		8

#define NOBJLIST(handle)	\
	(&swap_pager_object_list[((int)(intptr_t)handle >> 4) & (NOBJLISTS-1)])

static struct mtx sw_alloc_mtx;	/* protect list manipulation */ 
static struct pagerlst	swap_pager_object_list[NOBJLISTS];
static struct pagerlst	swap_pager_un_object_list;
static uma_zone_t	swap_zone;

/*
 * pagerops for OBJT_SWAP - "swap pager".  Some ops are also global procedure
 * calls hooked from other parts of the VM system and do not appear here.
 * (see vm/swap_pager.h).
 */
static vm_object_t
		swap_pager_alloc(void *handle, vm_ooffset_t size,
				      vm_prot_t prot, vm_ooffset_t offset);
static void	swap_pager_dealloc(vm_object_t object);
static int	swap_pager_getpages(vm_object_t, vm_page_t *, int, int);
static boolean_t
		swap_pager_haspage(vm_object_t object, vm_pindex_t pindex, int *before, int *after);
static void	swap_pager_init(void);
static void	swap_pager_unswapped(vm_page_t);
static void	swap_pager_strategy(vm_object_t, struct bio *);
static void	swap_pager_swapoff(int devidx, int *sw_used);

struct pagerops swappagerops = {
	swap_pager_init,	/* early system initialization of pager	*/
	swap_pager_alloc,	/* allocate an OBJT_SWAP object		*/
	swap_pager_dealloc,	/* deallocate an OBJT_SWAP object	*/
	swap_pager_getpages,	/* pagein				*/
	swap_pager_putpages,	/* pageout				*/
	swap_pager_haspage,	/* get backing store status for page	*/
	swap_pager_unswapped,	/* remove swap related to page		*/
	swap_pager_strategy	/* pager strategy call			*/
};

static struct buf *getchainbuf(struct bio *bp, struct vnode *vp, int flags);
static void flushchainbuf(struct buf *nbp);
static void waitchainbuf(struct bio *bp, int count, int done);

/*
 * dmmax is in page-sized chunks with the new swap system.  It was
 * dev-bsized chunks in the old.  dmmax is always a power of 2.
 *
 * swap_*() routines are externally accessible.  swp_*() routines are
 * internal.
 */
static int dmmax, dmmax_mask;
static int nswap_lowat = 128;	/* in pages, swap_pager_almost_full warn */
static int nswap_hiwat = 512;	/* in pages, swap_pager_almost_full warn */

SYSCTL_INT(_vm, OID_AUTO, dmmax,
	CTLFLAG_RD, &dmmax, 0, "Maximum size of a swap block");

static void	swp_sizecheck(void);
static void	swp_pager_sync_iodone(struct buf *bp);
static void	swp_pager_async_iodone(struct buf *bp);

/*
 * Swap bitmap functions
 */
static void	swp_pager_freeswapspace(daddr_t blk, int npages);
static daddr_t	swp_pager_getswapspace(int npages);

/*
 * Metadata functions
 */
static struct swblock **swp_pager_hash(vm_object_t object, vm_pindex_t index);
static void swp_pager_meta_build(vm_object_t, vm_pindex_t, daddr_t);
static void swp_pager_meta_free(vm_object_t, vm_pindex_t, daddr_t);
static void swp_pager_meta_free_all(vm_object_t);
static daddr_t swp_pager_meta_ctl(vm_object_t, vm_pindex_t, int);

/*
 * SWP_SIZECHECK() -	update swap_pager_full indication
 *	
 *	update the swap_pager_almost_full indication and warn when we are
 *	about to run out of swap space, using lowat/hiwat hysteresis.
 *
 *	Clear swap_pager_full ( task killing ) indication when lowat is met.
 *
 *	No restrictions on call
 *	This routine may not block.
 *	This routine must be called at splvm()
 */
static void
swp_sizecheck()
{
	GIANT_REQUIRED;

	if (vm_swap_size < nswap_lowat) {
		if (swap_pager_almost_full == 0) {
			printf("swap_pager: out of swap space\n");
			swap_pager_almost_full = 1;
		}
	} else {
		swap_pager_full = 0;
		if (vm_swap_size > nswap_hiwat)
			swap_pager_almost_full = 0;
	}
}

/*
 * SWP_PAGER_HASH() -	hash swap meta data
 *
 *	This is an helper function which hashes the swapblk given
 *	the object and page index.  It returns a pointer to a pointer
 *	to the object, or a pointer to a NULL pointer if it could not
 *	find a swapblk.
 *
 *	This routine must be called at splvm().
 */
static struct swblock **
swp_pager_hash(vm_object_t object, vm_pindex_t index)
{
	struct swblock **pswap;
	struct swblock *swap;

	index &= ~(vm_pindex_t)SWAP_META_MASK;
	pswap = &swhash[(index ^ (int)(intptr_t)object) & swhash_mask];
	while ((swap = *pswap) != NULL) {
		if (swap->swb_object == object &&
		    swap->swb_index == index
		) {
			break;
		}
		pswap = &swap->swb_hnext;
	}
	return (pswap);
}

/*
 * SWAP_PAGER_INIT() -	initialize the swap pager!
 *
 *	Expected to be started from system init.  NOTE:  This code is run 
 *	before much else so be careful what you depend on.  Most of the VM
 *	system has yet to be initialized at this point.
 */
static void
swap_pager_init()
{
	/*
	 * Initialize object lists
	 */
	int i;

	for (i = 0; i < NOBJLISTS; ++i)
		TAILQ_INIT(&swap_pager_object_list[i]);
	TAILQ_INIT(&swap_pager_un_object_list);
	mtx_init(&sw_alloc_mtx, "swap_pager list", NULL, MTX_DEF);

	/*
	 * Device Stripe, in PAGE_SIZE'd blocks
	 */
	dmmax = SWB_NPAGES * 2;
	dmmax_mask = ~(dmmax - 1);
}

/*
 * SWAP_PAGER_SWAP_INIT() - swap pager initialization from pageout process
 *
 *	Expected to be started from pageout process once, prior to entering
 *	its main loop.
 */
void
swap_pager_swap_init()
{
	int n, n2;

	/*
	 * Number of in-transit swap bp operations.  Don't
	 * exhaust the pbufs completely.  Make sure we
	 * initialize workable values (0 will work for hysteresis
	 * but it isn't very efficient).
	 *
	 * The nsw_cluster_max is constrained by the bp->b_pages[]
	 * array (MAXPHYS/PAGE_SIZE) and our locally defined
	 * MAX_PAGEOUT_CLUSTER.   Also be aware that swap ops are
	 * constrained by the swap device interleave stripe size.
	 *
	 * Currently we hardwire nsw_wcount_async to 4.  This limit is 
	 * designed to prevent other I/O from having high latencies due to
	 * our pageout I/O.  The value 4 works well for one or two active swap
	 * devices but is probably a little low if you have more.  Even so,
	 * a higher value would probably generate only a limited improvement
	 * with three or four active swap devices since the system does not
	 * typically have to pageout at extreme bandwidths.   We will want
	 * at least 2 per swap devices, and 4 is a pretty good value if you
	 * have one NFS swap device due to the command/ack latency over NFS.
	 * So it all works out pretty well.
	 */
	nsw_cluster_max = min((MAXPHYS/PAGE_SIZE), MAX_PAGEOUT_CLUSTER);

	mtx_lock(&pbuf_mtx);
	nsw_rcount = (nswbuf + 1) / 2;
	nsw_wcount_sync = (nswbuf + 3) / 4;
	nsw_wcount_async = 4;
	nsw_wcount_async_max = nsw_wcount_async;
	mtx_unlock(&pbuf_mtx);

	/*
	 * Initialize our zone.  Right now I'm just guessing on the number
	 * we need based on the number of pages in the system.  Each swblock
	 * can hold 16 pages, so this is probably overkill.  This reservation
	 * is typically limited to around 32MB by default.
	 */
	n = cnt.v_page_count / 2;
	if (maxswzone && n > maxswzone / sizeof(struct swblock))
		n = maxswzone / sizeof(struct swblock);
	n2 = n;
	swap_zone = uma_zcreate("SWAPMETA", sizeof(struct swblock), NULL, NULL,
	    NULL, NULL, UMA_ALIGN_PTR, UMA_ZONE_NOFREE);
	do {
		if (uma_zone_set_obj(swap_zone, NULL, n))
			break;
		/*
		 * if the allocation failed, try a zone two thirds the
		 * size of the previous attempt.
		 */
		n -= ((n + 2) / 3);
	} while (n > 0);
	if (swap_zone == NULL)
		panic("failed to create swap_zone.");
	if (n2 != n)
		printf("Swap zone entries reduced from %d to %d.\n", n2, n);
	n2 = n;

	/*
	 * Initialize our meta-data hash table.  The swapper does not need to
	 * be quite as efficient as the VM system, so we do not use an 
	 * oversized hash table.
	 *
	 * 	n: 		size of hash table, must be power of 2
	 *	swhash_mask:	hash table index mask
	 */
	for (n = 1; n < n2 / 8; n *= 2)
		;
	swhash = malloc(sizeof(struct swblock *) * n, M_VMPGDATA, M_WAITOK | M_ZERO);
	swhash_mask = n - 1;
}

/*
 * SWAP_PAGER_ALLOC() -	allocate a new OBJT_SWAP VM object and instantiate
 *			its metadata structures.
 *
 *	This routine is called from the mmap and fork code to create a new
 *	OBJT_SWAP object.  We do this by creating an OBJT_DEFAULT object
 *	and then converting it with swp_pager_meta_build().
 *
 *	This routine may block in vm_object_allocate() and create a named
 *	object lookup race, so we must interlock.   We must also run at
 *	splvm() for the object lookup to handle races with interrupts, but
 *	we do not have to maintain splvm() in between the lookup and the
 *	add because (I believe) it is not possible to attempt to create
 *	a new swap object w/handle when a default object with that handle
 *	already exists.
 *
 * MPSAFE
 */
static vm_object_t
swap_pager_alloc(void *handle, vm_ooffset_t size, vm_prot_t prot,
		 vm_ooffset_t offset)
{
	vm_object_t object;

	mtx_lock(&Giant);
	if (handle) {
		/*
		 * Reference existing named region or allocate new one.  There
		 * should not be a race here against swp_pager_meta_build()
		 * as called from vm_page_remove() in regards to the lookup
		 * of the handle.
		 */
		sx_xlock(&sw_alloc_sx);
		object = vm_pager_object_lookup(NOBJLIST(handle), handle);

		if (object != NULL) {
			vm_object_reference(object);
		} else {
			object = vm_object_allocate(OBJT_DEFAULT,
				OFF_TO_IDX(offset + PAGE_MASK + size));
			object->handle = handle;

			swp_pager_meta_build(object, 0, SWAPBLK_NONE);
		}
		sx_xunlock(&sw_alloc_sx);
	} else {
		object = vm_object_allocate(OBJT_DEFAULT,
			OFF_TO_IDX(offset + PAGE_MASK + size));

		swp_pager_meta_build(object, 0, SWAPBLK_NONE);
	}
	mtx_unlock(&Giant);
	return (object);
}

/*
 * SWAP_PAGER_DEALLOC() -	remove swap metadata from object
 *
 *	The swap backing for the object is destroyed.  The code is 
 *	designed such that we can reinstantiate it later, but this
 *	routine is typically called only when the entire object is
 *	about to be destroyed.
 *
 *	This routine may block, but no longer does. 
 *
 *	The object must be locked or unreferenceable.
 */
static void
swap_pager_dealloc(object)
	vm_object_t object;
{
	int s;

	GIANT_REQUIRED;

	/*
	 * Remove from list right away so lookups will fail if we block for
	 * pageout completion.
	 */
	mtx_lock(&sw_alloc_mtx);
	if (object->handle == NULL) {
		TAILQ_REMOVE(&swap_pager_un_object_list, object, pager_object_list);
	} else {
		TAILQ_REMOVE(NOBJLIST(object->handle), object, pager_object_list);
	}
	mtx_unlock(&sw_alloc_mtx);

	VM_OBJECT_LOCK_ASSERT(object, MA_OWNED);
	vm_object_pip_wait(object, "swpdea");

	/*
	 * Free all remaining metadata.  We only bother to free it from 
	 * the swap meta data.  We do not attempt to free swapblk's still
	 * associated with vm_page_t's for this object.  We do not care
	 * if paging is still in progress on some objects.
	 */
	s = splvm();
	swp_pager_meta_free_all(object);
	splx(s);
}

/************************************************************************
 *			SWAP PAGER BITMAP ROUTINES			*
 ************************************************************************/

/*
 * SWP_PAGER_GETSWAPSPACE() -	allocate raw swap space
 *
 *	Allocate swap for the requested number of pages.  The starting
 *	swap block number (a page index) is returned or SWAPBLK_NONE
 *	if the allocation failed.
 *
 *	Also has the side effect of advising that somebody made a mistake
 *	when they configured swap and didn't configure enough.
 *
 *	Must be called at splvm() to avoid races with bitmap frees from
 *	vm_page_remove() aka swap_pager_page_removed().
 *
 *	This routine may not block
 *	This routine must be called at splvm().
 */
static daddr_t
swp_pager_getswapspace(npages)
	int npages;
{
	daddr_t blk;

	GIANT_REQUIRED;

	if ((blk = blist_alloc(swapblist, npages)) == SWAPBLK_NONE) {
		if (swap_pager_full != 2) {
			printf("swap_pager_getswapspace: failed\n");
			swap_pager_full = 2;
			swap_pager_almost_full = 1;
		}
	} else {
		vm_swap_size -= npages;
		/* per-swap area stats */
		swdevt[BLK2DEVIDX(blk)].sw_used += npages;
		swp_sizecheck();
	}
	return (blk);
}

/*
 * SWP_PAGER_FREESWAPSPACE() -	free raw swap space 
 *
 *	This routine returns the specified swap blocks back to the bitmap.
 *
 *	Note:  This routine may not block (it could in the old swap code),
 *	and through the use of the new blist routines it does not block.
 *
 *	We must be called at splvm() to avoid races with bitmap frees from
 *	vm_page_remove() aka swap_pager_page_removed().
 *
 *	This routine may not block
 *	This routine must be called at splvm().
 */
static void
swp_pager_freeswapspace(blk, npages)
	daddr_t blk;
	int npages;
{
	struct swdevt *sp = &swdevt[BLK2DEVIDX(blk)];

	GIANT_REQUIRED;

	/* per-swap area stats */
	sp->sw_used -= npages;

	/*
	 * If we are attempting to stop swapping on this device, we
	 * don't want to mark any blocks free lest they be reused.
	 */
	if (sp->sw_flags & SW_CLOSING)
		return;

	blist_free(swapblist, blk, npages);
	vm_swap_size += npages;
	swp_sizecheck();
}

/*
 * SWAP_PAGER_FREESPACE() -	frees swap blocks associated with a page
 *				range within an object.
 *
 *	This is a globally accessible routine.
 *
 *	This routine removes swapblk assignments from swap metadata.
 *
 *	The external callers of this routine typically have already destroyed 
 *	or renamed vm_page_t's associated with this range in the object so 
 *	we should be ok.
 *
 *	This routine may be called at any spl.  We up our spl to splvm temporarily
 *	in order to perform the metadata removal.
 */
void
swap_pager_freespace(object, start, size)
	vm_object_t object;
	vm_pindex_t start;
	vm_size_t size;
{
	int s = splvm();

	VM_OBJECT_LOCK_ASSERT(object, MA_OWNED);
	swp_pager_meta_free(object, start, size);
	splx(s);
}

/*
 * SWAP_PAGER_RESERVE() - reserve swap blocks in object
 *
 *	Assigns swap blocks to the specified range within the object.  The 
 *	swap blocks are not zerod.  Any previous swap assignment is destroyed.
 *
 *	Returns 0 on success, -1 on failure.
 */
int
swap_pager_reserve(vm_object_t object, vm_pindex_t start, vm_size_t size)
{
	int s;
	int n = 0;
	daddr_t blk = SWAPBLK_NONE;
	vm_pindex_t beg = start;	/* save start index */

	s = splvm();
	while (size) {
		if (n == 0) {
			n = BLIST_MAX_ALLOC;
			while ((blk = swp_pager_getswapspace(n)) == SWAPBLK_NONE) {
				n >>= 1;
				if (n == 0) {
					swp_pager_meta_free(object, beg, start - beg);
					splx(s);
					return (-1);
				}
			}
		}
		swp_pager_meta_build(object, start, blk);
		--size;
		++start;
		++blk;
		--n;
	}
	swp_pager_meta_free(object, start, n);
	splx(s);
	return (0);
}

/*
 * SWAP_PAGER_COPY() -  copy blocks from source pager to destination pager
 *			and destroy the source.
 *
 *	Copy any valid swapblks from the source to the destination.  In
 *	cases where both the source and destination have a valid swapblk,
 *	we keep the destination's.
 *
 *	This routine is allowed to block.  It may block allocating metadata
 *	indirectly through swp_pager_meta_build() or if paging is still in
 *	progress on the source. 
 *
 *	This routine can be called at any spl
 *
 *	XXX vm_page_collapse() kinda expects us not to block because we 
 *	supposedly do not need to allocate memory, but for the moment we
 *	*may* have to get a little memory from the zone allocator, but
 *	it is taken from the interrupt memory.  We should be ok. 
 *
 *	The source object contains no vm_page_t's (which is just as well)
 *
 *	The source object is of type OBJT_SWAP.
 *
 *	The source and destination objects must be locked or 
 *	inaccessible (XXX are they ?)
 */
void
swap_pager_copy(srcobject, dstobject, offset, destroysource)
	vm_object_t srcobject;
	vm_object_t dstobject;
	vm_pindex_t offset;
	int destroysource;
{
	vm_pindex_t i;
	int s;

	GIANT_REQUIRED;

	s = splvm();
	/*
	 * If destroysource is set, we remove the source object from the 
	 * swap_pager internal queue now. 
	 */
	if (destroysource) {
		mtx_lock(&sw_alloc_mtx);
		if (srcobject->handle == NULL) {
			TAILQ_REMOVE(
			    &swap_pager_un_object_list, 
			    srcobject, 
			    pager_object_list
			);
		} else {
			TAILQ_REMOVE(
			    NOBJLIST(srcobject->handle),
			    srcobject,
			    pager_object_list
			);
		}
		mtx_unlock(&sw_alloc_mtx);
	}

	/*
	 * transfer source to destination.
	 */
	for (i = 0; i < dstobject->size; ++i) {
		daddr_t dstaddr;

		/*
		 * Locate (without changing) the swapblk on the destination,
		 * unless it is invalid in which case free it silently, or
		 * if the destination is a resident page, in which case the
		 * source is thrown away.
		 */
		dstaddr = swp_pager_meta_ctl(dstobject, i, 0);

		if (dstaddr == SWAPBLK_NONE) {
			/*
			 * Destination has no swapblk and is not resident,
			 * copy source.
			 */
			daddr_t srcaddr;

			srcaddr = swp_pager_meta_ctl(
			    srcobject, 
			    i + offset,
			    SWM_POP
			);

			if (srcaddr != SWAPBLK_NONE)
				swp_pager_meta_build(dstobject, i, srcaddr);
		} else {
			/*
			 * Destination has valid swapblk or it is represented
			 * by a resident page.  We destroy the sourceblock.
			 */
			
			swp_pager_meta_ctl(srcobject, i + offset, SWM_FREE);
		}
	}

	/*
	 * Free left over swap blocks in source.
	 *
	 * We have to revert the type to OBJT_DEFAULT so we do not accidently
	 * double-remove the object from the swap queues.
	 */
	if (destroysource) {
		swp_pager_meta_free_all(srcobject);
		/*
		 * Reverting the type is not necessary, the caller is going
		 * to destroy srcobject directly, but I'm doing it here
		 * for consistency since we've removed the object from its
		 * queues.
		 */
		srcobject->type = OBJT_DEFAULT;
	}
	splx(s);
}

/*
 * SWAP_PAGER_HASPAGE() -	determine if we have good backing store for
 *				the requested page.
 *
 *	We determine whether good backing store exists for the requested
 *	page and return TRUE if it does, FALSE if it doesn't.
 *
 *	If TRUE, we also try to determine how much valid, contiguous backing
 *	store exists before and after the requested page within a reasonable
 *	distance.  We do not try to restrict it to the swap device stripe
 *	(that is handled in getpages/putpages).  It probably isn't worth
 *	doing here.
 */
static boolean_t
swap_pager_haspage(object, pindex, before, after)
	vm_object_t object;
	vm_pindex_t pindex;
	int *before;
	int *after;
{
	daddr_t blk0;
	int s;

	/*
	 * do we have good backing store at the requested index ?
	 */
	s = splvm();
	blk0 = swp_pager_meta_ctl(object, pindex, 0);

	if (blk0 == SWAPBLK_NONE) {
		splx(s);
		if (before)
			*before = 0;
		if (after)
			*after = 0;
		return (FALSE);
	}

	/*
	 * find backwards-looking contiguous good backing store
	 */
	if (before != NULL) {
		int i;

		for (i = 1; i < (SWB_NPAGES/2); ++i) {
			daddr_t blk;

			if (i > pindex)
				break;
			blk = swp_pager_meta_ctl(object, pindex - i, 0);
			if (blk != blk0 - i)
				break;
		}
		*before = (i - 1);
	}

	/*
	 * find forward-looking contiguous good backing store
	 */
	if (after != NULL) {
		int i;

		for (i = 1; i < (SWB_NPAGES/2); ++i) {
			daddr_t blk;

			blk = swp_pager_meta_ctl(object, pindex + i, 0);
			if (blk != blk0 + i)
				break;
		}
		*after = (i - 1);
	}
	splx(s);
	return (TRUE);
}

/*
 * SWAP_PAGER_PAGE_UNSWAPPED() - remove swap backing store related to page
 *
 *	This removes any associated swap backing store, whether valid or
 *	not, from the page.  
 *
 *	This routine is typically called when a page is made dirty, at
 *	which point any associated swap can be freed.  MADV_FREE also
 *	calls us in a special-case situation
 *
 *	NOTE!!!  If the page is clean and the swap was valid, the caller
 *	should make the page dirty before calling this routine.  This routine
 *	does NOT change the m->dirty status of the page.  Also: MADV_FREE
 *	depends on it.
 *
 *	This routine may not block
 *	This routine must be called at splvm()
 */
static void
swap_pager_unswapped(m)
	vm_page_t m;
{
	swp_pager_meta_ctl(m->object, m->pindex, SWM_FREE);
}

/*
 * SWAP_PAGER_STRATEGY() - read, write, free blocks
 *
 *	This implements the vm_pager_strategy() interface to swap and allows
 *	other parts of the system to directly access swap as backing store
 *	through vm_objects of type OBJT_SWAP.  This is intended to be a 
 *	cacheless interface ( i.e. caching occurs at higher levels ).
 *	Therefore we do not maintain any resident pages.  All I/O goes
 *	directly to and from the swap device.
 *	
 *	Note that b_blkno is scaled for PAGE_SIZE
 *
 *	We currently attempt to run I/O synchronously or asynchronously as
 *	the caller requests.  This isn't perfect because we loose error
 *	sequencing when we run multiple ops in parallel to satisfy a request.
 *	But this is swap, so we let it all hang out.
 */
static void	
swap_pager_strategy(vm_object_t object, struct bio *bp)
{
	vm_pindex_t start;
	int count;
	int s;
	char *data;
	struct buf *nbp = NULL;

	GIANT_REQUIRED;

	/* XXX: KASSERT instead ? */
	if (bp->bio_bcount & PAGE_MASK) {
		biofinish(bp, NULL, EINVAL);
		printf("swap_pager_strategy: bp %p blk %d size %d, not page bounded\n", bp, (int)bp->bio_pblkno, (int)bp->bio_bcount);
		return;
	}

	/*
	 * Clear error indication, initialize page index, count, data pointer.
	 */
	bp->bio_error = 0;
	bp->bio_flags &= ~BIO_ERROR;
	bp->bio_resid = bp->bio_bcount;
	*(u_int *) &bp->bio_driver1 = 0;

	start = bp->bio_pblkno;
	count = howmany(bp->bio_bcount, PAGE_SIZE);
	data = bp->bio_data;

	s = splvm();

	/*
	 * Deal with BIO_DELETE
	 */
	if (bp->bio_cmd == BIO_DELETE) {
		/*
		 * FREE PAGE(s) - destroy underlying swap that is no longer
		 *		  needed.
		 */
		swp_pager_meta_free(object, start, count);
		splx(s);
		bp->bio_resid = 0;
		biodone(bp);
		return;
	}

	/*
	 * Execute read or write
	 */
	while (count > 0) {
		daddr_t blk;

		/*
		 * Obtain block.  If block not found and writing, allocate a
		 * new block and build it into the object.
		 */

		blk = swp_pager_meta_ctl(object, start, 0);
		if ((blk == SWAPBLK_NONE) && (bp->bio_cmd == BIO_WRITE)) {
			blk = swp_pager_getswapspace(1);
			if (blk == SWAPBLK_NONE) {
				bp->bio_error = ENOMEM;
				bp->bio_flags |= BIO_ERROR;
				break;
			}
			swp_pager_meta_build(object, start, blk);
		}
			
		/*
		 * Do we have to flush our current collection?  Yes if:
		 *
		 *	- no swap block at this index
		 *	- swap block is not contiguous
		 *	- we cross a physical disk boundry in the
		 *	  stripe.
		 */
		if (
		    nbp && (nbp->b_blkno + btoc(nbp->b_bcount) != blk ||
		     ((nbp->b_blkno ^ blk) & dmmax_mask)
		    )
		) {
			splx(s);
			if (bp->bio_cmd == BIO_READ) {
				++cnt.v_swapin;
				cnt.v_swappgsin += btoc(nbp->b_bcount);
			} else {
				++cnt.v_swapout;
				cnt.v_swappgsout += btoc(nbp->b_bcount);
				nbp->b_dirtyend = nbp->b_bcount;
			}
			flushchainbuf(nbp);
			s = splvm();
			nbp = NULL;
		}

		/*
		 * Add new swapblk to nbp, instantiating nbp if necessary.
		 * Zero-fill reads are able to take a shortcut.
		 */
		if (blk == SWAPBLK_NONE) {
			/*
			 * We can only get here if we are reading.  Since
			 * we are at splvm() we can safely modify b_resid,
			 * even if chain ops are in progress.
			 */
			bzero(data, PAGE_SIZE);
			bp->bio_resid -= PAGE_SIZE;
		} else {
			if (nbp == NULL) {
				nbp = getchainbuf(bp, swapdev_vp, B_ASYNC);
				nbp->b_blkno = blk;
				nbp->b_bcount = 0;
				nbp->b_data = data;
			}
			nbp->b_bcount += PAGE_SIZE;
		}
		--count;
		++start;
		data += PAGE_SIZE;
	}

	/*
	 *  Flush out last buffer
	 */
	splx(s);

	if (nbp) {
		if (nbp->b_iocmd == BIO_READ) {
			++cnt.v_swapin;
			cnt.v_swappgsin += btoc(nbp->b_bcount);
		} else {
			++cnt.v_swapout;
			cnt.v_swappgsout += btoc(nbp->b_bcount);
			nbp->b_dirtyend = nbp->b_bcount;
		}
		flushchainbuf(nbp);
		/* nbp = NULL; */
	}
	/*
	 * Wait for completion.
	 */
	waitchainbuf(bp, 0, 1);
}

/*
 * SWAP_PAGER_GETPAGES() - bring pages in from swap
 *
 *	Attempt to retrieve (m, count) pages from backing store, but make
 *	sure we retrieve at least m[reqpage].  We try to load in as large
 *	a chunk surrounding m[reqpage] as is contiguous in swap and which
 *	belongs to the same object.
 *
 *	The code is designed for asynchronous operation and 
 *	immediate-notification of 'reqpage' but tends not to be
 *	used that way.  Please do not optimize-out this algorithmic
 *	feature, I intend to improve on it in the future.
 *
 *	The parent has a single vm_object_pip_add() reference prior to
 *	calling us and we should return with the same.
 *
 *	The parent has BUSY'd the pages.  We should return with 'm'
 *	left busy, but the others adjusted.
 */
static int
swap_pager_getpages(object, m, count, reqpage)
	vm_object_t object;
	vm_page_t *m;
	int count, reqpage;
{
	struct buf *bp;
	vm_page_t mreq;
	int s;
	int i;
	int j;
	daddr_t blk;
	vm_pindex_t lastpindex;

	mreq = m[reqpage];

	if (mreq->object != object) {
		panic("swap_pager_getpages: object mismatch %p/%p", 
		    object, 
		    mreq->object
		);
	}
	/*
	 * Calculate range to retrieve.  The pages have already been assigned
	 * their swapblks.  We require a *contiguous* range that falls entirely
	 * within a single device stripe.   If we do not supply it, bad things
	 * happen.  Note that blk, iblk & jblk can be SWAPBLK_NONE, but the 
	 * loops are set up such that the case(s) are handled implicitly.
	 *
	 * The swp_*() calls must be made at splvm().  vm_page_free() does
	 * not need to be, but it will go a little faster if it is.
	 */
	s = splvm();
	blk = swp_pager_meta_ctl(mreq->object, mreq->pindex, 0);

	for (i = reqpage - 1; i >= 0; --i) {
		daddr_t iblk;

		iblk = swp_pager_meta_ctl(m[i]->object, m[i]->pindex, 0);
		if (blk != iblk + (reqpage - i))
			break;
		if ((blk ^ iblk) & dmmax_mask)
			break;
	}
	++i;

	for (j = reqpage + 1; j < count; ++j) {
		daddr_t jblk;

		jblk = swp_pager_meta_ctl(m[j]->object, m[j]->pindex, 0);
		if (blk != jblk - (j - reqpage))
			break;
		if ((blk ^ jblk) & dmmax_mask)
			break;
	}

	/*
	 * free pages outside our collection range.   Note: we never free
	 * mreq, it must remain busy throughout.
	 */
	vm_page_lock_queues();
	{
		int k;

		for (k = 0; k < i; ++k)
			vm_page_free(m[k]);
		for (k = j; k < count; ++k)
			vm_page_free(m[k]);
	}
	vm_page_unlock_queues();
	splx(s);


	/*
	 * Return VM_PAGER_FAIL if we have nothing to do.  Return mreq 
	 * still busy, but the others unbusied.
	 */
	if (blk == SWAPBLK_NONE)
		return (VM_PAGER_FAIL);

	/*
	 * Getpbuf() can sleep.
	 */
	VM_OBJECT_UNLOCK(object);
	/*
	 * Get a swap buffer header to perform the IO
	 */
	bp = getpbuf(&nsw_rcount);

	/*
	 * map our page(s) into kva for input
	 *
	 * NOTE: B_PAGING is set by pbgetvp()
	 */
	pmap_qenter((vm_offset_t)bp->b_data, m + i, j - i);

	bp->b_iocmd = BIO_READ;
	bp->b_iodone = swp_pager_async_iodone;
	bp->b_rcred = crhold(thread0.td_ucred);
	bp->b_wcred = crhold(thread0.td_ucred);
	bp->b_blkno = blk - (reqpage - i);
	bp->b_bcount = PAGE_SIZE * (j - i);
	bp->b_bufsize = PAGE_SIZE * (j - i);
	bp->b_pager.pg_reqpage = reqpage - i;

	VM_OBJECT_LOCK(object);
	vm_page_lock_queues();
	{
		int k;

		for (k = i; k < j; ++k) {
			bp->b_pages[k - i] = m[k];
			vm_page_flag_set(m[k], PG_SWAPINPROG);
		}
	}
	vm_page_unlock_queues();
	VM_OBJECT_UNLOCK(object);
	bp->b_npages = j - i;

	pbgetvp(swapdev_vp, bp);

	cnt.v_swapin++;
	cnt.v_swappgsin += bp->b_npages;

	/*
	 * We still hold the lock on mreq, and our automatic completion routine
	 * does not remove it.
	 */
	VM_OBJECT_LOCK(mreq->object);
	vm_object_pip_add(mreq->object, bp->b_npages);
	VM_OBJECT_UNLOCK(mreq->object);
	lastpindex = m[j-1]->pindex;

	/*
	 * perform the I/O.  NOTE!!!  bp cannot be considered valid after
	 * this point because we automatically release it on completion.
	 * Instead, we look at the one page we are interested in which we
	 * still hold a lock on even through the I/O completion.
	 *
	 * The other pages in our m[] array are also released on completion,
	 * so we cannot assume they are valid anymore either.
	 *
	 * NOTE: b_blkno is destroyed by the call to VOP_STRATEGY
	 */
	BUF_KERNPROC(bp);
	VOP_STRATEGY(bp->b_vp, bp);

	/*
	 * wait for the page we want to complete.  PG_SWAPINPROG is always
	 * cleared on completion.  If an I/O error occurs, SWAPBLK_NONE
	 * is set in the meta-data.
	 */
	s = splvm();
	vm_page_lock_queues();
	while ((mreq->flags & PG_SWAPINPROG) != 0) {
		vm_page_flag_set(mreq, PG_WANTED | PG_REFERENCED);
		cnt.v_intrans++;
		if (msleep(mreq, &vm_page_queue_mtx, PSWP, "swread", hz*20)) {
			printf(
			    "swap_pager: indefinite wait buffer: device:"
				" %s, blkno: %ld, size: %ld\n",
			    devtoname(bp->b_dev), (long)bp->b_blkno,
			    bp->b_bcount
			);
		}
	}
	vm_page_unlock_queues();
	splx(s);

	VM_OBJECT_LOCK(mreq->object);
	/*
	 * mreq is left busied after completion, but all the other pages
	 * are freed.  If we had an unrecoverable read error the page will
	 * not be valid.
	 */
	if (mreq->valid != VM_PAGE_BITS_ALL) {
		return (VM_PAGER_ERROR);
	} else {
		return (VM_PAGER_OK);
	}

	/*
	 * A final note: in a low swap situation, we cannot deallocate swap
	 * and mark a page dirty here because the caller is likely to mark
	 * the page clean when we return, causing the page to possibly revert 
	 * to all-zero's later.
	 */
}

/*
 *	swap_pager_putpages: 
 *
 *	Assign swap (if necessary) and initiate I/O on the specified pages.
 *
 *	We support both OBJT_DEFAULT and OBJT_SWAP objects.  DEFAULT objects
 *	are automatically converted to SWAP objects.
 *
 *	In a low memory situation we may block in VOP_STRATEGY(), but the new 
 *	vm_page reservation system coupled with properly written VFS devices 
 *	should ensure that no low-memory deadlock occurs.  This is an area
 *	which needs work.
 *
 *	The parent has N vm_object_pip_add() references prior to
 *	calling us and will remove references for rtvals[] that are
 *	not set to VM_PAGER_PEND.  We need to remove the rest on I/O
 *	completion.
 *
 *	The parent has soft-busy'd the pages it passes us and will unbusy
 *	those whos rtvals[] entry is not set to VM_PAGER_PEND on return.
 *	We need to unbusy the rest on I/O completion.
 */
void
swap_pager_putpages(object, m, count, sync, rtvals)
	vm_object_t object;
	vm_page_t *m;
	int count;
	boolean_t sync;
	int *rtvals;
{
	int i;
	int n = 0;

	GIANT_REQUIRED;
	if (count && m[0]->object != object) {
		panic("swap_pager_getpages: object mismatch %p/%p", 
		    object, 
		    m[0]->object
		);
	}
	/*
	 * Step 1
	 *
	 * Turn object into OBJT_SWAP
	 * check for bogus sysops
	 * force sync if not pageout process
	 */
	if (object->type != OBJT_SWAP)
		swp_pager_meta_build(object, 0, SWAPBLK_NONE);

	if (curproc != pageproc)
		sync = TRUE;

	/*
	 * Step 2
	 *
	 * Update nsw parameters from swap_async_max sysctl values.  
	 * Do not let the sysop crash the machine with bogus numbers.
	 */
	mtx_lock(&pbuf_mtx);
	if (swap_async_max != nsw_wcount_async_max) {
		int n;
		int s;

		/*
		 * limit range
		 */
		if ((n = swap_async_max) > nswbuf / 2)
			n = nswbuf / 2;
		if (n < 1)
			n = 1;
		swap_async_max = n;

		/*
		 * Adjust difference ( if possible ).  If the current async
		 * count is too low, we may not be able to make the adjustment
		 * at this time.
		 */
		s = splvm();
		n -= nsw_wcount_async_max;
		if (nsw_wcount_async + n >= 0) {
			nsw_wcount_async += n;
			nsw_wcount_async_max += n;
			wakeup(&nsw_wcount_async);
		}
		splx(s);
	}
	mtx_unlock(&pbuf_mtx);

	/*
	 * Step 3
	 *
	 * Assign swap blocks and issue I/O.  We reallocate swap on the fly.
	 * The page is left dirty until the pageout operation completes
	 * successfully.
	 */
	for (i = 0; i < count; i += n) {
		int s;
		int j;
		struct buf *bp;
		daddr_t blk;

		/*
		 * Maximum I/O size is limited by a number of factors.
		 */
		n = min(BLIST_MAX_ALLOC, count - i);
		n = min(n, nsw_cluster_max);

		s = splvm();

		/*
		 * Get biggest block of swap we can.  If we fail, fall
		 * back and try to allocate a smaller block.  Don't go
		 * overboard trying to allocate space if it would overly
		 * fragment swap.
		 */
		while (
		    (blk = swp_pager_getswapspace(n)) == SWAPBLK_NONE &&
		    n > 4
		) {
			n >>= 1;
		}
		if (blk == SWAPBLK_NONE) {
			for (j = 0; j < n; ++j)
				rtvals[i+j] = VM_PAGER_FAIL;
			splx(s);
			continue;
		}

		/*
		 * The I/O we are constructing cannot cross a physical
		 * disk boundry in the swap stripe.  Note: we are still
		 * at splvm().
		 */
		if ((blk ^ (blk + n)) & dmmax_mask) {
			j = ((blk + dmmax) & dmmax_mask) - blk;
			swp_pager_freeswapspace(blk + j, n - j);
			n = j;
		}

		/*
		 * All I/O parameters have been satisfied, build the I/O
		 * request and assign the swap space.
		 *
		 * NOTE: B_PAGING is set by pbgetvp()
		 */
		if (sync == TRUE) {
			bp = getpbuf(&nsw_wcount_sync);
		} else {
			bp = getpbuf(&nsw_wcount_async);
			bp->b_flags = B_ASYNC;
		}
		bp->b_iocmd = BIO_WRITE;

		pmap_qenter((vm_offset_t)bp->b_data, &m[i], n);

		bp->b_rcred = crhold(thread0.td_ucred);
		bp->b_wcred = crhold(thread0.td_ucred);
		bp->b_bcount = PAGE_SIZE * n;
		bp->b_bufsize = PAGE_SIZE * n;
		bp->b_blkno = blk;

		pbgetvp(swapdev_vp, bp);

		for (j = 0; j < n; ++j) {
			vm_page_t mreq = m[i+j];

			swp_pager_meta_build(
			    mreq->object, 
			    mreq->pindex,
			    blk + j
			);
			vm_page_dirty(mreq);
			rtvals[i+j] = VM_PAGER_OK;

			vm_page_lock_queues();
			vm_page_flag_set(mreq, PG_SWAPINPROG);
			vm_page_unlock_queues();
			bp->b_pages[j] = mreq;
		}
		bp->b_npages = n;
		/*
		 * Must set dirty range for NFS to work.
		 */
		bp->b_dirtyoff = 0;
		bp->b_dirtyend = bp->b_bcount;

		cnt.v_swapout++;
		cnt.v_swappgsout += bp->b_npages;
		VI_LOCK(swapdev_vp);
		swapdev_vp->v_numoutput++;
		VI_UNLOCK(swapdev_vp);

		splx(s);

		/*
		 * asynchronous
		 *
		 * NOTE: b_blkno is destroyed by the call to VOP_STRATEGY
		 */
		if (sync == FALSE) {
			bp->b_iodone = swp_pager_async_iodone;
			BUF_KERNPROC(bp);
			VOP_STRATEGY(bp->b_vp, bp);

			for (j = 0; j < n; ++j)
				rtvals[i+j] = VM_PAGER_PEND;
			/* restart outter loop */
			continue;
		}

		/*
		 * synchronous
		 *
		 * NOTE: b_blkno is destroyed by the call to VOP_STRATEGY
		 */
		bp->b_iodone = swp_pager_sync_iodone;
		VOP_STRATEGY(bp->b_vp, bp);

		/*
		 * Wait for the sync I/O to complete, then update rtvals.
		 * We just set the rtvals[] to VM_PAGER_PEND so we can call
		 * our async completion routine at the end, thus avoiding a
		 * double-free.
		 */
		s = splbio();
		while ((bp->b_flags & B_DONE) == 0) {
			tsleep(bp, PVM, "swwrt", 0);
		}
		for (j = 0; j < n; ++j)
			rtvals[i+j] = VM_PAGER_PEND;
		/*
		 * Now that we are through with the bp, we can call the
		 * normal async completion, which frees everything up.
		 */
		swp_pager_async_iodone(bp);
		splx(s);
	}
}

/*
 *	swap_pager_sync_iodone:
 *
 *	Completion routine for synchronous reads and writes from/to swap.
 *	We just mark the bp is complete and wake up anyone waiting on it.
 *
 *	This routine may not block.  This routine is called at splbio() or better.
 */
static void
swp_pager_sync_iodone(bp)
	struct buf *bp;
{
	bp->b_flags |= B_DONE;
	bp->b_flags &= ~B_ASYNC;
	wakeup(bp);
}

/*
 *	swp_pager_async_iodone:
 *
 *	Completion routine for asynchronous reads and writes from/to swap.
 *	Also called manually by synchronous code to finish up a bp.
 *
 *	For READ operations, the pages are PG_BUSY'd.  For WRITE operations, 
 *	the pages are vm_page_t->busy'd.  For READ operations, we PG_BUSY 
 *	unbusy all pages except the 'main' request page.  For WRITE 
 *	operations, we vm_page_t->busy'd unbusy all pages ( we can do this 
 *	because we marked them all VM_PAGER_PEND on return from putpages ).
 *
 *	This routine may not block.
 *	This routine is called at splbio() or better
 *
 *	We up ourselves to splvm() as required for various vm_page related
 *	calls.
 */
static void
swp_pager_async_iodone(bp)
	struct buf *bp;
{
	int s;
	int i;
	vm_object_t object = NULL;

	GIANT_REQUIRED;
	bp->b_flags |= B_DONE;

	/*
	 * report error
	 */
	if (bp->b_ioflags & BIO_ERROR) {
		printf(
		    "swap_pager: I/O error - %s failed; blkno %ld,"
			"size %ld, error %d\n",
		    ((bp->b_iocmd == BIO_READ) ? "pagein" : "pageout"),
		    (long)bp->b_blkno, 
		    (long)bp->b_bcount,
		    bp->b_error
		);
	}

	/*
	 * set object, raise to splvm().
	 */
	s = splvm();

	/*
	 * remove the mapping for kernel virtual
	 */
	pmap_qremove((vm_offset_t)bp->b_data, bp->b_npages);

	if (bp->b_npages) {
		object = bp->b_pages[0]->object;
		VM_OBJECT_LOCK(object);
	}
	vm_page_lock_queues();
	/*
	 * cleanup pages.  If an error occurs writing to swap, we are in
	 * very serious trouble.  If it happens to be a disk error, though,
	 * we may be able to recover by reassigning the swap later on.  So
	 * in this case we remove the m->swapblk assignment for the page 
	 * but do not free it in the rlist.  The errornous block(s) are thus
	 * never reallocated as swap.  Redirty the page and continue.
	 */
	for (i = 0; i < bp->b_npages; ++i) {
		vm_page_t m = bp->b_pages[i];

		vm_page_flag_clear(m, PG_SWAPINPROG);

		if (bp->b_ioflags & BIO_ERROR) {
			/*
			 * If an error occurs I'd love to throw the swapblk
			 * away without freeing it back to swapspace, so it
			 * can never be used again.  But I can't from an 
			 * interrupt.
			 */
			if (bp->b_iocmd == BIO_READ) {
				/*
				 * When reading, reqpage needs to stay
				 * locked for the parent, but all other
				 * pages can be freed.  We still want to
				 * wakeup the parent waiting on the page,
				 * though.  ( also: pg_reqpage can be -1 and 
				 * not match anything ).
				 *
				 * We have to wake specifically requested pages
				 * up too because we cleared PG_SWAPINPROG and
				 * someone may be waiting for that.
				 *
				 * NOTE: for reads, m->dirty will probably
				 * be overridden by the original caller of
				 * getpages so don't play cute tricks here.
				 *
				 * XXX IT IS NOT LEGAL TO FREE THE PAGE HERE
				 * AS THIS MESSES WITH object->memq, and it is
				 * not legal to mess with object->memq from an
				 * interrupt.
				 */
				m->valid = 0;
				vm_page_flag_clear(m, PG_ZERO);
				if (i != bp->b_pager.pg_reqpage)
					vm_page_free(m);
				else
					vm_page_flash(m);
				/*
				 * If i == bp->b_pager.pg_reqpage, do not wake 
				 * the page up.  The caller needs to.
				 */
			} else {
				/*
				 * If a write error occurs, reactivate page
				 * so it doesn't clog the inactive list,
				 * then finish the I/O.
				 */
				vm_page_dirty(m);
				vm_page_activate(m);
				vm_page_io_finish(m);
			}
		} else if (bp->b_iocmd == BIO_READ) {
			/*
			 * For read success, clear dirty bits.  Nobody should
			 * have this page mapped but don't take any chances,
			 * make sure the pmap modify bits are also cleared.
			 *
			 * NOTE: for reads, m->dirty will probably be 
			 * overridden by the original caller of getpages so
			 * we cannot set them in order to free the underlying
			 * swap in a low-swap situation.  I don't think we'd
			 * want to do that anyway, but it was an optimization
			 * that existed in the old swapper for a time before
			 * it got ripped out due to precisely this problem.
			 *
			 * clear PG_ZERO in page.
			 *
			 * If not the requested page then deactivate it.
			 *
			 * Note that the requested page, reqpage, is left
			 * busied, but we still have to wake it up.  The
			 * other pages are released (unbusied) by 
			 * vm_page_wakeup().  We do not set reqpage's
			 * valid bits here, it is up to the caller.
			 */
			pmap_clear_modify(m);
			m->valid = VM_PAGE_BITS_ALL;
			vm_page_undirty(m);
			vm_page_flag_clear(m, PG_ZERO);

			/*
			 * We have to wake specifically requested pages
			 * up too because we cleared PG_SWAPINPROG and
			 * could be waiting for it in getpages.  However,
			 * be sure to not unbusy getpages specifically
			 * requested page - getpages expects it to be 
			 * left busy.
			 */
			if (i != bp->b_pager.pg_reqpage) {
				vm_page_deactivate(m);
				vm_page_wakeup(m);
			} else {
				vm_page_flash(m);
			}
		} else {
			/*
			 * For write success, clear the modify and dirty 
			 * status, then finish the I/O ( which decrements the 
			 * busy count and possibly wakes waiter's up ).
			 */
			pmap_clear_modify(m);
			vm_page_undirty(m);
			vm_page_io_finish(m);
			if (!vm_page_count_severe() || !vm_page_try_to_cache(m))
				pmap_page_protect(m, VM_PROT_READ);
		}
	}
	vm_page_unlock_queues();

	/*
	 * adjust pip.  NOTE: the original parent may still have its own
	 * pip refs on the object.
	 */
	if (object != NULL) {
		vm_object_pip_wakeupn(object, bp->b_npages);
		VM_OBJECT_UNLOCK(object);
	}

	/*
	 * release the physical I/O buffer
	 */
	relpbuf(
	    bp, 
	    ((bp->b_iocmd == BIO_READ) ? &nsw_rcount : 
		((bp->b_flags & B_ASYNC) ? 
		    &nsw_wcount_async : 
		    &nsw_wcount_sync
		)
	    )
	);
	splx(s);
}

/*
 *	swap_pager_isswapped:
 *
 *	Return 1 if at least one page in the given object is paged
 *	out to the given swap device.
 *
 *	This routine may not block.
 */
int swap_pager_isswapped(vm_object_t object, int devidx) {
	daddr_t index = 0;
	int bcount;
	int i;

	VM_OBJECT_LOCK_ASSERT(object, MA_OWNED);
	for (bcount = 0; bcount < object->un_pager.swp.swp_bcount; bcount++) {
		struct swblock *swap;

		if ((swap = *swp_pager_hash(object, index)) != NULL) {
			for (i = 0; i < SWAP_META_PAGES; ++i) {
				daddr_t v = swap->swb_pages[i];
				if (v != SWAPBLK_NONE &&
				    BLK2DEVIDX(v) == devidx)
					return 1;
			}
		}

		index += SWAP_META_PAGES;
		if (index > 0x20000000)
			panic("swap_pager_isswapped: failed to locate all swap meta blocks");
	}
	return 0;
}

/*
 * SWP_PAGER_FORCE_PAGEIN() - force a swap block to be paged in
 *
 *	This routine dissociates the page at the given index within a
 *	swap block from its backing store, paging it in if necessary.
 *	If the page is paged in, it is placed in the inactive queue,
 *	since it had its backing store ripped out from under it.
 *	We also attempt to swap in all other pages in the swap block,
 *	we only guarantee that the one at the specified index is
 *	paged in.
 *
 *	XXX - The code to page the whole block in doesn't work, so we
 *	      revert to the one-by-one behavior for now.  Sigh.
 */
static __inline void
swp_pager_force_pagein(struct swblock *swap, int idx)
{
	vm_object_t object;
	vm_page_t m;
	vm_pindex_t pindex;

	object = swap->swb_object;
	pindex = swap->swb_index;

	VM_OBJECT_LOCK(object);
	vm_object_pip_add(object, 1);
	m = vm_page_grab(object, pindex + idx, VM_ALLOC_NORMAL|VM_ALLOC_RETRY);
	if (m->valid == VM_PAGE_BITS_ALL) {
		vm_object_pip_subtract(object, 1);
		VM_OBJECT_UNLOCK(object);
		vm_page_lock_queues();
		vm_page_activate(m);
		vm_page_dirty(m);
		vm_page_wakeup(m);
		vm_page_unlock_queues();
		vm_pager_page_unswapped(m);
		return;
	}

	if (swap_pager_getpages(object, &m, 1, 0) !=
	    VM_PAGER_OK)
		panic("swap_pager_force_pagein: read from swap failed");/*XXX*/
	vm_object_pip_subtract(object, 1);
	VM_OBJECT_UNLOCK(object);

	vm_page_lock_queues();
	vm_page_dirty(m);
	vm_page_dontneed(m);
	vm_page_wakeup(m);
	vm_page_unlock_queues();
	vm_pager_page_unswapped(m);
}


/*
 *	swap_pager_swapoff:
 *
 *	Page in all of the pages that have been paged out to the
 *	given device.  The corresponding blocks in the bitmap must be
 *	marked as allocated and the device must be flagged SW_CLOSING.
 *	There may be no processes swapped out to the device.
 *
 *	The sw_used parameter points to the field in the swdev structure
 *	that contains a count of the number of blocks still allocated
 *	on the device.  If we encounter objects with a nonzero pip count
 *	in our scan, we use this number to determine if we're really done.
 *
 *	This routine may block.
 */
static void
swap_pager_swapoff(int devidx, int *sw_used)
{
	struct swblock **pswap;
	struct swblock *swap;
	vm_object_t waitobj;
	daddr_t v;
	int i, j;

	GIANT_REQUIRED;

full_rescan:
	waitobj = NULL;
	for (i = 0; i <= swhash_mask; i++) { /* '<=' is correct here */
restart:
		pswap = &swhash[i];
		while ((swap = *pswap) != NULL) {
                        for (j = 0; j < SWAP_META_PAGES; ++j) {
                                v = swap->swb_pages[j];
                                if (v != SWAPBLK_NONE &&
				    BLK2DEVIDX(v) == devidx)
                                        break;
                        }
			if (j < SWAP_META_PAGES) {
				swp_pager_force_pagein(swap, j);
				goto restart;
			} else if (swap->swb_object->paging_in_progress) {
				if (!waitobj)
					waitobj = swap->swb_object;
			}
			pswap = &swap->swb_hnext;
		}
	}
	if (waitobj && *sw_used) {
	    /*
	     * We wait on an arbitrary object to clock our rescans
	     * to the rate of paging completion.
	     */
	    VM_OBJECT_LOCK(waitobj);
	    vm_object_pip_wait(waitobj, "swpoff");
	    VM_OBJECT_UNLOCK(waitobj);
	    goto full_rescan;
	}
	if (*sw_used)
	    panic("swapoff: failed to locate %d swap blocks", *sw_used);
}

/************************************************************************
 *				SWAP META DATA 				*
 ************************************************************************
 *
 *	These routines manipulate the swap metadata stored in the 
 *	OBJT_SWAP object.  All swp_*() routines must be called at
 *	splvm() because swap can be freed up by the low level vm_page
 *	code which might be called from interrupts beyond what splbio() covers.
 *
 *	Swap metadata is implemented with a global hash and not directly
 *	linked into the object.  Instead the object simply contains
 *	appropriate tracking counters.
 */

/*
 * SWP_PAGER_META_BUILD() -	add swap block to swap meta data for object
 *
 *	We first convert the object to a swap object if it is a default
 *	object.
 *
 *	The specified swapblk is added to the object's swap metadata.  If
 *	the swapblk is not valid, it is freed instead.  Any previously
 *	assigned swapblk is freed.
 *
 *	This routine must be called at splvm(), except when used to convert
 *	an OBJT_DEFAULT object into an OBJT_SWAP object.
 */
static void
swp_pager_meta_build(
	vm_object_t object, 
	vm_pindex_t pindex,
	daddr_t swapblk
) {
	struct swblock *swap;
	struct swblock **pswap;
	int idx;

	GIANT_REQUIRED;
	/*
	 * Convert default object to swap object if necessary
	 */
	if (object->type != OBJT_SWAP) {
		object->type = OBJT_SWAP;
		object->un_pager.swp.swp_bcount = 0;

		mtx_lock(&sw_alloc_mtx);
		if (object->handle != NULL) {
			TAILQ_INSERT_TAIL(
			    NOBJLIST(object->handle),
			    object, 
			    pager_object_list
			);
		} else {
			TAILQ_INSERT_TAIL(
			    &swap_pager_un_object_list,
			    object, 
			    pager_object_list
			);
		}
		mtx_unlock(&sw_alloc_mtx);
	}
	
	/*
	 * Locate hash entry.  If not found create, but if we aren't adding
	 * anything just return.  If we run out of space in the map we wait
	 * and, since the hash table may have changed, retry.
	 */
retry:
	pswap = swp_pager_hash(object, pindex);

	if ((swap = *pswap) == NULL) {
		int i;

		if (swapblk == SWAPBLK_NONE)
			return;

		swap = *pswap = uma_zalloc(swap_zone, M_NOWAIT);
		if (swap == NULL) {
			VM_WAIT;
			goto retry;
		}

		swap->swb_hnext = NULL;
		swap->swb_object = object;
		swap->swb_index = pindex & ~(vm_pindex_t)SWAP_META_MASK;
		swap->swb_count = 0;

		++object->un_pager.swp.swp_bcount;

		for (i = 0; i < SWAP_META_PAGES; ++i)
			swap->swb_pages[i] = SWAPBLK_NONE;
	}

	/*
	 * Delete prior contents of metadata
	 */
	idx = pindex & SWAP_META_MASK;

	if (swap->swb_pages[idx] != SWAPBLK_NONE) {
		swp_pager_freeswapspace(swap->swb_pages[idx], 1);
		--swap->swb_count;
	}

	/*
	 * Enter block into metadata
	 */
	swap->swb_pages[idx] = swapblk;
	if (swapblk != SWAPBLK_NONE)
		++swap->swb_count;
}

/*
 * SWP_PAGER_META_FREE() - free a range of blocks in the object's swap metadata
 *
 *	The requested range of blocks is freed, with any associated swap 
 *	returned to the swap bitmap.
 *
 *	This routine will free swap metadata structures as they are cleaned 
 *	out.  This routine does *NOT* operate on swap metadata associated
 *	with resident pages.
 *
 *	This routine must be called at splvm()
 */
static void
swp_pager_meta_free(vm_object_t object, vm_pindex_t index, daddr_t count)
{
	GIANT_REQUIRED;

	if (object->type != OBJT_SWAP)
		return;

	while (count > 0) {
		struct swblock **pswap;
		struct swblock *swap;

		pswap = swp_pager_hash(object, index);

		if ((swap = *pswap) != NULL) {
			daddr_t v = swap->swb_pages[index & SWAP_META_MASK];

			if (v != SWAPBLK_NONE) {
				swp_pager_freeswapspace(v, 1);
				swap->swb_pages[index & SWAP_META_MASK] =
					SWAPBLK_NONE;
				if (--swap->swb_count == 0) {
					*pswap = swap->swb_hnext;
					uma_zfree(swap_zone, swap);
					--object->un_pager.swp.swp_bcount;
				}
			}
			--count;
			++index;
		} else {
			int n = SWAP_META_PAGES - (index & SWAP_META_MASK);
			count -= n;
			index += n;
		}
	}
}

/*
 * SWP_PAGER_META_FREE_ALL() - destroy all swap metadata associated with object
 *
 *	This routine locates and destroys all swap metadata associated with
 *	an object.
 *
 *	This routine must be called at splvm()
 */
static void
swp_pager_meta_free_all(vm_object_t object)
{
	daddr_t index = 0;

	GIANT_REQUIRED;
	
	if (object->type != OBJT_SWAP)
		return;

	while (object->un_pager.swp.swp_bcount) {
		struct swblock **pswap;
		struct swblock *swap;

		pswap = swp_pager_hash(object, index);
		if ((swap = *pswap) != NULL) {
			int i;

			for (i = 0; i < SWAP_META_PAGES; ++i) {
				daddr_t v = swap->swb_pages[i];
				if (v != SWAPBLK_NONE) {
					--swap->swb_count;
					swp_pager_freeswapspace(v, 1);
				}
			}
			if (swap->swb_count != 0)
				panic("swap_pager_meta_free_all: swb_count != 0");
			*pswap = swap->swb_hnext;
			uma_zfree(swap_zone, swap);
			--object->un_pager.swp.swp_bcount;
		}
		index += SWAP_META_PAGES;
		if (index > 0x20000000)
			panic("swp_pager_meta_free_all: failed to locate all swap meta blocks");
	}
}

/*
 * SWP_PAGER_METACTL() -  misc control of swap and vm_page_t meta data.
 *
 *	This routine is capable of looking up, popping, or freeing
 *	swapblk assignments in the swap meta data or in the vm_page_t.
 *	The routine typically returns the swapblk being looked-up, or popped,
 *	or SWAPBLK_NONE if the block was freed, or SWAPBLK_NONE if the block
 *	was invalid.  This routine will automatically free any invalid 
 *	meta-data swapblks.
 *
 *	It is not possible to store invalid swapblks in the swap meta data
 *	(other then a literal 'SWAPBLK_NONE'), so we don't bother checking.
 *
 *	When acting on a busy resident page and paging is in progress, we 
 *	have to wait until paging is complete but otherwise can act on the 
 *	busy page.
 *
 *	This routine must be called at splvm().
 *
 *	SWM_FREE	remove and free swap block from metadata
 *	SWM_POP		remove from meta data but do not free.. pop it out
 */
static daddr_t
swp_pager_meta_ctl(
	vm_object_t object,
	vm_pindex_t pindex,
	int flags
) {
	struct swblock **pswap;
	struct swblock *swap;
	daddr_t r1;
	int idx;

	GIANT_REQUIRED;
	/*
	 * The meta data only exists of the object is OBJT_SWAP 
	 * and even then might not be allocated yet.
	 */
	if (object->type != OBJT_SWAP)
		return (SWAPBLK_NONE);

	r1 = SWAPBLK_NONE;
	pswap = swp_pager_hash(object, pindex);

	if ((swap = *pswap) != NULL) {
		idx = pindex & SWAP_META_MASK;
		r1 = swap->swb_pages[idx];

		if (r1 != SWAPBLK_NONE) {
			if (flags & SWM_FREE) {
				swp_pager_freeswapspace(r1, 1);
				r1 = SWAPBLK_NONE;
			}
			if (flags & (SWM_FREE|SWM_POP)) {
				swap->swb_pages[idx] = SWAPBLK_NONE;
				if (--swap->swb_count == 0) {
					*pswap = swap->swb_hnext;
					uma_zfree(swap_zone, swap);
					--object->un_pager.swp.swp_bcount;
				}
			} 
		}
	}
	return (r1);
}

/********************************************************
 *		CHAINING FUNCTIONS			*
 ********************************************************
 *
 *	These functions support recursion of I/O operations
 *	on bp's, typically by chaining one or more 'child' bp's
 *	to the parent.  Synchronous, asynchronous, and semi-synchronous
 *	chaining is possible.
 */

/*
 *	vm_pager_chain_iodone:
 *
 *	io completion routine for child bp.  Currently we fudge a bit
 *	on dealing with b_resid.   Since users of these routines may issue
 *	multiple children simultaneously, sequencing of the error can be lost.
 */
static void
vm_pager_chain_iodone(struct buf *nbp)
{
	struct bio *bp;
	u_int *count;

	bp = nbp->b_caller1;
	count = (u_int *)&(bp->bio_driver1);
	if (bp != NULL) {
		if (nbp->b_ioflags & BIO_ERROR) {
			bp->bio_flags |= BIO_ERROR;
			bp->bio_error = nbp->b_error;
		} else if (nbp->b_resid != 0) {
			bp->bio_flags |= BIO_ERROR;
			bp->bio_error = EINVAL;
		} else {
			bp->bio_resid -= nbp->b_bcount;
		}
		nbp->b_caller1 = NULL;
		--(*count);
		if (bp->bio_flags & BIO_FLAG1) {
			bp->bio_flags &= ~BIO_FLAG1;
			wakeup(bp);
		}
	}
	nbp->b_flags |= B_DONE;
	nbp->b_flags &= ~B_ASYNC;
	relpbuf(nbp, NULL);
}

/*
 *	getchainbuf:
 *
 *	Obtain a physical buffer and chain it to its parent buffer.  When
 *	I/O completes, the parent buffer will be B_SIGNAL'd.  Errors are
 *	automatically propagated to the parent
 */
static struct buf *
getchainbuf(struct bio *bp, struct vnode *vp, int flags)
{
	struct buf *nbp;
	u_int *count;

	GIANT_REQUIRED;
	nbp = getpbuf(NULL);
	count = (u_int *)&(bp->bio_driver1);

	nbp->b_caller1 = bp;
	++(*count);

	if (*count > 4)
		waitchainbuf(bp, 4, 0);

	nbp->b_iocmd = bp->bio_cmd;
	nbp->b_ioflags = 0;
	nbp->b_flags = flags;
	nbp->b_rcred = crhold(thread0.td_ucred);
	nbp->b_wcred = crhold(thread0.td_ucred);
	nbp->b_iodone = vm_pager_chain_iodone;

	if (vp)
		pbgetvp(vp, nbp);
	return (nbp);
}

static void
flushchainbuf(struct buf *nbp)
{
	GIANT_REQUIRED;
	if (nbp->b_bcount) {
		nbp->b_bufsize = nbp->b_bcount;
		if (nbp->b_iocmd == BIO_WRITE)
			nbp->b_dirtyend = nbp->b_bcount;
		BUF_KERNPROC(nbp);
		VOP_STRATEGY(nbp->b_vp, nbp);
	} else {
		bufdone(nbp);
	}
}

static void
waitchainbuf(struct bio *bp, int limit, int done)
{
 	int s;
	u_int *count;

	GIANT_REQUIRED;
	count = (u_int *)&(bp->bio_driver1);
	s = splbio();
	while (*count > limit) {
		bp->bio_flags |= BIO_FLAG1;
		tsleep(bp, PRIBIO + 4, "bpchain", 0);
	}
	if (done) {
		if (bp->bio_resid != 0 && !(bp->bio_flags & BIO_ERROR)) {
			bp->bio_flags |= BIO_ERROR;
			bp->bio_error = EINVAL;
		}
		biodone(bp);
	}
	splx(s);
}

/*
 *	swapdev_strategy:
 *
 *	VOP_STRATEGY() for swapdev_vp.
 *	Perform swap strategy interleave device selection.
 *
 *	The bp is expected to be locked and *not* B_DONE on call.
 */
static int
swapdev_strategy(ap)
	struct vop_strategy_args /* {
		struct vnode *a_vp;
		struct buf *a_bp;
	} */ *ap;
{
	int s, sz, off, seg, index;
	struct swdevt *sp;
	struct vnode *vp;
	struct buf *bp;

	KASSERT(ap->a_vp == ap->a_bp->b_vp, ("%s(%p != %p)",
	    __func__, ap->a_vp, ap->a_bp->b_vp));
	bp = ap->a_bp;
	sz = howmany(bp->b_bcount, PAGE_SIZE);

	/*
	 * Convert interleaved swap into per-device swap.  Note that
	 * the block size is left in PAGE_SIZE'd chunks (for the newswap)
	 * here.
	 */
	if (NSWAPDEV > 1) {
		off = bp->b_blkno % dmmax;
		if (off + sz > dmmax) {
			bp->b_error = EINVAL;
			bp->b_ioflags |= BIO_ERROR;
			bufdone(bp);
			return 0;
		}
		seg = bp->b_blkno / dmmax;
		index = seg % NSWAPDEV;
		seg /= NSWAPDEV;
		bp->b_blkno = seg * dmmax + off;
	} else {
		index = 0;
	}
	sp = &swdevt[index];
	if (bp->b_blkno + sz > sp->sw_nblks) {
		bp->b_error = EINVAL;
		bp->b_ioflags |= BIO_ERROR;
		bufdone(bp);
		return 0;
	}
	bp->b_dev = sp->sw_device;
	if (sp->sw_vp == NULL) {
		bp->b_error = ENODEV;
		bp->b_ioflags |= BIO_ERROR;
		bufdone(bp);
		return 0;
	}

	/*
	 * Convert from PAGE_SIZE'd to DEV_BSIZE'd chunks for the actual I/O
	 */
	bp->b_blkno = ctodb(bp->b_blkno);

	vhold(sp->sw_vp);
	s = splvm();
	if (bp->b_iocmd == BIO_WRITE) {
		vp = bp->b_vp;
		if (vp) {
			VI_LOCK(vp);
			vp->v_numoutput--;
			if ((vp->v_iflag & VI_BWAIT) && vp->v_numoutput <= 0) {
				vp->v_iflag &= ~VI_BWAIT;
				wakeup(&vp->v_numoutput);
			}
			VI_UNLOCK(vp);
		}
		VI_LOCK(sp->sw_vp);
		sp->sw_vp->v_numoutput++;
		VI_UNLOCK(sp->sw_vp);
	}
	bp->b_vp = sp->sw_vp;
	splx(s);
	if (bp->b_vp->v_type == VCHR)
		VOP_SPECSTRATEGY(bp->b_vp, bp);
	else
		VOP_STRATEGY(bp->b_vp, bp);
	return 0;
}

/*
 * Create a special vnode op vector for swapdev_vp - we only use
 * VOP_STRATEGY() and reclaim; everything else returns an error.
 */
vop_t **swapdev_vnodeop_p;
static struct vnodeopv_entry_desc swapdev_vnodeop_entries[] = {  
	{ &vop_default_desc,		(vop_t *) vop_defaultop },
	{ &vop_reclaim_desc,		(vop_t *) vop_null },
	{ &vop_strategy_desc,		(vop_t *) swapdev_strategy },
	{ NULL, NULL }
};
static struct vnodeopv_desc swapdev_vnodeop_opv_desc =
	{ &swapdev_vnodeop_p, swapdev_vnodeop_entries };

VNODEOP_SET(swapdev_vnodeop_opv_desc);

/*
 * System call swapon(name) enables swapping on device name,
 * which must be in the swdevsw.  Return EBUSY
 * if already swapping on this device.
 */
#ifndef _SYS_SYSPROTO_H_
struct swapon_args {
	char *name;
};
#endif

/* 
 * MPSAFE
 */
/* ARGSUSED */
int
swapon(td, uap)
	struct thread *td;
	struct swapon_args *uap;
{
	struct vattr attr;
	struct vnode *vp;
	struct nameidata nd;
	int error;

	mtx_lock(&Giant);
	error = suser(td);
	if (error)
		goto done2;

	while (swdev_syscall_active)
	    tsleep(&swdev_syscall_active, PUSER - 1, "swpon", 0);
	swdev_syscall_active = 1;

	/*
	 * Swap metadata may not fit in the KVM if we have physical
	 * memory of >1GB.
	 */
	if (swap_zone == NULL) {
		error = ENOMEM;
		goto done;
	}

	NDINIT(&nd, LOOKUP, FOLLOW, UIO_USERSPACE, uap->name, td);
	error = namei(&nd);
	if (error)
		goto done;

	NDFREE(&nd, NDF_ONLY_PNBUF);
	vp = nd.ni_vp;

	if (vn_isdisk(vp, &error))
		error = swaponvp(td, vp, vp->v_rdev, 0);
	else if (vp->v_type == VREG &&
	    (vp->v_mount->mnt_vfc->vfc_flags & VFCF_NETWORK) != 0 &&
	    (error = VOP_GETATTR(vp, &attr, td->td_ucred, td)) == 0) {
		/*
		 * Allow direct swapping to NFS regular files in the same
		 * way that nfs_mountroot() sets up diskless swapping.
		 */
		error = swaponvp(td, vp, NODEV, attr.va_size / DEV_BSIZE);
	}

	if (error)
		vrele(vp);
done:
	swdev_syscall_active = 0;
	wakeup_one(&swdev_syscall_active);
done2:
	mtx_unlock(&Giant);
	return (error);
}

/*
 * Swfree(index) frees the index'th portion of the swap map.
 * Each of the NSWAPDEV devices provides 1/NSWAPDEV'th of the swap
 * space, which is laid out with blocks of dmmax pages circularly
 * among the devices.
 *
 * The new swap code uses page-sized blocks.  The old swap code used
 * DEV_BSIZE'd chunks.
 */
int
swaponvp(td, vp, dev, nblks)
	struct thread *td;
	struct vnode *vp;
	dev_t dev;
	u_long nblks;
{
	int index;
	struct swdevt *sp;
	swblk_t vsbase;
	long blk;
	swblk_t dvbase;
	int error;
	u_long aligned_nblks, mblocks;
	off_t mediasize;

	if (!swapdev_vp) {
		error = getnewvnode("none", NULL, swapdev_vnodeop_p,
		    &swapdev_vp);
		if (error)
			panic("Cannot get vnode for swapdev");
		swapdev_vp->v_type = VNON;	/* Untyped */
	}

	ASSERT_VOP_UNLOCKED(vp, "swaponvp");
	for (sp = swdevt, index = 0 ; index < NSWAPDEV; index++, sp++) {
		if (sp->sw_vp == vp)
			return EBUSY;
		if (!sp->sw_vp)
			goto found;

	}
	return EINVAL;
    found:
	(void) vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
#ifdef MAC
	error = mac_check_system_swapon(td->td_ucred, vp);
	if (error == 0)
#endif
		error = VOP_OPEN(vp, FREAD | FWRITE, td->td_ucred, td);
	(void) VOP_UNLOCK(vp, 0, td);
	if (error)
		return (error);

	if (nblks == 0) {
		error = VOP_IOCTL(vp, DIOCGMEDIASIZE, (caddr_t)&mediasize,
		    FREAD, td->td_ucred, td);
		if (error == 0)
		    nblks = mediasize / DEV_BSIZE;
	}
	/*
	 * XXX: We should also check that the sectorsize makes sense
	 * XXX: it should be a power of two, no larger than the page size.
	 */
	if (nblks == 0) {
		(void) VOP_CLOSE(vp, FREAD | FWRITE, td->td_ucred, td);
		return (ENXIO);
	}

	/*
	 * If we go beyond this, we get overflows in the radix
	 * tree bitmap code.
	 */
	mblocks = 0x40000000 / BLIST_META_RADIX / NSWAPDEV;
	if (nblks > mblocks) {
		printf("WARNING: reducing size to maximum of %lu blocks per swap unit\n",
			mblocks);
		nblks = mblocks;
	}
	/*
	 * nblks is in DEV_BSIZE'd chunks, convert to PAGE_SIZE'd chunks.
	 * First chop nblks off to page-align it, then convert.
	 * 
	 * sw->sw_nblks is in page-sized chunks now too.
	 */
	nblks &= ~(ctodb(1) - 1);
	nblks = dbtoc(nblks);

	sp->sw_vp = vp;
	sp->sw_dev = dev2udev(dev);
	sp->sw_device = dev;
	sp->sw_flags = SW_FREED;
	sp->sw_nblks = nblks;
	sp->sw_used = 0;

	/*
	 * nblks, nswap, and dmmax are PAGE_SIZE'd parameters now, not
	 * DEV_BSIZE'd.   aligned_nblks is used to calculate the
	 * size of the swap bitmap, taking into account the stripe size.
	 */
	aligned_nblks = (nblks + (dmmax -1)) & ~(u_long)(dmmax -1);

	if (aligned_nblks * NSWAPDEV > nswap)
		nswap = aligned_nblks * NSWAPDEV;

	if (swapblist == NULL)
		swapblist = blist_create(nswap);
	else
		blist_resize(&swapblist, nswap, 0);

	for (dvbase = dmmax; dvbase < nblks; dvbase += dmmax) {
		blk = min(nblks - dvbase, dmmax);
		vsbase = index * dmmax + dvbase * NSWAPDEV;
		blist_free(swapblist, vsbase, blk);
		vm_swap_size += blk;
	}

	swap_pager_full = 0;

	return (0);
}

/*
 * SYSCALL: swapoff(devname)
 *
 * Disable swapping on the given device.
 */
#ifndef _SYS_SYSPROTO_H_
struct swapoff_args {
	char *name;
};
#endif

/*
 * MPSAFE
 */
/* ARGSUSED */
int
swapoff(td, uap)
	struct thread *td;
	struct swapoff_args *uap;
{
	struct vnode *vp;
	struct nameidata nd;
	struct swdevt *sp;
	swblk_t dvbase, vsbase;
	u_long nblks, aligned_nblks, blk;
	int error, index;

	mtx_lock(&Giant);

	error = suser(td);
	if (error)
		goto done2;

	while (swdev_syscall_active)
	    tsleep(&swdev_syscall_active, PUSER - 1, "swpoff", 0);
	swdev_syscall_active = 1;

	NDINIT(&nd, LOOKUP, FOLLOW, UIO_USERSPACE, uap->name, td);
	error = namei(&nd);
	if (error)
		goto done;
	NDFREE(&nd, NDF_ONLY_PNBUF);
	vp = nd.ni_vp;

	for (sp = swdevt, index = 0 ; index < NSWAPDEV; index++, sp++) {
		if (sp->sw_vp == vp)
			goto found;
	}
	error = EINVAL;
	goto done;
found:
#ifdef MAC
	(void) vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
	error = mac_check_system_swapoff(td->td_ucred, vp);
	(void) VOP_UNLOCK(vp, 0, td);
	if (error != 0)
		goto done;
#endif
	
	nblks = sp->sw_nblks;

	/*
	 * We can turn off this swap device safely only if the
	 * available virtual memory in the system will fit the amount
	 * of data we will have to page back in, plus an epsilon so
	 * the system doesn't become critically low on swap space.
	 */
	if (cnt.v_free_count + cnt.v_cache_count + vm_swap_size <
	    nblks + nswap_lowat) {
		error = ENOMEM;
		goto done;
	}

	/*
	 * Prevent further allocations on this device.
	 */
	sp->sw_flags |= SW_CLOSING;
	for (dvbase = dmmax; dvbase < nblks; dvbase += dmmax) {
		blk = min(nblks - dvbase, dmmax);
		vsbase = index * dmmax + dvbase * NSWAPDEV;
		vm_swap_size -= blist_fill(swapblist, vsbase, blk);
	}

	/*
	 * Page in the contents of the device and close it.
	 */
#ifndef NO_SWAPPING
       	vm_proc_swapin_all(index);
#endif /* !NO_SWAPPING */
	swap_pager_swapoff(index, &sp->sw_used);

	VOP_CLOSE(vp, FREAD | FWRITE, td->td_ucred, td);
	vrele(vp);
	sp->sw_vp = NULL;

	/*
	 * Resize the bitmap based on the new largest swap device,
	 * or free the bitmap if there are no more devices.
	 */
	for (sp = swdevt, nblks = 0; sp < swdevt + NSWAPDEV; sp++) {
		if (sp->sw_vp == NULL)
			continue;
		nblks = max(nblks, sp->sw_nblks);
	}

	aligned_nblks = (nblks + (dmmax -1)) & ~(u_long)(dmmax -1);
	nswap = aligned_nblks * NSWAPDEV;

	if (nswap == 0) {
		blist_destroy(swapblist);
		swapblist = NULL;
		vrele(swapdev_vp);
		swapdev_vp = NULL;
	} else
		blist_resize(&swapblist, nswap, 0);

done:
	swdev_syscall_active = 0;
	wakeup_one(&swdev_syscall_active);
done2:
	mtx_unlock(&Giant);
	return (error);
}

void
swap_pager_status(int *total, int *used)
{
	struct swdevt *sp;
	int i;

	*total = 0;
	*used = 0;
	for (sp = swdevt, i = 0; i < NSWAPDEV; i++, sp++) {
		if (sp->sw_vp == NULL)
			continue;
		*total += sp->sw_nblks;
		*used += sp->sw_used;
	}
}

static int
sysctl_vm_swap_info(SYSCTL_HANDLER_ARGS)
{
	int	*name = (int *)arg1;
	int	error, i, n;
	struct xswdev xs;
	struct swdevt *sp;

	if (arg2 != 1) /* name length */
		return (EINVAL);

	for (sp = swdevt, i = 0, n = 0 ; i < NSWAPDEV; i++, sp++) {
		if (sp->sw_vp) {
			if (n == *name) {
				xs.xsw_version = XSWDEV_VERSION;
				xs.xsw_dev = sp->sw_dev;
				xs.xsw_flags = sp->sw_flags;
				xs.xsw_nblks = sp->sw_nblks;
				xs.xsw_used = sp->sw_used;

				error = SYSCTL_OUT(req, &xs, sizeof(xs));
				return (error);
			}
			n++;
		}

	}
	return (ENOENT);
}

SYSCTL_INT(_vm, OID_AUTO, nswapdev, CTLFLAG_RD, 0, NSWAPDEV,
    "Number of swap devices");
SYSCTL_NODE(_vm, OID_AUTO, swap_info, CTLFLAG_RD, sysctl_vm_swap_info,
    "Swap statistics by device");

/*
 * vmspace_swap_count() - count the approximate swap useage in pages for a
 *			  vmspace.
 *
 *	The map must be locked.
 *
 *	Swap useage is determined by taking the proportional swap used by
 *	VM objects backing the VM map.  To make up for fractional losses,
 *	if the VM object has any swap use at all the associated map entries
 *	count for at least 1 swap page.
 */
int
vmspace_swap_count(struct vmspace *vmspace)
{
	vm_map_t map = &vmspace->vm_map;
	vm_map_entry_t cur;
	int count = 0;

	for (cur = map->header.next; cur != &map->header; cur = cur->next) {
		vm_object_t object;

		if ((cur->eflags & MAP_ENTRY_IS_SUB_MAP) == 0 &&
		    (object = cur->object.vm_object) != NULL) {
			VM_OBJECT_LOCK(object);
			if (object->type == OBJT_SWAP &&
			    object->un_pager.swp.swp_bcount != 0) {
				int n = (cur->end - cur->start) / PAGE_SIZE;

				count += object->un_pager.swp.swp_bcount *
				    SWAP_META_PAGES * n / object->size + 1;
			}
			VM_OBJECT_UNLOCK(object);
		}
	}
	return (count);
}

