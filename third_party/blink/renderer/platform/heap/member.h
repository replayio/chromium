// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_HEAP_MEMBER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_HEAP_MEMBER_H_

#include "base/check_op.h"
#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/heap/thread_state_storage.h"
#include "third_party/blink/renderer/platform/heap/write_barrier.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/blink/renderer/platform/wtf/construct_traits.h"
#include "third_party/blink/renderer/platform/wtf/hash_functions.h"
#include "third_party/blink/renderer/platform/wtf/hash_traits.h"
#include "third_party/blink/renderer/platform/wtf/type_traits.h"
#include "v8/include/cppgc/member.h"

namespace blink {

template <typename T>
using Member = cppgc::Member<T>;

template <typename T>
using WeakMember = cppgc::WeakMember<T>;

template <typename T>
using UntracedMember = cppgc::UntracedMember<T>;

namespace subtle {
template <typename T>
using UncompressedMember = cppgc::subtle::UncompressedMember<T>;
}

template <typename T>
inline bool IsHashTableDeletedValue(const Member<T>& m) {
  return m == cppgc::kSentinelPointer;
}

constexpr auto kMemberDeletedValue = cppgc::kSentinelPointer;

template <typename T>
struct ThreadingTrait<blink::Member<T>> {
  STATIC_ONLY(ThreadingTrait);
  static constexpr ThreadAffinity kAffinity = ThreadingTrait<T>::kAffinity;
};

template <typename T>
struct ThreadingTrait<blink::WeakMember<T>> {
  STATIC_ONLY(ThreadingTrait);
  static constexpr ThreadAffinity kAffinity = ThreadingTrait<T>::kAffinity;
};

template <typename T>
struct ThreadingTrait<blink::UntracedMember<T>> {
  STATIC_ONLY(ThreadingTrait);
  static constexpr ThreadAffinity kAffinity = ThreadingTrait<T>::kAffinity;
};

template <typename T>
inline void swap(Member<T>& a, Member<T>& b) {
  a.Swap(b);
}

static constexpr bool kBlinkMemberGCHasDebugChecks =
    !std::is_same<cppgc::internal::DefaultMemberCheckingPolicy,
                  cppgc::internal::DisabledCheckingPolicy>::value;

}  // namespace blink

namespace WTF {

template <typename T>
struct IsTraceable<blink::Member<T>> {
  STATIC_ONLY(IsTraceable);
  static const bool value = true;
};

template <typename T>
struct IsWeak<blink::WeakMember<T>> : std::true_type {};

template <typename T>
struct IsTraceable<blink::WeakMember<T>> {
  STATIC_ONLY(IsTraceable);
  static const bool value = true;
};

// Detects whether an object carries a record/replay id of its own, which makes
// hashing it reproducible across a recording and its replays.
template <typename T>
class HasRecordReplayId {
  template <typename U>
  static auto Check(U* u) -> decltype(u->RecordReplayId(), std::true_type());
  template <typename>
  static std::false_type Check(...);

 public:
  static constexpr bool value = decltype(Check<T>(nullptr))::value;
};

template <typename T>
constexpr bool has_record_replay_id = HasRecordReplayId<T>::value;

// Default hash for hash tables with Member<>-derived elements.
template <typename T, typename MemberType>
struct BaseMemberHashTraits : SimpleClassHashTraits<MemberType> {
  STATIC_ONLY(BaseMemberHashTraits);

  // Heap hash containers allow to operate with raw pointers, e.g.
  //   HeapHashSet<Member<GCed>> set;
  //   set.find(raw_ptr);
  // Therefore, provide two hashing functions, one for raw pointers, another for
  // Member. Prefer compressing raw pointers instead of decompressing Members,
  // assuming the former is cheaper.
  static unsigned GetHash(const T* key) {
    if constexpr (has_record_replay_id<T>) {
      if (recordreplay::IsRecordingOrReplaying("pointer-ids")) {
        int id = key->RecordReplayId();
        // Ids are allowed to be zero if we've diverged from the recording.
        if (!recordreplay::HasDivergedFromRecording()) {
          CHECK(id > 0);  // Should have been registered.
        }
        if (id > 0) {
          return WTF::GetHash(id);
        }
      }
    }
#if defined(CPPGC_POINTER_COMPRESSION)
    cppgc::internal::CompressedPointer st(key);
#else
    cppgc::internal::RawPointer st(key);
#endif
    return WTF::GetHash(st.GetAsInteger());
  }
  template <typename Member,
            std::enable_if_t<WTF::IsAnyMemberType<Member>::value>* = nullptr>
  static unsigned GetHash(const Member& m) {
    if constexpr (has_record_replay_id<T>) {
      if (recordreplay::IsRecordingOrReplaying("pointer-ids")) {
        int id = m.Get()->RecordReplayId();
        // Ids are allowed to be zero if we've diverged from the recording.
        if (!recordreplay::HasDivergedFromRecording()) {
          CHECK(id > 0);  // Should have been registered.
        }
        if (id > 0) {
          return WTF::GetHash(id);
        }
      }
    }
    return WTF::GetHash(m.GetRawStorage().GetAsInteger());
  }

