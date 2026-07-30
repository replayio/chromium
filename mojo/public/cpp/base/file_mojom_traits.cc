// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/public/cpp/base/file_mojom_traits.h"

#include "base/files/file.h"
#include "base/files/platform_file.h"
#include "base/record_replay.h"
#include "build/build_config.h"

namespace mojo {
namespace {

base::PlatformFile RecordReplayPlatformFile(base::PlatformFile platform_file) {
#if BUILDFLAG(IS_WIN)
  uintptr_t as_value = reinterpret_cast<uintptr_t>(platform_file);
#else
  uintptr_t as_value = static_cast<uintptr_t>(platform_file);
#endif
  uintptr_t recorded = recordreplay::RecordReplayValue(
      "Traits<FileDataView>::Read", as_value);
#if BUILDFLAG(IS_WIN)
  base::PlatformFile recorded_file =
      reinterpret_cast<base::PlatformFile>(recorded);
#else
  base::PlatformFile recorded_file =
      static_cast<base::PlatformFile>(recorded);
#endif
  // Opaque syscall identity only; discard live fd when substituting.
  if (recorded_file != platform_file &&
      platform_file != base::kInvalidPlatformFile) {
    base::ScopedPlatformFile discard(platform_file);
  }
  return recorded_file;
}

}  // namespace

mojo::PlatformHandle
StructTraits<mojo_base::mojom::FileDataView, base::File>::fd(base::File& file) {
  DCHECK(file.IsValid());

  return mojo::PlatformHandle(
      base::ScopedPlatformFile(file.TakePlatformFile()));
}

bool StructTraits<mojo_base::mojom::FileDataView, base::File>::Read(
    mojo_base::mojom::FileDataView data,
    base::File* file) {
  *file = base::File(RecordReplayPlatformFile(data.TakeFd().TakePlatformFile()),
                     data.async());
  return true;
}

}  // namespace mojo
