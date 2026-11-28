// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_FONT_FALLBACK_MAP_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_FONT_FALLBACK_MAP_H_

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/fonts/font_cache_client.h"
#include "third_party/blink/renderer/platform/fonts/font_description.h"
#include "third_party/blink/renderer/platform/fonts/font_fallback_list.h"
#include "third_party/blink/renderer/platform/fonts/font_selector_client.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_deque.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_map.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/deque.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

class FontSelector;

// This class acts as a cache ensuring that equivalent `FontDescription`s will
// have the same `FontFallbackList`.
//
// This class doesn't retain the `FontFallbackList`s however, only having a weak
// reference to them.
class PLATFORM_EXPORT FontFallbackMap : public FontCacheClient,
                                        public FontSelectorClient {
 public:
  explicit FontFallbackMap(FontSelector* font_selector)
      : font_selector_(font_selector),
        record_replay_id_(recordreplay::NewIdAnyThread("FontFallbackMap")) {}

  FontSelector* GetFontSelector() const { return font_selector_.Get(); }

  FontFallbackList* Get(const FontDescription& font_description);

  void Trace(Visitor* visitor) const override;

 private:
  // FontSelectorClient
  void FontsNeedUpdate(FontSelector*, FontInvalidationReason) override;

  // FontCacheClient
  void FontCacheInvalidated() override;

  void InvalidateAll();

  template <typename Predicate>
  void InvalidateInternal(Predicate predicate);

  const Member<FontSelector> font_selector_;
  HeapHashMap<FontDescription, WeakMember<FontFallbackList>>
      fallback_list_for_description_;

  // `fallback_list_for_description_` is weak, and the collector clears an
  // entry nothing else holds at a point which isn't the same on a replay.
  // Whether `Get()` finds a list then differs, and a new list looks its fonts
  // up again. So while recording or replaying the newest lists are held here,
  // with their descriptions, and a list leaves the map when it leaves this
  // list, whoever else still has it. The list is bounded, as a page can use
  // thousands of font descriptions.
  void RecordReplayHold(const FontDescription& font_description,
                        FontFallbackList* fallback_list);
  HeapDeque<Member<FontFallbackList>> record_replay_held_;
  Deque<FontDescription> record_replay_descriptions_;

  int record_replay_id_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_FONT_FALLBACK_MAP_H_
