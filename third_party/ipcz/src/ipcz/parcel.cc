// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/parcel.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

#include "ipcz/node_link.h"
#include "ipcz/node_link_memory.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/record_replay.h"

namespace ipcz {

Parcel::Parcel() = default;

Parcel::Parcel(SequenceNumber sequence_number)
    : sequence_number_(sequence_number) {}

Parcel::~Parcel() {
  for (Ref<APIObject>& object : objects_.storage) {
    if (object) {
      object->Close();
    }
  }
}

void Parcel::SetDataFromMessage(Message::ReceivedDataBuffer buffer,
                                absl::Span<uint8_t> data_view) {
  ABSL_ASSERT(data_view.empty() || data_view.begin() >= buffer.bytes().begin());
  ABSL_ASSERT(data_view.empty() || data_view.end() <= buffer.bytes().end());
  data_.storage = std::move(buffer);
  data_.view = data_view;
}

void Parcel::AllocateData(size_t num_bytes,
                          bool allow_partial,
                          NodeLinkMemory* memory) {
  ABSL_ASSERT(std::holds_alternative<std::monostate>(data_.storage));

  // [RecordReplay] A parcel's data stays out of shared memory when a renderer
  // is recorded, on both sides of the link: in the recorded or replaying
  // process itself, and in a browser of a record/replay run, which is what
  // sends a page its responses. Inline, a message reaches the renderer through
  // its channel, in a call whose output is in the recording, as every message
  // did with the legacy Mojo core; and a recorded node allocates no blocks for
  // parcel data, so nothing has to be recorded for the allocation.
  if (::recordreplay::RecordsAllContent() ||
      ::recordreplay::IsRecordingOrReplaying()) {
    memory = nullptr;
  }

  Fragment fragment;
  if (num_bytes > 0 && memory) {
    const size_t requested_fragment_size = num_bytes + sizeof(FragmentHeader);
    if (allow_partial) {
      fragment = memory->AllocateFragmentBestEffort(requested_fragment_size);
    } else {
      fragment = memory->AllocateFragment(requested_fragment_size);
    }
  }

  if (fragment.is_null()) {
    std::vector<uint8_t> bytes(num_bytes);
    data_.view = absl::MakeSpan(bytes);
    data_.storage = std::move(bytes);
    return;
  }

  // The smallest possible Fragment we could allocate above is still
  // subsantially larger than a FragmentHeader.
  ABSL_ASSERT(fragment.size() > sizeof(FragmentHeader));

  // Leave room for a FragmentHeader at the start of the fragment. This header
  // is not written until CommitData().
  const size_t data_size =
      std::min(num_bytes, fragment.size() - sizeof(FragmentHeader));
  data_.storage.emplace<DataFragment>(WrapRefCounted(memory), fragment);
  data_.view =
      fragment.mutable_bytes().subspan(sizeof(FragmentHeader), data_size);
}

bool Parcel::AdoptDataFragment(Ref<NodeLinkMemory> memory,
                               const Fragment& fragment) {
  if (!fragment.is_addressable() || fragment.size() <= sizeof(FragmentHeader) ||
      fragment.offset() % 8 != 0) {
    return false;
  }

  // This load-acquire is balanced by a store-release in CommitData() by the
  // producer of this data.
  const auto& header =
      *reinterpret_cast<const FragmentHeader*>(fragment.bytes().data());
  // The fragment was written by the other node, so its size is recorded.
  const uint32_t data_size = static_cast<uint32_t>(
      replay::Value("ipcz Parcel::AdoptDataFragment size",
                    header.size.load(std::memory_order_acquire)));
  const size_t max_data_size = fragment.size() - sizeof(FragmentHeader);
  if (data_size > max_data_size) {
    return false;
  }

  if (replay::IsRecordable()) {
    // The data was written by the other node as well. Copy it out of shared
    // memory into this parcel and record the copy, like the legacy core's
    // DataPipeConsumerDispatcher recorded what it read from its shared buffer.
    // When replaying the other node is not there, and the copy is filled from
    // the recording. Nothing reads the fragment after this, so it is freed.
    std::vector<uint8_t> bytes(data_size);
    const absl::Span<const uint8_t> fragment_data =
        fragment.bytes().subspan(sizeof(FragmentHeader), data_size);
    std::copy(fragment_data.begin(), fragment_data.end(), bytes.begin());
    replay::Bytes("ipcz Parcel::AdoptDataFragment", bytes.data(), data_size);
    data_.view = absl::MakeSpan(bytes);
    data_.storage = std::move(bytes);
    memory->FreeFragment(fragment);
    return true;
  }

  data_.storage.emplace<DataFragment>(std::move(memory), fragment);
  data_.view =
      fragment.mutable_bytes().subspan(sizeof(FragmentHeader), data_size);
  return true;
}

void Parcel::SetObjects(std::vector<Ref<APIObject>> objects) {
  objects_.storage = std::move(objects);
  objects_.view = absl::MakeSpan(objects_.storage);
}

void Parcel::CommitData(size_t num_bytes) {
  data_.view = data_.view.first(num_bytes);
  if (!has_data_fragment()) {
    return;
  }

  DataFragment& storage = std::get<DataFragment>(data_.storage);
  ABSL_ASSERT(storage.is_valid());
  ABSL_ASSERT(num_bytes <= storage.fragment().size() + sizeof(FragmentHeader));
  auto& header = *reinterpret_cast<FragmentHeader*>(
      storage.fragment().mutable_bytes().data());
  header.reserved.store(0, std::memory_order_relaxed);

  // This store-release is balanced by the load-acquire in AdoptDataFragment()
  // by the eventual consumer of this data.
  //
  // A static_cast is fine here: we do not allocate 4 GB+ fragments.
  header.size.store(static_cast<uint32_t>(num_bytes),
                    std::memory_order_release);
}

void Parcel::ReleaseDataFragment() {
  ABSL_ASSERT(has_data_fragment());
  std::ignore = std::get<DataFragment>(data_.storage).release();
  data_.storage.emplace<std::monostate>();
  data_.view = {};
}

void Parcel::ConsumeHandles(absl::Span<IpczHandle> out_handles) {
  absl::Span<Ref<APIObject>> objects = objects_.view;
  ABSL_ASSERT(out_handles.size() <= objects.size());

  for (size_t i = 0; i < out_handles.size(); ++i) {
    out_handles[i] = APIObject::ReleaseAsHandle(std::move(objects[i]));
  }

  objects_.view.remove_prefix(out_handles.size());
}

void Parcel::SetEnvelope(DriverObject envelope) {
  envelope_ = std::move(envelope);
}

std::string Parcel::Describe() const {
  std::stringstream ss;
  ss << "parcel " << sequence_number() << " (";
  if (!data_view().empty()) {
    // Cheesy heuristic: if the first character is an ASCII letter or number,
    // assume the parcel data is human-readable and print a few characters.
    if (std::isalnum(data_view()[0])) {
      const absl::Span<const uint8_t> preview = data_view().subspan(0, 8);
      ss << "\"" << std::string(preview.begin(), preview.end());
      if (preview.size() < data_size()) {
        ss << "...\", " << data_size() << " bytes";
      } else {
        ss << '"';
      }
    }
  } else {
    ss << "no data";
  }
  if (!objects_view().empty()) {
    ss << ", " << num_objects() << " handles";
  }
  ss << ")";
  return ss.str();
}

Parcel::DataFragment::~DataFragment() {
  reset();
}

Fragment Parcel::DataFragment::release() {
  memory_.reset();
  return std::exchange(fragment_, {});
}

void Parcel::DataFragment::reset() {
  if (!is_valid()) {
    return;
  }

  memory_->FreeFragment(fragment_);
  memory_.reset();
  fragment_ = {};
}

}  // namespace ipcz
