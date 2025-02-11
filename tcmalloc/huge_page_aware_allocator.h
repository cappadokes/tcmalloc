// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_HUGE_PAGE_AWARE_ALLOCATOR_H_
#define TCMALLOC_HUGE_PAGE_AWARE_ALLOCATOR_H_

#include <stddef.h>

#include "absl/base/thread_annotations.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/lifetime_based_allocator.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace huge_page_allocator_internal {

LifetimePredictionOptions decide_lifetime_predictions();
bool decide_subrelease();

HugeRegionUsageOption huge_region_option();

class StaticForwarder {
 public:
  // Runtime parameters.  This can change between calls.
  static absl::Duration filler_skip_subrelease_interval() {
    return Parameters::filler_skip_subrelease_interval();
  }
  static absl::Duration filler_skip_subrelease_short_interval() {
    return Parameters::filler_skip_subrelease_short_interval();
  }
  static absl::Duration filler_skip_subrelease_long_interval() {
    return Parameters::filler_skip_subrelease_long_interval();
  }

  static bool release_partial_alloc_pages() {
    return Parameters::release_partial_alloc_pages();
  }

  static bool hpaa_subrelease() { return Parameters::hpaa_subrelease(); }

  // Arena state.
  static Arena& arena();

  // PageAllocator state.

  // Check page heap memory limit.  `n` indicates the size of the allocation
  // currently being made, which will not be included in the sampled memory heap
  // for realized fragmentation estimation.
  static void ShrinkToUsageLimit(Length n)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // PageMap state.
  static void* GetHugepage(HugePage p);
  static bool Ensure(PageId page, Length length)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  static void Set(PageId page, Span* span);
  static void SetHugepage(HugePage p, void* pt);

  // SpanAllocator state.
  static Span* NewSpan(PageId page, Length length)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock)
          ABSL_ATTRIBUTE_RETURNS_NONNULL;
  static void DeleteSpan(Span* span)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) ABSL_ATTRIBUTE_NONNULL();

  // SystemAlloc state.
  static AddressRange AllocatePages(size_t bytes, size_t align, MemoryTag tag) {
    return SystemAlloc(bytes, align, tag);
  }
  // TODO(ckennelly): Accept PageId/Length.
  static bool ReleasePages(void* ptr, size_t size) {
    return SystemRelease(ptr, size);
  }
};

struct HugePageAwareAllocatorOptions {
  MemoryTag tag;
  HugeRegionUsageOption use_huge_region_more_often = huge_region_option();
  LifetimePredictionOptions lifetime_options = decide_lifetime_predictions();
  // TODO(b/242550501): Strongly type
  bool separate_allocs_for_few_and_many_objects_spans =
      Parameters::separate_allocs_for_few_and_many_objects_spans();
};

// An implementation of the PageAllocator interface that is hugepage-efficient.
// Attempts to pack allocations into full hugepages wherever possible,
// and aggressively returns empty ones to the system.
//
// Some notes: locking discipline here is a bit funny, because
// we want to *not* hold the pageheap lock while backing memory.
//
// We have here a collection of slightly different allocators each
// optimized for slightly different purposes.  This file has two main purposes:
// - pick the right one for a given allocation
// - provide enough data to figure out what we picked last time!

template <typename Forwarder>
class HugePageAwareAllocator final : public PageAllocatorInterface {
 public:
  explicit HugePageAwareAllocator(const HugePageAwareAllocatorOptions& options);
  ~HugePageAwareAllocator() override = default;

  // Allocate a run of "n" pages.  Returns zero if out of memory.
  // Caller should not pass "n == 0" -- instead, n should have
  // been rounded up already.
  Span* New(Length n, size_t objects_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) override;

  // As New, but the returned span is aligned to a <align>-page boundary.
  // <align> must be a power of two.
  Span* NewAligned(Length n, Length align, size_t objects_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) override;

  // Delete the span "[p, p+n-1]".
  // REQUIRES: span was returned by earlier call to New() and
  //           has not yet been deleted.
  void Delete(Span* span, size_t objects_per_span)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  BackingStats stats() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  void GetSmallSpanStats(SmallSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  void GetLargeSpanStats(LargeSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  // Try to release at least num_pages for reuse by the OS.  Returns
  // the actual number of pages released, which may be less than
  // num_pages if there weren't enough pages to release. The result
  // may also be larger than num_pages since page_heap might decide to
  // release one large range instead of fragmenting it into two
  // smaller released and unreleased ranges.
  Length ReleaseAtLeastNPages(Length num_pages)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override;

  Length ReleaseAtLeastNPagesBreakingHugepages(Length n)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Prints stats about the page heap to *out.
  void Print(Printer* out) ABSL_LOCKS_EXCLUDED(pageheap_lock) override;

  // Print stats to *out, excluding long/likely uninteresting things
  // unless <everything> is true.
  void Print(Printer* out, bool everything) ABSL_LOCKS_EXCLUDED(pageheap_lock);

  void PrintInPbtxt(PbtxtRegion* region)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) override;

  BackingStats FillerStats() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return filler_.stats();
  }

