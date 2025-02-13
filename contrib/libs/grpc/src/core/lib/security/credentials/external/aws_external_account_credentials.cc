//
// Copyright 2020 gRPC authors.
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
//
#include <grpc/support/port_platform.h>

#include "src/core/lib/security/credentials/external/aws_external_account_credentials.h"

#include "y_absl/strings/str_format.h"
#include "y_absl/strings/str_join.h"
#include "y_absl/strings/str_replace.h"

#include "src/core/lib/gpr/env.h"
#include "src/core/lib/http/httpcli_ssl_credentials.h"

namespace grpc_core {

namespace {

const char* kExpectedEnvironmentId = "aws1";

const char* kRegionEnvVar = "AWS_REGION";
const char* kDefaultRegionEnvVar = "AWS_DEFAULT_REGION";
const char* kAccessKeyIdEnvVar = "AWS_ACCESS_KEY_ID";
const char* kSecretAccessKeyEnvVar = "AWS_SECRET_ACCESS_KEY";
const char* kSessionTokenEnvVar = "AWS_SESSION_TOKEN";

TString UrlEncode(const y_absl::string_view& s) {
  const char* hex = "0123456789ABCDEF";
  TString result;
  result.reserve(s.length());
  for (auto c : s) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '!' ||
        c == '\'' || c == '(' || c == ')' || c == '*' || c == '~' || c == '.') {
      result.push_back(c);
    } else {
      result.push_back('%');
      result.push_back(hex[static_cast<unsigned char>(c) >> 4]);
      result.push_back(hex[static_cast<unsigned char>(c) & 15]);
    }
  }
  return result;
}

}  // namespace

RefCountedPtr<AwsExternalAccountCredentials>
AwsExternalAccountCredentials::Create(Options options,
                                      std::vector<TString> scopes,
                                      grpc_error_handle* error) {
  auto creds = MakeRefCounted<AwsExternalAccountCredentials>(
      std::move(options), std::move(scopes), error);
  if (*error == GRPC_ERROR_NONE) {
    return creds;
  } else {
    return nullptr;
  }
}

AwsExternalAccountCredentials::AwsExternalAccountCredentials(
    Options options, std::vector<TString> scopes, grpc_error_handle* error)
    : ExternalAccountCredentials(options, std::move(scopes)) {
  audience_ = options.audience;
  auto it = options.credential_source.object_value().find("environment_id");
  if (it == options.credential_source.object_value().end()) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "environment_id field not present.");
    return;
  }
  if (it->second.type() != Json::Type::STRING) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "environment_id field must be a string.");
    return;
  }
  if (it->second.string_value() != kExpectedEnvironmentId) {
    *error =
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("environment_id does not match.");
    return;
  }
  it = options.credential_source.object_value().find("region_url");
  if (it == options.credential_source.object_value().end()) {
    *error =
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("region_url field not present.");
    return;
  }
  if (it->second.type() != Json::Type::STRING) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "region_url field must be a string.");
    return;
  }
  region_url_ = it->second.string_value();
  it = options.credential_source.object_value().find("url");
  if (it != options.credential_source.object_value().end() &&
      it->second.type() == Json::Type::STRING) {
    url_ = it->second.string_value();
  }
  it = options.credential_source.object_value().find(
      "regional_cred_verification_url");
  if (it == options.credential_source.object_value().end()) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "regional_cred_verification_url field not present.");
    return;
  }
  if (it->second.type() != Json::Type::STRING) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "regional_cred_verification_url field must be a string.");
    return;
  }
  regional_cred_verification_url_ = it->second.string_value();
}

void AwsExternalAccountCredentials::RetrieveSubjectToken(
    HTTPRequestContext* ctx, const Options& /*options*/,
    std::function<void(TString, grpc_error_handle)> cb) {
  if (ctx == nullptr) {
    FinishRetrieveSubjectToken(
        "",
        GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "Missing HTTPRequestContext to start subject token retrieval."));
    return;
  }
  ctx_ = ctx;
  cb_ = cb;
  if (signer_ != nullptr) {
    BuildSubjectToken();
  } else {
    RetrieveRegion();
  }
}

