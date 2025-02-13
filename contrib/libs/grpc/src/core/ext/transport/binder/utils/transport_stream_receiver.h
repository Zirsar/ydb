// Copyright 2021 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GRPC_CORE_EXT_TRANSPORT_BINDER_UTILS_TRANSPORT_STREAM_RECEIVER_H
#define GRPC_CORE_EXT_TRANSPORT_BINDER_UTILS_TRANSPORT_STREAM_RECEIVER_H

#include <grpc/support/port_platform.h>

#include <functional>
#include <util/generic/string.h>
#include <util/string/cast.h>
#include <vector>

#include "y_absl/status/statusor.h"

#include "src/core/ext/transport/binder/wire_format/transaction.h"

namespace grpc_binder {

typedef int StreamIdentifier;

class TransportStreamReceiver {
 public:
  virtual ~TransportStreamReceiver() = default;

  using InitialMetadataCallbackType =
      std::function<void(y_absl::StatusOr<Metadata>)>;
  using MessageDataCallbackType =
      std::function<void(y_absl::StatusOr<TString>)>;
  using TrailingMetadataCallbackType =
      std::function<void(y_absl::StatusOr<Metadata>, int)>;

  // Only handles single time invocation. Callback object will be deleted.
  // The callback should be valid until invocation or unregister.
  virtual void RegisterRecvInitialMetadata(StreamIdentifier id,
                                           InitialMetadataCallbackType cb) = 0;
  virtual void RegisterRecvMessage(StreamIdentifier id,
                                   MessageDataCallbackType cb) = 0;
  virtual void RegisterRecvTrailingMetadata(
      StreamIdentifier id, TrailingMetadataCallbackType cb) = 0;

  // For the following functions, the second arguments are the transaction
  // result received from the lower level. If it is None, that means there's
  // something wrong when receiving the corresponding transaction. In such case,
  // we should cancel the gRPC callback as well.
  virtual void NotifyRecvInitialMetadata(
      StreamIdentifier id, y_absl::StatusOr<Metadata> initial_metadata) = 0;
  virtual void NotifyRecvMessage(StreamIdentifier id,
                                 y_absl::StatusOr<TString> message) = 0;
  virtual void NotifyRecvTrailingMetadata(
      StreamIdentifier id, y_absl::StatusOr<Metadata> trailing_metadata,
      int status) = 0;
  // Remove all entries associated with stream number `id`.
  virtual void CancelStream(StreamIdentifier id) = 0;

  static const y_absl::string_view kGrpcBinderTransportCancelledGracefully;
};

}  // namespace grpc_binder

#endif  // GRPC_CORE_EXT_TRANSPORT_BINDER_UTILS_TRANSPORT_STREAM_RECEIVER_H