  HugeLength DonatedHugePages() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return donated_huge_pages_;
  }

  // Number of pages that have been retained on huge pages by donations that did
  // not reassemble by the time the larger allocation was deallocated.
  Length AbandonedPages() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return abandoned_pages_;
  }

  const HugeCache* cache() const { return &cache_; }

  LifetimeBasedAllocator& lifetime_based_allocator() {
    return lifetime_allocator_;
  }

  const HugeRegionSet<HugeRegion>& region() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return regions_;
  };

 private:
  typedef HugePageFiller<PageTracker> FillerType;
  FillerType filler_ ABSL_GUARDED_BY(pageheap_lock);

  class RegionAllocImpl final : public LifetimeBasedAllocator::RegionAlloc {
   public:
    explicit RegionAllocImpl(HugePageAwareAllocator* p) : p_(p) {}

    // We need to explicitly instantiate the destructor here so that it gets
    // placed within GOOGLE_MALLOC_SECTION.
    ~RegionAllocImpl() override {}

    HugeRegion* AllocRegion(HugeLength n, HugeRange* range) override
        ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
      if (!range->valid()) {
        *range = p_->alloc_.Get(n);
      }
      if (!range->valid()) return nullptr;
      HugeRegion* region = p_->region_allocator_.New();
      new (region) HugeRegion(*range, MemoryModifyFunction(SystemRelease));
      return region;
    }

   private:
    HugePageAwareAllocator* p_;
  };

  class VirtualMemoryAllocator final : public VirtualAllocator {
   public:
    explicit VirtualMemoryAllocator(
        HugePageAwareAllocator& hpaa ABSL_ATTRIBUTE_LIFETIME_BOUND)
        : hpaa_(hpaa) {}

    ABSL_MUST_USE_RESULT AddressRange operator()(size_t bytes,
                                                 size_t align) override
        ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
      return hpaa_.AllocAndReport(bytes, align);
    }

   private:
    HugePageAwareAllocator& hpaa_;
  };

  class ArenaMetadataAllocator final : public MetadataAllocator {
   public:
    explicit ArenaMetadataAllocator(
        HugePageAwareAllocator& hpaa ABSL_ATTRIBUTE_LIFETIME_BOUND)
        : hpaa_(hpaa) {}

    ABSL_MUST_USE_RESULT void* operator()(size_t bytes) override
        ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
      return hpaa_.forwarder_.arena().Alloc(bytes);
    }

   public:
    HugePageAwareAllocator& hpaa_;
  };

  // Calls SystemRelease, but with dropping of pageheap_lock around the call.
  static ABSL_MUST_USE_RESULT bool UnbackWithoutLock(void* start, size_t length)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  HugeRegionSet<HugeRegion> regions_ ABSL_GUARDED_BY(pageheap_lock);

  PageHeapAllocator<FillerType::Tracker> tracker_allocator_
      ABSL_GUARDED_BY(pageheap_lock);
  PageHeapAllocator<HugeRegion> region_allocator_
      ABSL_GUARDED_BY(pageheap_lock);

  FillerType::Tracker* GetTracker(HugePage p);

  void SetTracker(HugePage p, FillerType::Tracker* pt);

  AddressRange AllocAndReport(size_t bytes, size_t align)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  VirtualMemoryAllocator vm_allocator_ ABSL_GUARDED_BY(pageheap_lock);
  ArenaMetadataAllocator metadata_allocator_ ABSL_GUARDED_BY(pageheap_lock);
  HugeAllocator alloc_ ABSL_GUARDED_BY(pageheap_lock);
  HugeCache cache_ ABSL_GUARDED_BY(pageheap_lock);

  // donated_huge_pages_ measures the number of huge pages contributed to the
  // filler from left overs of large huge page allocations.  When the large
  // allocation is deallocated, we decrement this count *if* we were able to
  // fully reassemble the address range (that is, the partial hugepage did not
  // get stuck in the filler).
  HugeLength donated_huge_pages_ ABSL_GUARDED_BY(pageheap_lock);
  // abandoned_pages_ tracks the number of pages contributed to the filler after
  // a donating allocation is deallocated but the entire huge page has not been
  // reassembled.
  Length abandoned_pages_ ABSL_GUARDED_BY(pageheap_lock);

  // Performs lifetime predictions for large objects and places short-lived
  // objects into a separate region to reduce filler contention.
  RegionAllocImpl lifetime_allocator_region_alloc_;
  LifetimeBasedAllocator lifetime_allocator_;

  void GetSpanStats(SmallSpanStats* small, LargeSpanStats* large,
                    PageAgeHistograms* ages)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  PageId RefillFiller(Length n, size_t num_objects, bool* from_released)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Allocate the first <n> from p, and contribute the rest to the filler.  If
  // "donated" is true, the contribution will be marked as coming from the
  // tail of a multi-hugepage alloc.  Returns the allocated section.
  PageId AllocAndContribute(HugePage p, Length n, size_t num_objects,
                            bool donated)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  // Helpers for New().

  Span* LockAndAlloc(Length n, size_t objects_per_span, bool* from_released);

  Span* AllocSmall(Length n, size_t objects_per_span, bool* from_released)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  Span* AllocLarge(Length n, size_t objects_per_span, bool* from_released,
                   LifetimeStats* lifetime_context)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  Span* AllocEnormous(Length n, size_t objects_per_span, bool* from_released)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  Span* AllocRawHugepages(Length n, size_t num_objects, bool* from_released)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Allocates a span and adds a tracker. This span has to be associated with a
  // filler donation and have an associated page tracker. A tracker will only be
  // added if there is an associated lifetime prediction.
  Span* AllocRawHugepagesAndMaybeTrackLifetime(
      Length n, size_t num_objects,
      const LifetimeBasedAllocator::AllocationResult& lifetime_alloc,
      bool* from_released) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  bool AddRegion() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void ReleaseHugepage(FillerType::Tracker* pt)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  // Return an allocation from a single hugepage.
  void DeleteFromHugepage(FillerType::Tracker* pt, PageId p, Length n,
                          size_t num_objects, bool might_abandon)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Finish an allocation request - give it a span and mark it in the pagemap.
  Span* Finalize(Length n, size_t num_objects, PageId page);

  ABSL_ATTRIBUTE_NO_UNIQUE_ADDRESS Forwarder forwarder_;
};

