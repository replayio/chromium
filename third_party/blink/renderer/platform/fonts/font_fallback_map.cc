// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/fonts/font_fallback_map.h"

#include "third_party/blink/renderer/platform/fonts/font_selector.h"

namespace blink {

void FontFallbackMap::Trace(Visitor* visitor) const {
  visitor->Trace(font_selector_);
  FontCacheClient::Trace(visitor);
  FontSelectorClient::Trace(visitor);
}

FontFallbackMap::~FontFallbackMap() {
  AutoLockForParallelTextShaping guard(lock_);
  InvalidateAll();
}

static std::atomic<int> g_record_replay_fallback_list_lock = 0;
static int GetOrCreateFallbackListLock() {
  if (!g_record_replay_fallback_list_lock) {
    g_record_replay_fallback_list_lock = (int)recordreplay::CreateOrderedLock("FontFallbackMap");
  }
  return g_record_replay_fallback_list_lock;
}

scoped_refptr<FontFallbackList> FontFallbackMap::Get(
    const FontDescription& font_description) {
  AutoLockForParallelTextShaping guard(lock_);
  auto iter = fallback_list_for_description_.find(font_description);
  if (!IsMainThread()) {
    recordreplay::Warning("[RUN-3109] FontFallbackMap::Get on non-main thread");
  }
  {
    // We see a divergence in the following Assert but with no obvious
    // root cause.
    // It might be possible that |fallback_list_for_description_| gets modified
    // by more than one thread but |lock_| is actually a no-op.
    // → Let's add auto-ordering here just to make sure that we see the
    // warning before we hit this Assert again. If we see the warning,
    // we can enforce that |lock_| is actually an ordered lock.
    // Else, look for other possible sources of non-determinism.
    recordreplay::AutoOrderedLock lck(GetOrCreateFallbackListLock());
  }
  recordreplay::Assert("[RUN-3109] FontFallbackMap::Get %d %d %d %u",
                       iter != fallback_list_for_description_.end(),
                       iter != fallback_list_for_description_.end() ? iter->value->RecordReplayId() : -1,
                       iter != fallback_list_for_description_.end() ? iter->value->HasOneRef() : -1,
                       font_description.GetHash());
  if (iter != fallback_list_for_description_.end()) {
    DCHECK(iter->value->IsValid());
    return iter->value;
  }
  auto add_result = fallback_list_for_description_.insert(
      font_description, FontFallbackList::Create(*this));
  return add_result.stored_value->value;
}

void FontFallbackMap::Remove(const FontDescription& font_description) {
    if (recordreplay::AreEventsDisallowed("FontFallbackMap::Remove")) {
    // Leak fallback_list_for_description_ contents.
    return;
  }
  AutoLockForParallelTextShaping guard(lock_);
  auto iter = fallback_list_for_description_.find(font_description);
  DCHECK_NE(iter, fallback_list_for_description_.end());
  DCHECK(iter->value->IsValid());
  DCHECK(iter->value->HasOneRef());
  fallback_list_for_description_.erase(iter);
}

void FontFallbackMap::InvalidateAll() {
    if (recordreplay::AreEventsDisallowed("FontFallbackMap::InvalidateAll")) {
    // Leak fallback_list_for_description_ contents.
    return;
  }
  lock_.AssertAcquired();
  for (auto& entry : fallback_list_for_description_)
    entry.value->MarkInvalid();
  fallback_list_for_description_.clear();
}

template <typename Predicate>
void FontFallbackMap::InvalidateInternal(Predicate predicate) {
  lock_.AssertAcquired();
  Vector<FontDescription> invalidated;
  for (auto& entry : fallback_list_for_description_) {
    if (predicate(*entry.value)) {
      invalidated.push_back(entry.key);
      entry.value->MarkInvalid();
    }
  }
  fallback_list_for_description_.RemoveAll(invalidated);
}

void FontFallbackMap::FontsNeedUpdate(FontSelector*,
                                      FontInvalidationReason reason) {
  if (recordreplay::AreEventsDisallowed("FontFallbackMap::FontsNeedUpdate")) {
    // Leak fallback_list_for_description_ contents to avoid divergence down the
    // road.
    return;
  }

  AutoLockForParallelTextShaping guard(lock_);
  switch (reason) {
    case FontInvalidationReason::kFontFaceLoaded:
      InvalidateInternal([](const FontFallbackList& fallback_list) {
        return fallback_list.HasLoadingFallback();
      });
      break;
    case FontInvalidationReason::kFontFaceDeleted:
      InvalidateInternal([](const FontFallbackList& fallback_list) {
        return fallback_list.HasCustomFont();
      });
      break;
    default:
      InvalidateAll();
  }
}

void FontFallbackMap::FontCacheInvalidated() {
  AutoLockForParallelTextShaping guard(lock_);
  InvalidateAll();
}

}  // namespace blink