void AwsExternalAccountCredentials::RetrieveRegion() {
  UniquePtr<char> region_from_env(gpr_getenv(kRegionEnvVar));
  if (region_from_env == nullptr) {
    region_from_env = UniquePtr<char>(gpr_getenv(kDefaultRegionEnvVar));
  }
  if (region_from_env != nullptr) {
    region_ = TString(region_from_env.get());
    if (url_.empty()) {
      RetrieveSigningKeys();
    } else {
      RetrieveRoleName();
    }
    return;
  }
  y_absl::StatusOr<URI> uri = URI::Parse(region_url_);
  if (!uri.ok()) {
    FinishRetrieveSubjectToken(
        "", GRPC_ERROR_CREATE_FROM_CPP_STRING(y_absl::StrFormat(
                "Invalid region url. %s", uri.status().ToString())));
    return;
  }
  grpc_http_request request;
  memset(&request, 0, sizeof(grpc_http_request));
  grpc_http_response_destroy(&ctx_->response);
  ctx_->response = {};
  GRPC_CLOSURE_INIT(&ctx_->closure, OnRetrieveRegion, this, nullptr);
  RefCountedPtr<grpc_channel_credentials> http_request_creds;
  if (uri->scheme() == "http") {
    http_request_creds = RefCountedPtr<grpc_channel_credentials>(
        grpc_insecure_credentials_create());
  } else {
    http_request_creds = CreateHttpRequestSSLCredentials();
  }
  http_request_ =
      HttpRequest::Get(std::move(*uri), nullptr /* channel args */,
                       ctx_->pollent, &request, ctx_->deadline, &ctx_->closure,
                       &ctx_->response, std::move(http_request_creds));
  http_request_->Start();
  grpc_http_request_destroy(&request);
}

void AwsExternalAccountCredentials::OnRetrieveRegion(void* arg,
                                                     grpc_error_handle error) {
  AwsExternalAccountCredentials* self =
      static_cast<AwsExternalAccountCredentials*>(arg);
  self->OnRetrieveRegionInternal(GRPC_ERROR_REF(error));
}

void AwsExternalAccountCredentials::OnRetrieveRegionInternal(
    grpc_error_handle error) {
  if (error != GRPC_ERROR_NONE) {
    FinishRetrieveSubjectToken("", error);
    return;
  }
  // Remove the last letter of availability zone to get pure region
  y_absl::string_view response_body(ctx_->response.body,
                                  ctx_->response.body_length);
  region_ = TString(response_body.substr(0, response_body.size() - 1));
  if (url_.empty()) {
    RetrieveSigningKeys();
  } else {
    RetrieveRoleName();
  }
}

void AwsExternalAccountCredentials::RetrieveRoleName() {
  y_absl::StatusOr<URI> uri = URI::Parse(url_);
  if (!uri.ok()) {
    FinishRetrieveSubjectToken(
        "", GRPC_ERROR_CREATE_FROM_CPP_STRING(
                y_absl::StrFormat("Invalid url: %s.", uri.status().ToString())));
    return;
  }
  grpc_http_request request;
  memset(&request, 0, sizeof(grpc_http_request));
  grpc_http_response_destroy(&ctx_->response);
  ctx_->response = {};
  GRPC_CLOSURE_INIT(&ctx_->closure, OnRetrieveRoleName, this, nullptr);
  // TODO(ctiller): use the caller's resource quota.
  RefCountedPtr<grpc_channel_credentials> http_request_creds;
  if (uri->scheme() == "http") {
    http_request_creds = RefCountedPtr<grpc_channel_credentials>(
        grpc_insecure_credentials_create());
  } else {
    http_request_creds = CreateHttpRequestSSLCredentials();
  }
  http_request_ =
      HttpRequest::Get(std::move(*uri), nullptr /* channel args */,
                       ctx_->pollent, &request, ctx_->deadline, &ctx_->closure,
                       &ctx_->response, std::move(http_request_creds));
  http_request_->Start();
  grpc_http_request_destroy(&request);
}