template <class Forwarder>
inline HugePageAwareAllocator<Forwarder>::HugePageAwareAllocator(
    const HugePageAwareAllocatorOptions& options)
    : PageAllocatorInterface("HugePageAware", options.tag),
      filler_(options.separate_allocs_for_few_and_many_objects_spans,
              MemoryModifyFunction(&forwarder_.ReleasePages)),
      regions_(options.use_huge_region_more_often),
      vm_allocator_(*this),
      metadata_allocator_(*this),
      alloc_(vm_allocator_, metadata_allocator_),
      cache_(HugeCache{&alloc_, metadata_allocator_,
                       MemoryModifyFunction(UnbackWithoutLock)}),
      lifetime_allocator_region_alloc_(this),
      lifetime_allocator_(options.lifetime_options,
                          &lifetime_allocator_region_alloc_) {
  tracker_allocator_.Init(&forwarder_.arena());
  region_allocator_.Init(&forwarder_.arena());
}

template <class Forwarder>
inline HugePageAwareAllocator<Forwarder>::FillerType::Tracker*
HugePageAwareAllocator<Forwarder>::GetTracker(HugePage p) {
  void* v = forwarder_.GetHugepage(p);
  FillerType::Tracker* pt = reinterpret_cast<FillerType::Tracker*>(v);
  ASSERT(pt == nullptr || pt->location() == p);
  return pt;
}

template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::SetTracker(
    HugePage p, HugePageAwareAllocator<Forwarder>::FillerType::Tracker* pt) {
  forwarder_.SetHugepage(p, pt);
}

template <class Forwarder>
inline PageId HugePageAwareAllocator<Forwarder>::AllocAndContribute(
    HugePage p, Length n, size_t num_objects, bool donated) {
  CHECK_CONDITION(p.start_addr() != nullptr);
  FillerType::Tracker* pt = tracker_allocator_.New();
  new (pt)
      FillerType::Tracker(p, absl::base_internal::CycleClock::Now(), donated);
  ASSERT(pt->longest_free_range() >= n);
  ASSERT(pt->was_donated() == donated);
  // if the page was donated, we track its size so that we can potentially
  // measure it in abandoned_count_ once this large allocation gets deallocated.
  if (pt->was_donated()) {
    pt->set_abandoned_count(n);
  }
  PageId page = pt->Get(n).page;
  ASSERT(page == p.first_page());
  SetTracker(p, pt);
  filler_.Contribute(pt, donated, num_objects);
  ASSERT(pt->was_donated() == donated);
  return page;
}

template <class Forwarder>
inline PageId HugePageAwareAllocator<Forwarder>::RefillFiller(
    Length n, size_t num_objects, bool* from_released) {
  HugeRange r = cache_.Get(NHugePages(1), from_released);
  if (!r.valid()) return PageId{0};
  // This is duplicate to Finalize, but if we need to break up
  // hugepages to get to our usage limit it would be very bad to break
  // up what's left of r after we allocate from there--while r is
  // mostly empty, clearly what's left in the filler is too fragmented
  // to be very useful, and we would rather release those
  // pages. Otherwise, we're nearly guaranteed to release r (if n
  // isn't very large), and the next allocation will just repeat this
  // process.
  forwarder_.ShrinkToUsageLimit(n);
  return AllocAndContribute(r.start(), n, num_objects, /*donated=*/false);
}

