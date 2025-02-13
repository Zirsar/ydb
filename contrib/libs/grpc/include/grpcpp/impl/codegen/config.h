/*
 *
 * Copyright 2016 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef GRPCPP_IMPL_CODEGEN_CONFIG_H
#define GRPCPP_IMPL_CODEGEN_CONFIG_H

// IWYU pragma: private, include <grpcpp/support/config.h>

#include <util/generic/string.h>
#include <util/string/cast.h>

/// The following macros are deprecated and appear only for users
/// with PB files generated using gRPC 1.0.x plugins. They should
/// not be used in new code
#define GRPC_OVERRIDE override  // deprecated
#define GRPC_FINAL final        // deprecated

#ifdef GRPC_CUSTOM_STRING
#warning GRPC_CUSTOM_STRING is no longer supported. Please use TString.
#endif

namespace grpc {

// Using grpc::string and grpc::to_string is discouraged in favor of
// TString and ::ToString. This is only for legacy code using
// them explictly.
typedef TString string;

}  // namespace grpc

#endif  // GRPCPP_IMPL_CODEGEN_CONFIG_H