void AwsExternalAccountCredentials::OnRetrieveRoleName(
    void* arg, grpc_error_handle error) {
  AwsExternalAccountCredentials* self =
      static_cast<AwsExternalAccountCredentials*>(arg);
  self->OnRetrieveRoleNameInternal(GRPC_ERROR_REF(error));
}

void AwsExternalAccountCredentials::OnRetrieveRoleNameInternal(
    grpc_error_handle error) {
  if (error != GRPC_ERROR_NONE) {
    FinishRetrieveSubjectToken("", error);
    return;
  }
  role_name_ = TString(ctx_->response.body, ctx_->response.body_length);
  RetrieveSigningKeys();
}

void AwsExternalAccountCredentials::RetrieveSigningKeys() {
  UniquePtr<char> access_key_id_from_env(gpr_getenv(kAccessKeyIdEnvVar));
  UniquePtr<char> secret_access_key_from_env(
      gpr_getenv(kSecretAccessKeyEnvVar));
  UniquePtr<char> token_from_env(gpr_getenv(kSessionTokenEnvVar));
  if (access_key_id_from_env != nullptr &&
      secret_access_key_from_env != nullptr && token_from_env != nullptr) {
    access_key_id_ = TString(access_key_id_from_env.get());
    secret_access_key_ = TString(secret_access_key_from_env.get());
    token_ = TString(token_from_env.get());
    BuildSubjectToken();
    return;
  }
  if (role_name_.empty()) {
    FinishRetrieveSubjectToken(
        "", GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                "Missing role name when retrieving signing keys."));
    return;
  }
  TString url_with_role_name = y_absl::StrCat(url_, "/", role_name_);
  y_absl::StatusOr<URI> uri = URI::Parse(url_with_role_name);
  if (!uri.ok()) {
    FinishRetrieveSubjectToken(
        "", GRPC_ERROR_CREATE_FROM_CPP_STRING(y_absl::StrFormat(
                "Invalid url with role name: %s.", uri.status().ToString())));
    return;
  }
  grpc_http_request request;
  memset(&request, 0, sizeof(grpc_http_request));
  grpc_http_response_destroy(&ctx_->response);
  ctx_->response = {};
  GRPC_CLOSURE_INIT(&ctx_->closure, OnRetrieveSigningKeys, this, nullptr);
  // TODO(ctiller): use the caller's resource quota.
  RefCountedPtr<grpc_channel_credentials> http_request_creds;
  if (uri->scheme() == "http") {
    http_request_creds = RefCountedPtr<grpc_channel_credentials>(
        grpc_insecure_credentials_create());
  } else {
    http_request_creds = CreateHttpRequestSSLCredentials();
  }
  http_request_ =
      HttpRequest::Get(std::move(*uri), nullptr /* channel args */,
                       ctx_->pollent, &request, ctx_->deadline, &ctx_->closure,
                       &ctx_->response, std::move(http_request_creds));
  http_request_->Start();
  grpc_http_request_destroy(&request);
}

void AwsExternalAccountCredentials::OnRetrieveSigningKeys(
    void* arg, grpc_error_handle error) {
  AwsExternalAccountCredentials* self =
      static_cast<AwsExternalAccountCredentials*>(arg);
  self->OnRetrieveSigningKeysInternal(GRPC_ERROR_REF(error));
}