  // Iteration over a table keyed this way is reproducible exactly when the
  // elements carry a record/replay id.
  static constexpr bool kIsRecordReplayDeterministicHash =
      has_record_replay_id<T>;

  static constexpr bool kEmptyValueIsZero = true;

  using PeekInType = T*;
  using PeekOutType = T*;
  using IteratorGetType = MemberType*;
  using IteratorConstGetType = const MemberType*;
  using IteratorReferenceType = MemberType&;
  using IteratorConstReferenceType = const MemberType&;

  static PeekOutType Peek(const MemberType& value) { return value; }

  static void ConstructDeletedValue(MemberType& slot) {
    slot = cppgc::kSentinelPointer;
  }

  static bool IsDeletedValue(const MemberType& value) {
    return value == cppgc::kSentinelPointer;
  }
};

// Custom HashTraits<Member<Type>> can inherit this type.
template <typename T>
struct MemberHashTraits : BaseMemberHashTraits<T, blink::Member<T>> {
  static constexpr bool kCanTraceConcurrently = true;
};
template <typename T>
struct HashTraits<blink::Member<T>> : MemberHashTraits<T> {};

// Custom HashTraits<WeakMember<Type>> can inherit this type.
template <typename T>
struct WeakMemberHashTraits : BaseMemberHashTraits<T, blink::WeakMember<T>> {
  static constexpr bool kCanTraceConcurrently = true;
};
template <typename T>
struct HashTraits<blink::WeakMember<T>> : WeakMemberHashTraits<T> {};

// Custom HashTraits<UntracedMember<Type>> can inherit this type.
template <typename T>
struct UntracedMemberHashTraits
    : BaseMemberHashTraits<T, blink::UntracedMember<T>> {};
template <typename T>
struct HashTraits<blink::UntracedMember<T>> : UntracedMemberHashTraits<T> {};

template <typename T>
class MemberConstructTraits {
  STATIC_ONLY(MemberConstructTraits);

 public:
  template <typename... Args>
  static T* Construct(void* location, Args&&... args) {
    // `Construct()` creates a new Member which must not be visible to the
    // concurrent marker yet, similar to regular ctors in Member.
    return new (NotNullTag::kNotNull, location) T(std::forward<Args>(args)...);
  }

  template <typename... Args>
  static T* ConstructAndNotifyElement(void* location, Args&&... args) {
    // `ConstructAndNotifyElement()` updates an existing Member which might
    // also be concurrently traced while we update it. The regular ctors
    // for Member don't use an atomic write which can lead to data races.
    T* object = new (NotNullTag::kNotNull, location)
        T(std::forward<Args>(args)..., typename T::AtomicInitializerTag());
    NotifyNewElement(object);
    return object;
  }

  static void NotifyNewElement(T* element) {
    blink::WriteBarrier::DispatchForObject(element);
  }

  static void NotifyNewElements(T* array, size_t len) {
    // Checking the first element is sufficient for determining whether a
    // marking or generational barrier is required.
    if (LIKELY((len == 0) || !blink::WriteBarrier::IsWriteBarrierNeeded(array)))
      return;

    while (len-- > 0) {
      blink::WriteBarrier::DispatchForObject(array);
      array++;
    }
  }
};

template <typename T, typename Traits, typename Allocator>
class ConstructTraits<blink::Member<T>, Traits, Allocator> final
    : public MemberConstructTraits<blink::Member<T>> {};

template <typename T, typename Traits, typename Allocator>
class ConstructTraits<blink::WeakMember<T>, Traits, Allocator> final
    : public MemberConstructTraits<blink::WeakMember<T>> {};

template <typename T>
struct IsPointerType<blink::WeakMember<T>> : std::true_type {};

template <typename T>
struct IsPointerType<blink::Member<T>> : std::true_type {};

}  // namespace WTF

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_HEAP_MEMBER_H_