template <class Forwarder>
inline Span* HugePageAwareAllocator<Forwarder>::Finalize(Length n,
                                                         size_t num_objects,
                                                         PageId page)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
  ASSERT(page != PageId{0});
  Span* ret = forwarder_.NewSpan(page, n);
  forwarder_.Set(page, ret);
  ASSERT(!ret->sampled());
  info_.RecordAlloc(page, n, num_objects);
  forwarder_.ShrinkToUsageLimit(n);
  return ret;
}

// For anything <= half a huge page, we will unconditionally use the filler
// to pack it into a single page.  If we need another page, that's fine.
template <class Forwarder>
inline Span* HugePageAwareAllocator<Forwarder>::AllocSmall(
    Length n, size_t objects_per_span, bool* from_released) {
  auto [pt, page] = filler_.TryGet(n, objects_per_span);
  if (ABSL_PREDICT_TRUE(pt != nullptr)) {
    *from_released = false;
    return Finalize(n, objects_per_span, page);
  }

  page = RefillFiller(n, objects_per_span, from_released);
  if (ABSL_PREDICT_FALSE(page == PageId{0})) {
    return nullptr;
  }
  return Finalize(n, objects_per_span, page);
}

template <class Forwarder>
inline Span* HugePageAwareAllocator<Forwarder>::AllocLarge(
    Length n, size_t objects_per_span, bool* from_released,
    LifetimeStats* lifetime_context) {
  // If it's an exact page multiple, just pull it from pages directly.
  HugeLength hl = HLFromPages(n);
  if (hl.in_pages() == n) {
    return AllocRawHugepages(n, objects_per_span, from_released);
  }

  PageId page;
  // If we fit in a single hugepage, try the Filler first.
  if (n < kPagesPerHugePage) {
    auto [pt, page] = filler_.TryGet(n, objects_per_span);
    if (ABSL_PREDICT_TRUE(pt != nullptr)) {
      *from_released = false;
      return Finalize(n, objects_per_span, page);
    }
  }

  // Try to perform a lifetime-based allocation.
  LifetimeBasedAllocator::AllocationResult lifetime =
      lifetime_allocator_.MaybeGet(n, from_released, lifetime_context);

  // TODO(mmaas): Implement tracking if this is subsequently put into a
  // conventional region (currently ignored).

  // Was an object allocated in the lifetime region? If so, we return it.
  if (lifetime.TryGetAllocation(&page)) {
    return Finalize(n, objects_per_span, page);
  }

  // If we're using regions in this binary (see below comment), is
  // there currently available space there?
  if (regions_.MaybeGet(n, &page, from_released)) {
    return Finalize(n, objects_per_span, page);
  }

  // We have two choices here: allocate a new region or go to
  // hugepages directly (hoping that slack will be filled by small
  // allocation.) The second strategy is preferrable, as it's
  // typically faster and usually more space efficient, but it's sometimes
  // catastrophic.
  //
  // See https://github.com/google/tcmalloc/tree/master/docs/regions-are-not-optional.md
  //
  // So test directly if we're in the bad case--almost no binaries are.
  // If not, just fall back to direct allocation (and hope we do hit that case!)
  const Length slack = info_.slack();
  const Length donated =
      regions_.UseHugeRegionMoreOften() ? abandoned_pages_ + slack : slack;
  // Don't bother at all until the binary is reasonably sized.
  if (donated < HLFromBytes(64 * 1024 * 1024).in_pages()) {
    return AllocRawHugepagesAndMaybeTrackLifetime(n, objects_per_span, lifetime,
                                                  from_released);
  }

  // In the vast majority of binaries, we have many small allocations which
  // will nicely fill slack.  (Fleetwide, the average ratio is 15:1; only
  // a handful of binaries fall below 1:1.)
  //
  // If we enable an experiment that tries to use huge regions more frequently,
  // we skip the check.
  const Length small = info_.small();
  if (slack < small && !regions_.UseHugeRegionMoreOften()) {
    return AllocRawHugepagesAndMaybeTrackLifetime(n, objects_per_span, lifetime,
                                                  from_released);
  }

  // We couldn't allocate a new region. They're oversized, so maybe we'd get
  // lucky with a smaller request?
  if (!AddRegion()) {
    return AllocRawHugepagesAndMaybeTrackLifetime(n, objects_per_span, lifetime,
                                                  from_released);
  }

  CHECK_CONDITION(regions_.MaybeGet(n, &page, from_released));
  return Finalize(n, objects_per_span, page);
}

template <class Forwarder>
inline Span* HugePageAwareAllocator<Forwarder>::AllocEnormous(
    Length n, size_t objects_per_span, bool* from_released) {
  return AllocRawHugepages(n, objects_per_span, from_released);
}

