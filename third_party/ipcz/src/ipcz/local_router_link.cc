// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/local_router_link.h"

#include <sstream>
#include <utility>

#include "ipcz/link_side.h"
#include "ipcz/link_type.h"
#include "ipcz/router.h"
#include "ipcz/router_link_state.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/ordered_mutex.h"
#include "util/ref_counted.h"

namespace ipcz {

// This object is shared between the two Routers on either end of a
// LocalRouterLink. The Routers access each other through references owned by
// this object.
class LocalRouterLink::SharedState
    : public RefCounted<LocalRouterLink::SharedState> {
 public:
  SharedState(LinkType type,
              LocalRouterLink::InitialState initial_state,
              Ref<Router> router_a,
              Ref<Router> router_b)
      : type_(type),
        router_a_(std::move(router_a)),
        router_b_(std::move(router_b)) {
    if (initial_state == LocalRouterLink::kStable) {
      link_state_.status = RouterLinkState::kStable;
    }
  }

  LinkType type() const { return type_; }

  // [Replay] Unordered, and deliberately not used to make decisions.  It backs
  // RouterLink::GetLinkState(), which no caller invokes on a local link -- only
  // RemoteRouterLink reads its own.  Every decision this link derives from
  // `link_state_` goes through the ordered accessors below.  DSRC-416.
  RouterLinkState& link_state() { return link_state_; }

  // Returns the Router on the given `side` of this link. Note that this may
  // return null if the Router in question has been deactivated, for example due
  // to the application closing the Router's controlling portal.
  Ref<Router> GetRouter(LinkSide side) {
    OrderedMutexLock lock(&mutex_);
    switch (side.value()) {
      case LinkSide::kA:
        return router_a_;

      case LinkSide::kB:
        return router_b_;
    }
  }

  void Deactivate(LinkSide side) {
    OrderedMutexLock lock(&mutex_);
    switch (side.value()) {
      case LinkSide::kA:
        router_a_.reset();
        break;

      case LinkSide::kB:
        router_b_.reset();
        break;
    }
  }

  // [Replay] Every decision taken from `link_state_` is made here, under the
  // same ordered mutex that guards the Router refs, so the order in which the
  // two ends reach the status word is recorded instead of being thread arrival
  // order.
  //
  // `RouterLinkState::status` is a bare `std::atomic<uint32_t>` CAS'd from both
  // ends of the link, and the two ends run on different sequences: one thread
  // serializes a router (Router::SerializeNewRouter -> TryLockForBypass) while
  // another propagates closure or marks its side stable (Router::Flush ->
  // TryLockForClosure / MarkSideStable).  Nothing serialised those CASes.
  // Router::mutex_ does not: each end takes its OWN router's mutex, and this
  // word is shared between the two.  Measured: SerializeNewRouter took the
  // local-peer arm in a replay whose recording took the proxy arm (13 reports)
  // and the reverse (3 reports) on one binary -- thread arrival order, not
  // data.  DSRC-416.
  //
  // RemoteRouterLink records its decisions rather than ordering them
  // (DSRC-241/245) because its RouterLinkState lives in a fragment the peer
  // PROCESS writes, which no ordering in this process can reach.  A local
  // link's state is this process's own, so it can be ordered -- and ordering it
  // keeps the word and the decisions consistent, instead of pinning answers
  // over a word that still races.
  void MarkSideStable(LinkSide side) {
    OrderedMutexLock lock(&mutex_);
    link_state_.SetSideStable(side);
  }

  bool TryLockForBypass(LinkSide side,
                        const NodeName& bypass_request_source) {
    OrderedMutexLock lock(&mutex_);
    if (!link_state_.TryLock(side)) {
      return false;
    }
    link_state_.allowed_bypass_request_source.StoreRelease(
        bypass_request_source);
    return true;
  }

  bool TryLockForClosure(LinkSide side) {
    OrderedMutexLock lock(&mutex_);
    return link_state_.TryLock(side);
  }

  void UnlockLink(LinkSide side) {
    OrderedMutexLock lock(&mutex_);
    link_state_.Unlock(side);
  }

  bool ResetWaitingBit(LinkSide side) {
    OrderedMutexLock lock(&mutex_);
    return link_state_.ResetWaitingBit(side);
  }

  bool CanNodeRequestBypass(LinkSide side,
                            const NodeName& bypass_request_source) {
    OrderedMutexLock lock(&mutex_);
    // Balanced by a release in TryLockForBypass().
    const NodeName allowed_source =
        link_state_.allowed_bypass_request_source.LoadAcquire();
    return link_state_.is_locked_by(side.opposite()) &&
           allowed_source == bypass_request_source;
  }

 private:
  friend class RefCounted<SharedState>;

  ~SharedState() = default;

  const LinkType type_;

  // [Replay] Ordered, so the interleaving between the two ends of this link --
  // which run on different sequences and share `link_state_` -- is reproduced
  // in a replay.  DSRC-416, following DSRC-276 for Router::mutex_.
  OrderedMutex mutex_{"ipcz::LocalRouterLink::SharedState::mutex_"};
  RouterLinkState link_state_;
  Ref<Router> router_a_ ABSL_GUARDED_BY(mutex_);
  Ref<Router> router_b_ ABSL_GUARDED_BY(mutex_);
};

// static
RouterLink::Pair LocalRouterLink::CreatePair(LinkType type,
                                             const Router::Pair& routers,
                                             InitialState initial_state) {
  ABSL_ASSERT(type == LinkType::kCentral || type == LinkType::kBridge);
  auto state = MakeRefCounted<SharedState>(type, initial_state, routers.first,
                                           routers.second);
  auto a = AdoptRef(new LocalRouterLink(LinkSide::kA, state));
  auto b = AdoptRef(new LocalRouterLink(LinkSide::kB, state));
  return {a, b};
}

LocalRouterLink::LocalRouterLink(LinkSide side, Ref<SharedState> state)
    : side_(side), state_(std::move(state)) {}

LocalRouterLink::~LocalRouterLink() = default;

LinkType LocalRouterLink::GetType() const {
  return state_->type();
}

RouterLinkState* LocalRouterLink::GetLinkState() const {
  return &state_->link_state();
}

void LocalRouterLink::WaitForLinkStateAsync(std::function<void()> callback) {
  callback();
}

Ref<Router> LocalRouterLink::GetLocalPeer() {
  return state_->GetRouter(side_.opposite());
}

RemoteRouterLink* LocalRouterLink::AsRemoteRouterLink() {
  return nullptr;
}

void LocalRouterLink::AllocateParcelData(size_t num_bytes,
                                         bool allow_partial,
                                         Parcel& parcel) {
  parcel.AllocateData(num_bytes, allow_partial, /*memory=*/nullptr);
}

void LocalRouterLink::AcceptParcel(std::unique_ptr<Parcel> parcel) {
  if (Ref<Router> receiver = state_->GetRouter(side_.opposite())) {
    if (state_->type() == LinkType::kCentral) {
      receiver->AcceptInboundParcel(std::move(parcel));
    } else {
      ABSL_ASSERT(state_->type() == LinkType::kBridge);
      receiver->AcceptOutboundParcel(std::move(parcel));
    }
  }
}

void LocalRouterLink::AcceptRouteClosure(SequenceNumber sequence_length) {
  if (Ref<Router> receiver = state_->GetRouter(side_.opposite())) {
    receiver->AcceptRouteClosureFrom(state_->type(), sequence_length);
  }
}

void LocalRouterLink::AcceptRouteDisconnected() {
  if (Ref<Router> receiver = state_->GetRouter(side_.opposite())) {
    receiver->AcceptRouteDisconnectedFrom(state_->type());
  }
}

void LocalRouterLink::MarkSideStable() {
  state_->MarkSideStable(side_);
}

bool LocalRouterLink::TryLockForBypass(const NodeName& bypass_request_source) {
  return state_->TryLockForBypass(side_, bypass_request_source);
}

bool LocalRouterLink::TryLockForClosure() {
  return state_->TryLockForClosure(side_);
}

void LocalRouterLink::Unlock() {
  state_->UnlockLink(side_);
}

bool LocalRouterLink::FlushOtherSideIfWaiting() {
  const LinkSide other_side = side_.opposite();
  if (!state_->ResetWaitingBit(other_side)) {
    return false;
  }

  // The mutex is released before calling into the Router: RouterLink's class
  // comment forbids holding a link's own lock across a Router call, and the
  // ordered mutex must stay a leaf so it cannot invert against Router::mutex_.
  if (Ref<Router> receiver = state_->GetRouter(side_.opposite())) {
    receiver->Flush(Router::kForceProxyBypassAttempt);
  }
  return true;
}

bool LocalRouterLink::CanNodeRequestBypass(
    const NodeName& bypass_request_source) {
  return state_->CanNodeRequestBypass(side_, bypass_request_source);
}

void LocalRouterLink::BypassPeer(const NodeName& bypass_target_node,
                                 SublinkId bypass_target_sublink) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::StopProxying(SequenceNumber inbound_sequence_length,
                                   SequenceNumber outbound_sequence_length) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::ProxyWillStop(SequenceNumber inbound_sequence_length) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::BypassPeerWithLink(
    SublinkId new_sublink,
    FragmentRef<RouterLinkState> new_link_state,
    SequenceNumber inbound_sequence_length) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::StopProxyingToLocalPeer(
    SequenceNumber outbound_sequence_length) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::Deactivate() {
  state_->Deactivate(side_);
}

std::string LocalRouterLink::Describe() const {
  std::stringstream ss;
  ss << side_.ToString() << "-side link to local peer "
     << state_->GetRouter(side_.opposite()).get() << " on "
     << side_.opposite().ToString() << " side";
  return ss.str();
}

}  // namespace ipcz
