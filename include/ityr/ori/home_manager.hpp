#pragma once

#include <algorithm>

#include "ityr/common/util.hpp"
#include "ityr/common/span.hpp"
#include "ityr/common/topology.hpp"
#include "ityr/common/logger.hpp"
#include "ityr/common/virtual_mem.hpp"
#include "ityr/common/physical_mem.hpp"
#include "ityr/ori/util.hpp"
#include "ityr/ori/block_regions.hpp"
#include "ityr/ori/cache_system.hpp"
#include "ityr/ori/tlb.hpp"

namespace ityr::ori {

template <block_size_t BlockSize>
class home_manager {
public:
  home_manager(std::size_t mmap_entry_limit)
    : mmap_entry_limit_(mmap_entry_limit),
      cs_(mmap_entry_limit_, mmap_entry(this)) {}

  template <bool IncrementRef>
  bool checkout_fast(std::byte* addr, std::size_t size) {
    ITYR_CHECK(addr);
    ITYR_CHECK(size > 0);

    auto meo = home_tlb_.get([&](const common::span<std::byte>& seg) {
      return seg.data() <= addr && addr + size <= seg.data() + seg.size();
    });
    if (!meo.has_value()) {
      return false;
    }
    mmap_entry& me = **meo;

    if constexpr (IncrementRef) {
      me.ref_count++;
    }

    return true;
  }

  template <bool IncrementRef>
  void checkout_seg(std::byte*                  seg_addr,
                    std::size_t                 seg_size,
                    const common::physical_mem& pm,
                    std::size_t                 pm_offset) {
    ITYR_CHECK(seg_addr);
    ITYR_CHECK(seg_size > 0);

    mmap_entry& me = get_entry(seg_addr);

    if (seg_addr != me.mapped_addr) {
      me.addr      = seg_addr;
      me.size      = seg_size;
      me.pm        = &pm;
      me.pm_offset = pm_offset;
      home_segments_to_map_.push_back(&me);
    }

    if constexpr (IncrementRef) {
      me.ref_count++;
    }

    home_tlb_.add({seg_addr, seg_size}, &me);
  }

  template <bool DecrementRef>
  bool checkin_fast(const std::byte* addr, std::size_t size) {
    ITYR_CHECK(addr);
    ITYR_CHECK(size > 0);

    if constexpr (!DecrementRef) {
      return false;
    }

    auto meo = home_tlb_.get([&](const common::span<std::byte>& seg) {
      return seg.data() <= addr && addr + size <= seg.data() + seg.size();
    });
    if (!meo.has_value()) {
      return false;
    }
    mmap_entry& me = **meo;

    me.ref_count--;

    return true;
  }

  template <bool DecrementRef>
  void checkin_seg(std::byte* seg_addr) {
    if constexpr (DecrementRef) {
      mmap_entry& me = get_entry<false>(seg_addr);
      me.ref_count--;
    }
  }

  void checkout_complete() {
    if (!home_segments_to_map_.empty()) {
      for (mmap_entry* me : home_segments_to_map_) {
        update_mapping(*me);
      }
      home_segments_to_map_.clear();
    }
  }

  void ensure_evicted(void* addr) {
    cs_.ensure_evicted(cache_key(addr));
  }

private:
  using cache_key_t = uintptr_t;

  struct mmap_entry {
    cache_entry_idx_t           entry_idx   = std::numeric_limits<cache_entry_idx_t>::max();
    std::byte*                  addr        = nullptr;
    std::byte*                  mapped_addr = nullptr;
    std::size_t                 size        = 0;
    std::size_t                 mapped_size = 0;
    const common::physical_mem* pm          = nullptr;
    std::size_t                 pm_offset   = 0;
    int                         ref_count   = 0;
    home_manager*               outer;

    mmap_entry(home_manager* outer_p) : outer(outer_p) {}

    /* Callback functions for cache_system class */

    bool is_evictable() const {
      return ref_count == 0;
    }

    void on_evict() {
      ITYR_CHECK(is_evictable());
      ITYR_CHECK(mapped_addr == addr);
      entry_idx = std::numeric_limits<cache_entry_idx_t>::max();
      // for safety
      outer->home_tlb_.clear();
    }

    void on_cache_map(cache_entry_idx_t idx) {
      entry_idx = idx;
    }
  };

  template <bool UpdateLRU = true>
  mmap_entry& get_entry(void* addr) {
    try {
      return cs_.template ensure_cached<UpdateLRU>(cache_key(addr));
    } catch (cache_full_exception& e) {
      common::die("home segments are exhausted (too much checked-out memory)");
    }
  }

  void update_mapping(mmap_entry& me) {
    if (me.mapped_addr) {
      common::verbose("Unmap home segment [%p, %p) (size=%ld)",
                      me.mapped_addr, me.mapped_addr + me.mapped_size, me.mapped_size);
      common::mmap_no_physical_mem(me.mapped_addr, me.mapped_size, true);
    }
    ITYR_CHECK(me.pm);
    ITYR_CHECK(me.addr);
    common::verbose("Map home segment [%p, %p) (size=%ld)",
                    me.addr, me.addr + me.size, me.size);
    me.pm->map_to_vm(me.addr, me.size, me.pm_offset);
    me.mapped_addr = me.addr;
    me.mapped_size = me.size;
  }

  cache_key_t cache_key(void* addr) const {
    ITYR_CHECK(addr);
    ITYR_CHECK(reinterpret_cast<uintptr_t>(addr) % BlockSize == 0);
    return reinterpret_cast<uintptr_t>(addr) / BlockSize;
  }

  std::size_t                               mmap_entry_limit_;
  cache_system<cache_key_t, mmap_entry>     cs_;
  tlb<common::span<std::byte>, mmap_entry*> home_tlb_;
  std::vector<mmap_entry*>                  home_segments_to_map_;
};

}
