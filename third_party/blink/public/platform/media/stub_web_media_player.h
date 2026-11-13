// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_PLATFORM_MEDIA_STUB_WEB_MEDIA_PLAYER_H_
#define THIRD_PARTY_BLINK_PUBLIC_PLATFORM_MEDIA_STUB_WEB_MEDIA_PLAYER_H_

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "third_party/blink/public/platform/web_common.h"
#include "third_party/blink/public/platform/web_media_player.h"

namespace base {
class SingleThreadTaskRunner;
}  // namespace base

namespace blink {
class WebMediaPlayerClient;
class WebMediaPlayerDelegate;

// A player for recording/replaying, where media isn't played: it loads a
// fixed-length resource without tracks, fetching and decoding nothing, and
// plays it on the clock. Only URL sources load.
BLINK_PLATFORM_EXPORT std::unique_ptr<WebMediaPlayer> CreateStubWebMediaPlayer(
    WebMediaPlayerClient* client,
    WebMediaPlayerDelegate* delegate,
    scoped_refptr<base::SingleThreadTaskRunner> main_task_runner);

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_PLATFORM_MEDIA_STUB_WEB_MEDIA_PLAYER_H_
