/*
 * Copyright (C) 2006, 2008 Apple Computer, Inc.  All rights reserved.
 * Copyright (C) 2007-2008 Torch Mobile, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
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
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_FONT_PLATFORM_DATA_CACHE_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_FONT_PLATFORM_DATA_CACHE_H_

#include "third_party/blink/renderer/platform/fonts/font_cache_key.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_deque.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_map.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/wtf/deque.h"

namespace blink {

enum class AlternateFontName;
class FontCache;
class FontDescription;
class FontFaceCreationParams;
class FontPlatformData;

// `FontPlatformDataCache` is the shared cache mapping from `FontDescription`
// to `FontPlatformData`.
class FontPlatformDataCache final {
  DISALLOW_NEW();

 public:
  FontPlatformDataCache();

  void Trace(Visitor* visitor) const {
    visitor->Trace(map_);
    visitor->Trace(record_replay_held_);
  }

  const FontPlatformData* GetOrCreateFontPlatformData(
      FontCache* font_cache,
      const FontDescription& font_description,
      const FontFaceCreationParams& creation_params,
      AlternateFontName alternate_font_name);

  void Clear() {
    map_.clear();
    record_replay_held_.clear();
    record_replay_keys_.clear();
  }

 private:
  friend class FontCacheMacTest;
  HeapHashMap<FontCacheKey, WeakMember<const FontPlatformData>> map_;

  // `map_` is weak, and the collector clears an entry nothing else holds at a
  // point which isn't the same on a replay. Making the entry again asks the
  // system for the font, which is recorded. So while recording or replaying
  // the newest entries are held here, with their keys, and an entry leaves the
  // map when it leaves this list, whoever else still has it: the map then has
  // the same entries on a replay. The list is bounded, as these are collected
  // objects and a page can ask for thousands.
  void RecordReplayHold(const FontCacheKey& key,
                        const FontPlatformData* platform_data);
  HeapDeque<Member<const FontPlatformData>> record_replay_held_;
  Deque<FontCacheKey> record_replay_keys_;

  // A maximum float value to which we limit incoming font sizes. This is the
  // smallest float so that multiplying it by
  // FontCacheKey::PrecisionMultiplier() is still smaller than
  // std::numeric_limits<wtf_size_t>::max() - 1 in order to avoid hitting
  // HashMap sentinel values (placed at std::numeric_limits<wtf_size_t>::max()
  // and std::numeric_limits<wtf_size_t>::max() - 1) for FontPlatformDataCache.
  const float font_size_limit_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_FONT_PLATFORM_DATA_CACHE_H_
