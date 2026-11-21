// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/client/record_replay_shared_image_interface.h"

#include <utility>

#include "base/memory/shared_memory_mapping.h"
#include "base/notreached.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "ui/gfx/gpu_memory_buffer_handle.h"

namespace gpu {

RecordReplaySharedImageInterface::RecordReplaySharedImageInterface() = default;
RecordReplaySharedImageInterface::~RecordReplaySharedImageInterface() = default;

scoped_refptr<ClientSharedImage>
RecordReplaySharedImageInterface::CreateSharedImageForSoftwareCompositor(
    const SharedImageInfo& si_info) {
  base::WritableSharedMemoryMapping mapping;
  gfx::GpuMemoryBufferHandle handle;
  CreateSharedMemoryRegionFromSIInfo(si_info, mapping, handle);
  return base::MakeRefCounted<ClientSharedImage>(
      Mailbox::Generate(), si_info, SyncToken(), holder_, std::move(mapping));
}

// The images live only in this process, so there is nothing to tell a GPU
// service about them.
void RecordReplaySharedImageInterface::DestroySharedImage(
    std::vector<SyncToken> sync_tokens,
    const Mailbox& mailbox) {}

void RecordReplaySharedImageInterface::DestroySharedImage(
    const SyncToken& sync_token,
    scoped_refptr<ClientSharedImage> client_shared_image) {}

bool RecordReplaySharedImageInterface::IsLost() const {
  return false;
}

bool RecordReplaySharedImageInterface::AddGpuChannelLostObserver(
    GpuChannelLostObserver* observer) {
  return true;
}

void RecordReplaySharedImageInterface::RemoveGpuChannelLostObserver(
    GpuChannelLostObserver* observer) {}

// Without a GPU service, no work is ordered by sync tokens.
SyncToken RecordReplaySharedImageInterface::GenVerifiedSyncToken() {
  return SyncToken();
}

SyncToken RecordReplaySharedImageInterface::GenUnverifiedSyncToken() {
  return SyncToken();
}

void RecordReplaySharedImageInterface::VerifySyncToken(SyncToken& sync_token) {}

bool RecordReplaySharedImageInterface::CanVerifySyncToken(
    const gpu::SyncToken& sync_token) {
  return true;
}

void RecordReplaySharedImageInterface::VerifyFlush() {}

void RecordReplaySharedImageInterface::WaitSyncToken(
    const SyncToken& sync_token) {}

const SharedImageCapabilities&
RecordReplaySharedImageInterface::GetCapabilities() {
  return capabilities_;
}

scoped_refptr<ClientSharedImage>
RecordReplaySharedImageInterface::CreateSharedImage(
    const SharedImageInfo& si_info,
    SurfaceHandle surface_handle,
    std::optional<SharedImagePoolId> pool_id) {
  NOTREACHED();
}

scoped_refptr<ClientSharedImage>
RecordReplaySharedImageInterface::CreateSharedImage(
    const SharedImageInfo& si_info,
    base::span<const uint8_t> pixel_data) {
  NOTREACHED();
}

scoped_refptr<ClientSharedImage>
RecordReplaySharedImageInterface::CreateSharedImage(
    const SharedImageInfo& si_info,
    SurfaceHandle surface_handle,
    gfx::BufferUsage buffer_usage,
    std::optional<SharedImagePoolId> pool_id) {
  NOTREACHED();
}

scoped_refptr<ClientSharedImage>
RecordReplaySharedImageInterface::CreateSharedImage(
    const SharedImageInfo& si_info,
    SurfaceHandle surface_handle,
    gfx::BufferUsage buffer_usage,
    gfx::GpuMemoryBufferHandle buffer_handle) {
  NOTREACHED();
}

scoped_refptr<ClientSharedImage>
RecordReplaySharedImageInterface::CreateSharedImage(
    const SharedImageInfo& si_info,
    gfx::GpuMemoryBufferHandle buffer_handle) {
  NOTREACHED();
}

scoped_refptr<ClientSharedImage>
RecordReplaySharedImageInterface::CreateSharedImageForMLTensor(
    std::string debug_label,
    viz::SharedImageFormat format,
    const gfx::Size& size,
    gpu::SharedImageUsageSet usage) {
  NOTREACHED();
}

void RecordReplaySharedImageInterface::UpdateSharedImage(
    const SyncToken& sync_token,
    const Mailbox& mailbox) {
  NOTREACHED();
}

void RecordReplaySharedImageInterface::UpdateSharedImage(
    const SyncToken& sync_token,
    std::unique_ptr<gfx::GpuFence> acquire_fence,
    const Mailbox& mailbox) {
  NOTREACHED();
}

scoped_refptr<ClientSharedImage>
RecordReplaySharedImageInterface::ImportSharedImage(
    ExportedSharedImage exported_shared_image) {
  NOTREACHED();
}

}  // namespace gpu
