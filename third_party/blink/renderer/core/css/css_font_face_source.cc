/*
 * Copyright (C) 2007, 2008, 2010, 2011 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/core/css/css_font_face_source.h"
#include "base/no_destructor.h"

#include "base/record_replay.h"
#include "third_party/blink/renderer/core/css/css_font_face.h"
#include "third_party/blink/renderer/platform/fonts/font_cache_key.h"
#include "third_party/blink/renderer/platform/fonts/font_description.h"
#include "third_party/blink/renderer/platform/fonts/font_face_creation_params.h"
#include "third_party/blink/renderer/platform/fonts/simple_font_data.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_set.h"

namespace blink {

// [RecordReplay] No leak gate here any more. The fork's M108 baseline kept the
// cached font data alive from this destructor, because FontDataTable was then
// an off-heap HashMap holding the last strong reference to each SimpleFontData,
// so when the GC finalized a source decided when its typeface died. Upstream
// has since made SimpleFontData garbage collected and the table a
// HeapHashMap of WeakMember, which is not an owner, so collecting a source can
// no longer move that point and there is nothing left for a gate to pin. The
// port of the old gate was actively harmful: it ran inside
// SweepFinalizer::FinalizePage, where iterating the table compares FontCacheKeys
// whose strings may already be swept, and resurrecting a weakly held object into
// a Persistent hands the next concurrent marker an object the sweeper is still
// freeing. Both shapes were measured as recorder segfaults in
// gc/font-face-collected-before-style-recalc. DSRC-433.

namespace {

// [RecordReplay] Holds every SimpleFontData a web font source has handed out,
// see GetFontData. The reference has to be a GC root, because M123 moved font
// data to Oilpan and a raw pointer here would compile while letting the font be
// collected, which is the opposite of the point. Same idiom as
// font_cache.cc's ReplayLastResortSimpleFontDataStorage.
Persistent<GCedHeapHashSet<Member<const SimpleFontData>>>&
ReplayWebFontDataStorage() {
  static base::NoDestructor<
      Persistent<GCedHeapHashSet<Member<const SimpleFontData>>>>
      font_data(MakeGarbageCollected<
                GCedHeapHashSet<Member<const SimpleFontData>>>());
  return *font_data;
}

}  // namespace

const SimpleFontData* CSSFontFaceSource::GetFontData(
    const FontDescription& font_description,
    const FontSelectionCapabilities& font_selection_capabilities) {
  // If the font hasn't loaded or an error occurred, then we've got nothing.
  if (!IsValid()) {
    ReportFontLookup(font_description);
    return nullptr;
  }

  if (IsLocalNonBlocking()) {
    // We're local. Just return a SimpleFontData from the normal cache.
    return CreateFontData(font_description, font_selection_capabilities);
  }

  bool is_unique_match = false;
  FontCacheKey key =
      font_description.CacheKey(FontFaceCreationParams(), is_unique_match);

  auto result = font_data_table_.insert(key, nullptr);
  if (result.is_new_entry) {
    result.stored_value->value =
        CreateFontData(font_description, font_selection_capabilities);
  }

  // [RecordReplay] Keep what this source hands out. Up to the fork's M108
  // baseline font_data_table_ held a strong reference, so a source's font data
  // -- and through it the typeface it was built from -- lived as long as the
  // source and was destroyed by Skia's glyph cache rather than by the GC.
  // Upstream made that table weak when it moved font data to Oilpan, so a
  // later lookup now misses whenever the entry was collected, which depends on
  // when the GC ran. A miss does not merely rebuild the same font: measured on
  // the M158 leapfrog stack, the replay in
  // gc/font-face-collected-before-style-recalc leaves the web font entirely
  // and takes FontCache::GetFontData's fallback path, blocking on a font
  // service round trip the recording never made. The case gives the replay a
  // far smaller heap than the recording on purpose, so the two sides collect at
  // different points by design.
  //
  // The reference is held outside the source for the reason the M108 gate
  // existed: the data has to outlive the source. An earlier attempt pinned
  // FontPlatformDataCache's insert sites instead and changed nothing, because
  // this is the site M108 owned. DSRC-440.
  if (recordreplay::EnterLeakMemory("CSSFontFaceSource")) {
    if (const SimpleFontData* data = result.stored_value->value.Get()) {
      ReplayWebFontDataStorage()->insert(data);
    }
  }

  return result.stored_value->value.Get();
}

}  // namespace blink