template <class Forwarder>
inline Span* HugePageAwareAllocator<Forwarder>::AllocRawHugepages(
    Length n, size_t num_objects, bool* from_released) {
  HugeLength hl = HLFromPages(n);

  HugeRange r = cache_.Get(hl, from_released);
  if (!r.valid()) return nullptr;

  // We now have a huge page range that covers our request.  There
  // might be some slack in it if n isn't a multiple of
  // kPagesPerHugePage. Add the hugepage with slack to the filler,
  // pretending the non-slack portion is a smaller allocation.
  Length total = hl.in_pages();
  Length slack = total - n;
  HugePage first = r.start();
  SetTracker(first, nullptr);
  HugePage last = first + r.len() - NHugePages(1);
  if (slack == Length(0)) {
    SetTracker(last, nullptr);
    return Finalize(total, num_objects, r.start().first_page());
  }

  ++donated_huge_pages_;

  Length here = kPagesPerHugePage - slack;
  ASSERT(here > Length(0));
  AllocAndContribute(last, here, num_objects, /*donated=*/true);
  Span* span = Finalize(n, num_objects, r.start().first_page());
  span->set_donated(/*value=*/true);
  return span;
}

template <class Forwarder>
inline Span*
HugePageAwareAllocator<Forwarder>::AllocRawHugepagesAndMaybeTrackLifetime(
    Length n, size_t num_objects,
    const LifetimeBasedAllocator::AllocationResult& lifetime_alloc,
    bool* from_released) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
  Span* result = AllocRawHugepages(n, num_objects, from_released);

  if (result != nullptr) {
    // If this is an object with a lifetime prediction and led to a donation,
    // add it to the tracker so that we can track its lifetime.
    HugePage hp = HugePageContaining(result->last_page());
    FillerType::Tracker* pt = GetTracker(hp);
    ASSERT(pt != nullptr);

    // The allocator may shrink the heap in response to allocations, which may
    // cause the page to be subreleased and not donated anymore once we get
    // here. If it still is, we attach a lifetime tracker (if enabled).
    if (ABSL_PREDICT_TRUE(pt->donated())) {
      lifetime_allocator_.MaybeAddTracker(lifetime_alloc,
                                          pt->lifetime_tracker());
    }
  }

  return result;
}

inline static void BackSpan(Span* span) {
  SystemBack(span->start_address(), span->bytes_in_span());
}

// public
template <class Forwarder>
inline Span* HugePageAwareAllocator<Forwarder>::New(Length n,
                                                    size_t objects_per_span) {
  CHECK_CONDITION(n > Length(0));
  bool from_released;
  Span* s = LockAndAlloc(n, objects_per_span, &from_released);
  if (s) {
    // Prefetch for writing, as we anticipate using the memory soon.
    PrefetchW(s->start_address());
    // TODO(b/256233439):  Improve accuracy of from_released value.  The filler
    // may have subreleased pages and is returning them now.
    if (from_released) BackSpan(s);
  }
  ASSERT(!s || GetMemoryTag(s->start_address()) == tag_);
  return s;
}

template <class Forwarder>
inline Span* HugePageAwareAllocator<Forwarder>::LockAndAlloc(
    Length n, size_t objects_per_span, bool* from_released) {
  // Check whether we may perform lifetime-based allocation, and if so, collect
  // the allocation context without holding the lock.
  LifetimeStats* lifetime_ctx = lifetime_allocator_.CollectLifetimeContext(n);

  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  // Our policy depends on size.  For small things, we will pack them
  // into single hugepages.
  if (n <= kPagesPerHugePage / 2) {
    return AllocSmall(n, objects_per_span, from_released);
  }

  // For anything too big for the filler, we use either a direct hugepage
  // allocation, or possibly the regions if we are worried about slack.
  if (n <= HugeRegion::size().in_pages()) {
    return AllocLarge(n, objects_per_span, from_released, lifetime_ctx);
  }

  // In the worst case, we just fall back to directly allocating a run
  // of hugepages.
  return AllocEnormous(n, objects_per_span, from_released);
}

// public
template <class Forwarder>
inline Span* HugePageAwareAllocator<Forwarder>::NewAligned(
    Length n, Length align, size_t objects_per_span) {
  if (align <= Length(1)) {
    return New(n, objects_per_span);
  }

  // we can do better than this, but...
  // TODO(b/134690769): support higher align.
  CHECK_CONDITION(align <= kPagesPerHugePage);
  bool from_released;
  Span* s;
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    s = AllocRawHugepages(n, objects_per_span, &from_released);
  }
  if (s && from_released) BackSpan(s);
  ASSERT(!s || GetMemoryTag(s->start_address()) == tag_);
  return s;
}

template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::DeleteFromHugepage(
    FillerType::Tracker* pt, PageId p, Length n, size_t num_objects,
    bool might_abandon) {
  if (ABSL_PREDICT_TRUE(filler_.Put(pt, p, n, num_objects) == nullptr)) {
    // If this allocation had resulted in a donation to the filler, we record
    // these pages as abandoned.
    if (ABSL_PREDICT_FALSE(might_abandon)) {
      ASSERT(pt->was_donated());
      abandoned_pages_ += pt->abandoned_count();
      pt->set_abandoned(true);
    }
    return;
  }
  if (pt->was_donated()) {
    --donated_huge_pages_;
    if (pt->abandoned()) {
      abandoned_pages_ -= pt->abandoned_count();
      pt->set_abandoned(false);
    }
  } else {
    ASSERT(pt->abandoned_count() == Length(0));
  }
  lifetime_allocator_.MaybePutTracker(pt->lifetime_tracker(), n);
  ReleaseHugepage(pt);
}

