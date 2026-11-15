// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/buffer_pool.h"

#include <algorithm>

#include "ipcz/block_allocator_pool.h"
#include "ipcz/metrics.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/numeric/bits.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/ordered_mutex.h"
#include "util/record_replay.h"

namespace ipcz {

BufferPool::BufferPool() = default;

BufferPool::~BufferPool() = default;

Fragment BufferPool::GetFragment(const FragmentDescriptor& descriptor) {
  if (descriptor.is_null()) {
    return {};
  }

  OrderedMutexLock lock(&mutex_);
  auto it = mappings_.find(descriptor.buffer_id());
  if (it == mappings_.end()) {
    return Fragment::PendingFromDescriptor(descriptor);
  }

  auto& [id, mapping] = *it;
  return Fragment::MappedFromDescriptor(descriptor, mapping);
}

bool BufferPool::AddBlockBuffer(
    BufferId id,
    DriverMemoryMapping mapping,
    absl::Span<const BlockAllocator> block_allocators) {
  ABSL_ASSERT(mapping.is_valid());

  // Basic consistency checks before we change any pool state: ensure that each
  // given allocator actually lives within the memory mapped by `mapping`, and
  // that each has a unique power-of-2 block size.
  size_t block_sizes_present = 0;
  for (const auto& allocator : block_allocators) {
    if (&allocator.region().front() < &mapping.bytes().front() ||
        &allocator.region().back() > &mapping.bytes().back()) {
      // Not a valid allocator region for this mapping.
      return false;
    }

    const size_t block_size = allocator.block_size();
    ABSL_ASSERT(block_size >= 8 && block_size <= (1 << 30));
    if (!absl::has_single_bit(block_size)) {
      // Not a power of two.
      return false;
    }

    if (block_sizes_present & block_size) {
      // Duplicate allocator block size for this buffer.
      return false;
    }

    block_sizes_present |= block_size;
  }

  std::vector<WaitForBufferCallback> callbacks;
  {
    OrderedMutexLock lock(&mutex_);
    auto [it, inserted] = mappings_.insert({id, std::move(mapping)});
    if (!inserted) {
      return false;
    }

    auto callbacks_it = buffer_callbacks_.find(id);
    if (callbacks_it != buffer_callbacks_.end()) {
      callbacks = std::move(callbacks_it->second);
      buffer_callbacks_.erase(callbacks_it);
    }

    auto& inserted_mapping = it->second;
    for (const auto& allocator : block_allocators) {
      const size_t block_size = allocator.block_size();
      auto [pool_it, pool_inserted] =
          block_allocator_pools_.insert({block_size, nullptr});
      auto& pool = pool_it->second;
      if (pool_inserted) {
        pool = std::make_unique<BlockAllocatorPool>();
      }
      pool->Add(id, inserted_mapping.bytes(), allocator);
    }
  }

  for (auto& callback : callbacks) {
    callback();
  }

  return true;
}

size_t BufferPool::GetTotalBlockCapacity(size_t block_size) {
  BlockAllocatorPool* pool;
  {
    OrderedMutexLock lock(&mutex_);
    auto it = block_allocator_pools_.find(block_size);
    if (it == block_allocator_pools_.end()) {
      return 0;
    }

    pool = it->second.get();
  }

  return pool->GetCapacity();
}

Fragment BufferPool::RecordReplayAllocatedBlock(const Fragment& fragment) {
  // The allocation looked its buffer up without ordering this pool's mutex.
  // Acquire the mutex here, ordered, both when recording and when replaying:
  // a replaying thread then only gets past this point once the thread which
  // added the block's buffer when recording has added it when replaying.
  { OrderedMutexLock lock(&mutex_); }

  const uint64_t buffer_id = static_cast<uint64_t>(replay::Value(
      "ipcz BufferPool block buffer",
      fragment.is_null() ? kInvalidBufferId.value()
                         : fragment.buffer_id().value()));
  const uint32_t offset = static_cast<uint32_t>(replay::Value(
      "ipcz BufferPool block offset",
      fragment.is_null() ? 0 : fragment.offset()));
  const uint32_t size = static_cast<uint32_t>(replay::Value(
      "ipcz BufferPool block size", fragment.is_null() ? 0 : fragment.size()));
  if (!::recordreplay::IsReplaying()) {
    return fragment;
  }
  if (buffer_id == kInvalidBufferId.value()) {
    return {};
  }

  // The lookup below stands in for the one made by the allocation when
  // recording, which was not ordered either.
  ::recordreplay::AutoPassThroughEvents pass_through;
  const Fragment recorded_fragment =
      GetFragment(FragmentDescriptor(BufferId{buffer_id}, offset, size));
  if (!recorded_fragment.is_addressable()) {
    return {};
  }
  return recorded_fragment;
}

Fragment BufferPool::AllocateBlock(size_t block_size) {
  if (!replay::IsRecordable()) {
    return AllocateBlockInternal(block_size);
  }

  // Which block an allocation gets depends on what the other node allocated
  // and freed. Record it, and when replaying take the recorded block without
  // touching the allocators: the other node is not there and this node's copy
  // of their memory does not have the state they had when recording. The
  // allocation itself is passed through, as the locks it takes depend on the
  // state of the allocators.
  Fragment fragment;
  if (!::recordreplay::IsReplaying()) {
    ::recordreplay::AutoPassThroughEvents pass_through;
    fragment = AllocateBlockInternal(block_size);
  }
  return RecordReplayAllocatedBlock(fragment);
}

Fragment BufferPool::AllocateBlockBestEffort(size_t preferred_block_size) {
  if (!replay::IsRecordable()) {
    return AllocateBlockBestEffortInternal(preferred_block_size);
  }

  // See AllocateBlock().
  Fragment fragment;
  if (!::recordreplay::IsReplaying()) {
    ::recordreplay::AutoPassThroughEvents pass_through;
    fragment = AllocateBlockBestEffortInternal(preferred_block_size);
  }
  return RecordReplayAllocatedBlock(fragment);
}

bool BufferPool::FreeBlock(const Fragment& fragment) {
  if (!replay::IsRecordable()) {
    return FreeBlockInternal(fragment);
  }

  // Blocks are freed when the last reference to an object is dropped, which
  // can happen on another thread when replaying, and whether a fragment is
  // freed at all depends on a reference count the other node shares. So unlike
  // allocation, freeing adds nothing to the recording: it is passed through
  // and its locks are not ordered. A replaying node frees into its own copy of
  // the allocators, which recorded allocations never take from (see
  // AllocateBlock()).
  ::recordreplay::AutoPassThroughEvents pass_through;
  return FreeBlockInternal(fragment);
}

Fragment BufferPool::AllocateBlockInternal(size_t block_size) {
  ABSL_ASSERT(absl::has_single_bit(block_size));

  BlockAllocatorPool* pool;
  {
    OrderedMutexLock lock(&mutex_);
    auto it = block_allocator_pools_.lower_bound(block_size);
    if (it == block_allocator_pools_.end()) {
      metrics::RecordAllocateBlockResult(
          metrics::BlockAllocationSource::kBufferPool,
          /*block_size=*/block_size,
          /*success=*/false);
      return {};
    }

    // NOTE: BlockAllocatorPools live as long as this BufferPool once added, and
    // they are thread-safe objects; so retaining this pointer through the
    // extent of AllocateBlock() is safe.
    pool = it->second.get();
  }

  Fragment fragment = pool->Allocate();
  metrics::RecordAllocateBlockResult(
      metrics::BlockAllocationSource::kBufferPool,
      /*block_size=*/block_size,
      /*success=*/!fragment.is_null());
  return fragment;
}

Fragment BufferPool::AllocateBlockBestEffortInternal(
    size_t preferred_block_size) {
  ABSL_ASSERT(absl::has_single_bit(preferred_block_size));

  // Limit the number of attempts we make to scale down the requested size in
  // search of an available fragment. This value was chosen arbitrarily.
  constexpr size_t kMaxAttempts = 3;

  BlockAllocatorPoolMap::iterator pool_iter;
  BlockAllocatorPool* pool;
  {
    OrderedMutexLock lock(&mutex_);
    if (block_allocator_pools_.empty()) {
      return {};
    }

    pool_iter = block_allocator_pools_.lower_bound(preferred_block_size);
    if (pool_iter == block_allocator_pools_.end()) {
      --pool_iter;
    }

    pool = pool_iter->second.get();
  }

  for (size_t attempts = 0; attempts < kMaxAttempts; ++attempts) {
    const Fragment fragment = pool->Allocate();
    if (!fragment.is_null()) {
      return fragment;
    }

    OrderedMutexLock lock(&mutex_);
    if (pool_iter == block_allocator_pools_.begin()) {
      return {};
    }

    --pool_iter;
    pool = pool_iter->second.get();
  }

  return {};
}

bool BufferPool::FreeBlockInternal(const Fragment& fragment) {
  BlockAllocatorPool* pool;
  {
    OrderedMutexLock lock(&mutex_);
    auto it = block_allocator_pools_.find(fragment.size());
    if (it == block_allocator_pools_.end()) {
      return false;
    }

    pool = it->second.get();
  }

  return pool->Free(fragment);
}

void BufferPool::WaitForBufferAsync(BufferId id,
                                    WaitForBufferCallback callback) {
  {
    OrderedMutexLock lock(&mutex_);
    auto it = mappings_.find(id);
    if (it == mappings_.end()) {
      buffer_callbacks_[id].push_back(std::move(callback));
      return;
    }
  }

  callback();
}

void BufferPool::DropPendingBufferCallbacks() {
  // Dropping callbacks may have side-effects, so do it outside of a lock.
  BufferCallbackMap buffer_callbacks;
  {
    OrderedMutexLock lock(&mutex_);
    buffer_callbacks.swap(buffer_callbacks_);
  }
}

}  // namespace ipcz