void AwsExternalAccountCredentials::OnRetrieveSigningKeysInternal(
    grpc_error_handle error) {
  if (error != GRPC_ERROR_NONE) {
    FinishRetrieveSubjectToken("", error);
    return;
  }
  y_absl::string_view response_body(ctx_->response.body,
                                  ctx_->response.body_length);
  Json json = Json::Parse(response_body, &error);
  if (error != GRPC_ERROR_NONE || json.type() != Json::Type::OBJECT) {
    FinishRetrieveSubjectToken(
        "", GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
                "Invalid retrieve signing keys response.", &error, 1));
    GRPC_ERROR_UNREF(error);
    return;
  }
  auto it = json.object_value().find("AccessKeyId");
  if (it != json.object_value().end() &&
      it->second.type() == Json::Type::STRING) {
    access_key_id_ = it->second.string_value();
  } else {
    FinishRetrieveSubjectToken(
        "", GRPC_ERROR_CREATE_FROM_CPP_STRING(y_absl::StrFormat(
                "Missing or invalid AccessKeyId in %s.", response_body)));
    return;
  }
  it = json.object_value().find("SecretAccessKey");
  if (it != json.object_value().end() &&
      it->second.type() == Json::Type::STRING) {
    secret_access_key_ = it->second.string_value();
  } else {
    FinishRetrieveSubjectToken(
        "", GRPC_ERROR_CREATE_FROM_CPP_STRING(y_absl::StrFormat(
                "Missing or invalid SecretAccessKey in %s.", response_body)));
    return;
  }
  it = json.object_value().find("Token");
  if (it != json.object_value().end() &&
      it->second.type() == Json::Type::STRING) {
    token_ = it->second.string_value();
  } else {
    FinishRetrieveSubjectToken(
        "", GRPC_ERROR_CREATE_FROM_CPP_STRING(y_absl::StrFormat(
                "Missing or invalid Token in %s.", response_body)));
    return;
  }
  BuildSubjectToken();
}

void AwsExternalAccountCredentials::BuildSubjectToken() {
  grpc_error_handle error = GRPC_ERROR_NONE;
  if (signer_ == nullptr) {
    cred_verification_url_ = y_absl::StrReplaceAll(
        regional_cred_verification_url_, {{"{region}", region_}});
    signer_ = y_absl::make_unique<AwsRequestSigner>(
        access_key_id_, secret_access_key_, token_, "POST",
        cred_verification_url_, region_, "",
        std::map<TString, TString>(), &error);
    if (error != GRPC_ERROR_NONE) {
      FinishRetrieveSubjectToken(
          "", GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
                  "Creating aws request signer failed.", &error, 1));
      GRPC_ERROR_UNREF(error);
      return;
    }
  }
  auto signed_headers = signer_->GetSignedRequestHeaders();
  if (error != GRPC_ERROR_NONE) {
    FinishRetrieveSubjectToken("",
                               GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
                                   "Invalid getting signed request"
                                   "headers.",
                                   &error, 1));
    GRPC_ERROR_UNREF(error);
    return;
  }
  // Construct subject token
  Json::Array headers;
  headers.push_back(Json(
      {{"key", "Authorization"}, {"value", signed_headers["Authorization"]}}));
  headers.push_back(Json({{"key", "host"}, {"value", signed_headers["host"]}}));
  headers.push_back(
      Json({{"key", "x-amz-date"}, {"value", signed_headers["x-amz-date"]}}));
  headers.push_back(Json({{"key", "x-amz-security-token"},
                          {"value", signed_headers["x-amz-security-token"]}}));
  headers.push_back(
      Json({{"key", "x-goog-cloud-target-resource"}, {"value", audience_}}));
  Json::Object object{{"url", Json(cred_verification_url_)},
                      {"method", Json("POST")},
                      {"headers", Json(headers)}};
  Json subject_token_json(object);
  TString subject_token = UrlEncode(subject_token_json.Dump());
  FinishRetrieveSubjectToken(subject_token, GRPC_ERROR_NONE);
}

void AwsExternalAccountCredentials::FinishRetrieveSubjectToken(
    TString subject_token, grpc_error_handle error) {
  // Reset context
  ctx_ = nullptr;
  // Move object state into local variables.
  auto cb = cb_;
  cb_ = nullptr;
  // Invoke the callback.
  if (error != GRPC_ERROR_NONE) {
    cb("", error);
  } else {
    cb(subject_token, GRPC_ERROR_NONE);
  }
}

}  // namespace grpc_core