template <class Forwarder>
inline bool HugePageAwareAllocator<Forwarder>::AddRegion() {
  HugeRange r = alloc_.Get(HugeRegion::size());
  if (!r.valid()) return false;
  HugeRegion* region = region_allocator_.New();
  new (region) HugeRegion(r, MemoryModifyFunction(SystemRelease));
  regions_.Contribute(region);
  return true;
}

template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::Delete(Span* span,
                                                      size_t objects_per_span) {
  ASSERT(!span || GetMemoryTag(span->start_address()) == tag_);
  PageId p = span->first_page();
  HugePage hp = HugePageContaining(p);
  Length n = span->num_pages();
  info_.RecordFree(p, n, objects_per_span);

  bool might_abandon = span->donated();
  forwarder_.DeleteSpan(span);
  // Clear the descriptor of the page so a second pass through the same page
  // could trigger the check on `span != nullptr` in do_free_pages.
  forwarder_.Set(p, nullptr);

  // The tricky part, as with so many allocators: where did we come from?
  // There are several possibilities.
  FillerType::Tracker* pt = GetTracker(hp);
  // a) We got packed by the filler onto a single hugepage - return our
  //    allocation to that hugepage in the filler.
  if (ABSL_PREDICT_TRUE(pt != nullptr)) {
    ASSERT(hp == HugePageContaining(p + n - Length(1)));
    DeleteFromHugepage(pt, p, n, objects_per_span, might_abandon);
    return;
  }

  // b) We got put into a region, possibly crossing hugepages -
  //    return our allocation to the region.
  if (regions_.MaybePut(p, n)) return;
  if (lifetime_allocator_.MaybePut(p, n)) return;

  // c) we came straight from the HugeCache - return straight there.  (We
  //    might have had slack put into the filler - if so, return that virtual
  //    allocation to the filler too!)
  ASSERT(n >= kPagesPerHugePage);
  HugeLength hl = HLFromPages(n);
  HugePage last = hp + hl - NHugePages(1);
  Length slack = hl.in_pages() - n;
  if (slack == Length(0)) {
    ASSERT(GetTracker(last) == nullptr);
  } else {
    pt = GetTracker(last);
    lifetime_allocator_.MaybePutTracker(pt->lifetime_tracker(), n);
    CHECK_CONDITION(pt != nullptr);
    ASSERT(pt->was_donated());
    // We put the slack into the filler (see AllocEnormous.)
    // Handle this page separately as a virtual allocation
    // onto the last hugepage.
    PageId virt = last.first_page();
    Length virt_len = kPagesPerHugePage - slack;
    // We may have used the slack, which would prevent us from returning
    // the entire range now.  If filler returned a Tracker, we are fully empty.
    if (filler_.Put(pt, virt, virt_len, objects_per_span) == nullptr) {
      // Last page isn't empty -- pretend the range was shorter.
      --hl;

      // Note that we abandoned virt_len pages with pt.  These can be reused for
      // other allocations, but this can contribute to excessive slack in the
      // filler.
      abandoned_pages_ += pt->abandoned_count();
      pt->set_abandoned(true);
    } else {
      // Last page was empty - but if we sub-released it, we still
      // have to split it off and release it independently.)
      //
      // We were able to reclaim the donated slack.
      --donated_huge_pages_;
      ASSERT(!pt->abandoned());

      if (pt->released()) {
        --hl;
        ReleaseHugepage(pt);
      } else {
        // Get rid of the tracker *object*, but not the *hugepage* (which is
        // still part of our range.)
        SetTracker(pt->location(), nullptr);
        ASSERT(!pt->lifetime_tracker()->is_tracked());
        tracker_allocator_.Delete(pt);
      }
    }
  }
  cache_.Release({hp, hl});
}

template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::ReleaseHugepage(
    FillerType::Tracker* pt) {
  ASSERT(pt->used_pages() == Length(0));
  HugeRange r = {pt->location(), NHugePages(1)};
  SetTracker(pt->location(), nullptr);

  if (pt->released()) {
    cache_.ReleaseUnbacked(r);
  } else {
    cache_.Release(r);
  }

  ASSERT(!pt->lifetime_tracker()->is_tracked());
  tracker_allocator_.Delete(pt);
}

// public
template <class Forwarder>
inline BackingStats HugePageAwareAllocator<Forwarder>::stats() const {
  BackingStats stats = alloc_.stats();
  const auto actual_system = stats.system_bytes;
  stats += cache_.stats();
  stats += filler_.stats();
  stats += regions_.stats();
  stats += lifetime_allocator_.GetRegionStats().value_or(BackingStats());
  // the "system" (total managed) byte count is wildly double counted,
  // since it all comes from HugeAllocator but is then managed by
  // cache/regions/filler. Adjust for that.
  stats.system_bytes = actual_system;
  return stats;
}

// public
template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::GetSmallSpanStats(
    SmallSpanStats* result) {
  GetSpanStats(result, nullptr, nullptr);
}

// public
template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::GetLargeSpanStats(
    LargeSpanStats* result) {
  GetSpanStats(nullptr, result, nullptr);
}

template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::GetSpanStats(
    SmallSpanStats* small, LargeSpanStats* large, PageAgeHistograms* ages) {
  if (small != nullptr) {
    *small = SmallSpanStats();
  }
  if (large != nullptr) {
    *large = LargeSpanStats();
  }

  alloc_.AddSpanStats(small, large, ages);
  filler_.AddSpanStats(small, large, ages);
  regions_.AddSpanStats(small, large, ages);
  cache_.AddSpanStats(small, large, ages);
}

// public
template <class Forwarder>
inline Length HugePageAwareAllocator<Forwarder>::ReleaseAtLeastNPages(
    Length num_pages) {
  Length released;
  released += cache_.ReleaseCachedPages(HLFromPages(num_pages)).in_pages();

  // This is our long term plan but in current state will lead to insufficient
  // THP coverage. It is however very useful to have the ability to turn this on
  // for testing.
  // TODO(b/134690769): make this work, remove the flag guard.
  if (forwarder_.hpaa_subrelease()) {
    if (released < num_pages) {
      released += filler_.ReleasePages(
          num_pages - released,
          SkipSubreleaseIntervals{
              .peak_interval = forwarder_.filler_skip_subrelease_interval(),
              .short_interval =
                  forwarder_.filler_skip_subrelease_short_interval(),
              .long_interval =
                  forwarder_.filler_skip_subrelease_long_interval()},
          forwarder_.release_partial_alloc_pages(),
          /*hit_limit*/ false);
    }
  }

  // Release all backed-but-free hugepages from HugeRegion.
  // TODO(b/199203282): We release all the free hugepages from HugeRegions when
  // the experiment is enabled. We can also explore releasing only a desired
  // number of pages.
  if (regions_.UseHugeRegionMoreOften()) {
    released += regions_.ReleasePages();
  }

  info_.RecordRelease(num_pages, released);
  return released;
}

inline static double BytesToMiB(size_t bytes) {
  const double MiB = 1048576.0;
  return bytes / MiB;
}

inline static void BreakdownStats(Printer* out, const BackingStats& s,
                                  const char* label) {
  out->printf("%s %6.1f MiB used, %6.1f MiB free, %6.1f MiB unmapped\n", label,
              BytesToMiB(s.system_bytes - s.free_bytes - s.unmapped_bytes),
              BytesToMiB(s.free_bytes), BytesToMiB(s.unmapped_bytes));
}

inline static void BreakdownStatsInPbtxt(PbtxtRegion* hpaa,
                                         const BackingStats& s,
                                         const char* key) {
  auto usage = hpaa->CreateSubRegion(key);
  usage.PrintI64("used", s.system_bytes - s.free_bytes - s.unmapped_bytes);
  usage.PrintI64("free", s.free_bytes);
  usage.PrintI64("unmapped", s.unmapped_bytes);
}

// public
template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::Print(Printer* out) {
  Print(out, true);
}

template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::Print(Printer* out,
                                                     bool everything) {
  SmallSpanStats small;
  LargeSpanStats large;
  BackingStats bstats;
  PageAgeHistograms ages(absl::base_internal::CycleClock::Now());
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  bstats = stats();
  GetSpanStats(&small, &large, &ages);
  PrintStats("HugePageAware", out, bstats, small, large, everything);
  out->printf(
      "\nHuge page aware allocator components:\n"
      "------------------------------------------------\n");
  out->printf("HugePageAware: breakdown of used / free / unmapped space:\n");

  auto fstats = filler_.stats();
  BreakdownStats(out, fstats, "HugePageAware: filler  ");

  auto rstats = regions_.stats();
  BreakdownStats(out, rstats, "HugePageAware: region  ");

  // Report short-lived region allocations when enabled.
  auto lstats = lifetime_allocator_.GetRegionStats();
  if (lstats.has_value()) {
    BreakdownStats(out, lstats.value(), "HugePageAware: lifetime");
  }

  auto cstats = cache_.stats();
  // Everything in the filler came from the cache -
  // adjust the totals so we see the amount used by the mutator.
  cstats.system_bytes -= fstats.system_bytes;
  BreakdownStats(out, cstats, "HugePageAware: cache   ");

  auto astats = alloc_.stats();
  // Everything in *all* components came from here -
  // so again adjust the totals.
  astats.system_bytes -=
      (fstats + rstats + lstats.value_or(BackingStats()) + cstats).system_bytes;
  BreakdownStats(out, astats, "HugePageAware: alloc   ");
  out->printf("\n");

  out->printf(
      "HugePageAware: filler donations %zu (%zu pages from abandoned "
      "donations)\n",
      donated_huge_pages_.raw_num(), abandoned_pages_.raw_num());

  // Component debug output
  // Filler is by far the most important; print (some) of it
  // unconditionally.
  filler_.Print(out, everything);
  out->printf("\n");
  if (everything) {
    regions_.Print(out);
    out->printf("\n");
    cache_.Print(out);
    lifetime_allocator_.Print(out);
    out->printf("\n");
    alloc_.Print(out);
    out->printf("\n");

    // Use statistics
    info_.Print(out);

    // and age tracking.
    ages.Print("HugePageAware", out);
  }

  out->printf("PARAMETER use_huge_region_more_often %d\n",
              regions_.UseHugeRegionMoreOften() ? 1 : 0);
  out->printf("PARAMETER hpaa_subrelease %d\n",
              forwarder_.hpaa_subrelease() ? 1 : 0);
}

template <class Forwarder>
inline void HugePageAwareAllocator<Forwarder>::PrintInPbtxt(
    PbtxtRegion* region) {
  SmallSpanStats small;
  LargeSpanStats large;
  PageAgeHistograms ages(absl::base_internal::CycleClock::Now());
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  GetSpanStats(&small, &large, &ages);
  PrintStatsInPbtxt(region, small, large, ages);
  {
    auto hpaa = region->CreateSubRegion("huge_page_allocator");
    hpaa.PrintBool("using_hpaa", true);
    hpaa.PrintBool("using_hpaa_subrelease", forwarder_.hpaa_subrelease());
    hpaa.PrintBool("use_huge_region_more_often",
                   regions_.UseHugeRegionMoreOften());

    // Fill HPAA Usage
    auto fstats = filler_.stats();
    BreakdownStatsInPbtxt(&hpaa, fstats, "filler_usage");

    auto rstats = regions_.stats();
    BreakdownStatsInPbtxt(&hpaa, rstats, "region_usage");

    auto cstats = cache_.stats();
    // Everything in the filler came from the cache -
    // adjust the totals so we see the amount used by the mutator.
    cstats.system_bytes -= fstats.system_bytes;
    BreakdownStatsInPbtxt(&hpaa, cstats, "cache_usage");

    auto astats = alloc_.stats();
    // Everything in *all* components came from here -
    // so again adjust the totals.
    astats.system_bytes -= (fstats + rstats + cstats).system_bytes;

    auto lstats = lifetime_allocator_.GetRegionStats();
    if (lstats.has_value()) {
      astats.system_bytes -= lstats.value().system_bytes;
      BreakdownStatsInPbtxt(&hpaa, lstats.value(), "lifetime_region_usage");
    }

    BreakdownStatsInPbtxt(&hpaa, astats, "alloc_usage");

    filler_.PrintInPbtxt(&hpaa);
    regions_.PrintInPbtxt(&hpaa);
    cache_.PrintInPbtxt(&hpaa);
    alloc_.PrintInPbtxt(&hpaa);
    lifetime_allocator_.PrintInPbtxt(&hpaa);

    // Use statistics
    info_.PrintInPbtxt(&hpaa, "hpaa_stat");

    hpaa.PrintI64("filler_donated_huge_pages", donated_huge_pages_.raw_num());
    hpaa.PrintI64("filler_abandoned_pages", abandoned_pages_.raw_num());
  }
}

template <class Forwarder>
inline AddressRange HugePageAwareAllocator<Forwarder>::AllocAndReport(
    size_t bytes, size_t align) {
  auto ret = forwarder_.AllocatePages(bytes, align, tag_);
  if (ret.ptr == nullptr) return ret;
  const PageId page = PageIdContaining(ret.ptr);
  const Length page_len = BytesToLengthFloor(ret.bytes);
  forwarder_.Ensure(page, page_len);
  return ret;
}

template <class Forwarder>
inline Length
HugePageAwareAllocator<Forwarder>::ReleaseAtLeastNPagesBreakingHugepages(
    Length n) {
  // We desperately need to release memory, and are willing to
  // compromise on hugepage usage. That means releasing from the filler.
  return filler_.ReleasePages(n, SkipSubreleaseIntervals{},
                              /*release_partial_alloc_pages=*/false,
                              /*hit_limit=*/true);
}

template <class Forwarder>
inline bool HugePageAwareAllocator<Forwarder>::UnbackWithoutLock(
    void* start, size_t length) {
  pageheap_lock.Unlock();
  const bool ret = SystemRelease(start, length);
  pageheap_lock.Lock();
  return ret;
}

}  // namespace huge_page_allocator_internal

using HugePageAwareAllocator =
    huge_page_allocator_internal::HugePageAwareAllocator<
        huge_page_allocator_internal::StaticForwarder>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_PAGE_AWARE_ALLOCATOR_H_
