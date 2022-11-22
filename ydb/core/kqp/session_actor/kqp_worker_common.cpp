#include "kqp_worker_common.h"

namespace NKikimr::NKqp {

using namespace NYql;

namespace {

bool IsSameProtoTypeImpl(const NKikimrMiniKQL::TDataType& actual, const NKikimrMiniKQL::TDataType& expected) {
    return actual.GetScheme() == expected.GetScheme() &&
        actual.GetDecimalParams().GetPrecision() == expected.GetDecimalParams().GetPrecision() &&
        actual.GetDecimalParams().GetScale() == expected.GetDecimalParams().GetScale();
}

bool IsSameProtoTypeImpl(const NKikimrMiniKQL::TTupleType& actual, const NKikimrMiniKQL::TTupleType& expected) {
    size_t size = actual.ElementSize();
    if (size != expected.ElementSize()) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        if (!IsSameProtoType(actual.GetElement(i), expected.GetElement(i))) {
            return false;
        }
    }
    return true;
}

bool IsSameProtoTypeImpl(const NKikimrMiniKQL::TStructType& actual, const NKikimrMiniKQL::TStructType& expected) {
    size_t size = actual.MemberSize();
    if (size != expected.MemberSize()) {
        return false;
    }
    std::map<TString, NKikimrMiniKQL::TType> expected_fields;
    for (size_t i = 0; i < size; ++i) {
        auto& st = expected.GetMember(i);
        expected_fields.emplace(st.GetName(), st.GetType());
    }
    for (size_t i = 0; i < size; ++i) {
        auto& f = actual.GetMember(i);
        auto it = expected_fields.find(f.GetName());
        if (it == expected_fields.end()) {
            return false;
        }

        if (!IsSameProtoType(f.GetType(), it->second)) {
            return false;
        }
    }
    return true;
}

bool IsSameProtoTypeImpl(const NKikimrMiniKQL::TVariantType& actual, const NKikimrMiniKQL::TVariantType& expected) {
    if (actual.GetTypeCase() != expected.GetTypeCase()) {
        return false;
    }
    switch (actual.GetTypeCase()) {
        case NKikimrMiniKQL::TVariantType::kTupleItems:
            return IsSameProtoTypeImpl(actual.GetTupleItems(), expected.GetTupleItems());
        case NKikimrMiniKQL::TVariantType::kStructItems:
            return IsSameProtoTypeImpl(actual.GetStructItems(), expected.GetStructItems());
        case NKikimrMiniKQL::TVariantType::TYPE_NOT_SET:
            Y_ENSURE(false, "Variant type not set");
            return false;
    }
}

} // namespace

EKikimrStatsMode GetStatsModeInt(const NKikimrKqp::TQueryRequest& queryRequest) {
    switch (queryRequest.GetCollectStats()) {
        case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_NONE:
            return EKikimrStatsMode::None;
        case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_BASIC:
            return EKikimrStatsMode::Basic;
        case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL:
            return EKikimrStatsMode::Full;
        case Ydb::Table::QueryStatsCollection::STATS_COLLECTION_PROFILE:
            return EKikimrStatsMode::Profile;
        default:
            return EKikimrStatsMode::None;
    }
}

TKikimrQueryLimits GetQueryLimits(const TKqpWorkerSettings& settings) {
    const auto& queryLimitsProto = settings.Service.GetQueryLimits();
    const auto& phaseLimitsProto = queryLimitsProto.GetPhaseLimits();

    TKikimrQueryLimits queryLimits;
    auto& phaseLimits = queryLimits.PhaseLimits;
    phaseLimits.AffectedShardsLimit = phaseLimitsProto.GetAffectedShardsLimit();
    phaseLimits.ReadsetCountLimit = phaseLimitsProto.GetReadsetCountLimit();
    phaseLimits.ComputeNodeMemoryLimitBytes = phaseLimitsProto.GetComputeNodeMemoryLimitBytes();
    phaseLimits.TotalReadSizeLimitBytes = phaseLimitsProto.GetTotalReadSizeLimitBytes();

    return queryLimits;
}

void SlowLogQuery(const TActorContext &ctx, const TKikimrConfiguration* config, const TKqpRequestInfo& requestInfo,
    const TDuration& duration, Ydb::StatusIds::StatusCode status, const TString& userToken, ui64 parametersSize,
    NKikimrKqp::TEvQueryResponse *record, const std::function<TString()> extractQueryText)
{
    auto logSettings = ctx.LoggerSettings();
    if (!logSettings) {
        return;
    }

    ui32 thresholdMs = 0;
    NActors::NLog::EPriority priority;

    if (logSettings->Satisfies(NActors::NLog::PRI_TRACE, NKikimrServices::KQP_SLOW_LOG)) {
        priority = NActors::NLog::PRI_TRACE;
        thresholdMs = config->_KqpSlowLogTraceThresholdMs.Get().GetRef();
    } else if (logSettings->Satisfies(NActors::NLog::PRI_NOTICE, NKikimrServices::KQP_SLOW_LOG)) {
        priority = NActors::NLog::PRI_NOTICE;
        thresholdMs = config->_KqpSlowLogNoticeThresholdMs.Get().GetRef();
    } else if (logSettings->Satisfies(NActors::NLog::PRI_WARN, NKikimrServices::KQP_SLOW_LOG)) {
        priority = NActors::NLog::PRI_WARN;
        thresholdMs = config->_KqpSlowLogWarningThresholdMs.Get().GetRef();
    } else {
        return;
    }

    if (duration >= TDuration::MilliSeconds(thresholdMs)) {
        auto username = NACLib::TUserToken(userToken).GetUserSID();
        if (username.empty()) {
            username = "UNAUTHENTICATED";
        }

        Y_VERIFY_DEBUG(extractQueryText);
        auto queryText = extractQueryText();

        auto paramsText = TStringBuilder()
            << ToString(parametersSize)
            << 'b';

        ui64 resultsSize = 0;
        for (auto& result : record->GetResponse().GetResults()) {
            resultsSize += result.ByteSize();
        }

        LOG_LOG_S(ctx, priority, NKikimrServices::KQP_SLOW_LOG, requestInfo
            << "Slow query, duration: " << duration.ToString()
            << ", status: " << status
            << ", user: " << username
            << ", results: " << resultsSize << 'b'
            << ", text: \"" << EscapeC(queryText) << '"'
            << ", parameters: " << paramsText);
    }
}

bool IsSameProtoType(const NKikimrMiniKQL::TType& actual, const NKikimrMiniKQL::TType& expected) {
    if (actual.GetKind() != expected.GetKind()) {
        return false;
    }

    switch (actual.GetKind()) {
        case NKikimrMiniKQL::ETypeKind::Void:
            return true;
        case NKikimrMiniKQL::ETypeKind::Data:
            return IsSameProtoTypeImpl(actual.GetData(), expected.GetData());
        case NKikimrMiniKQL::ETypeKind::Optional:
            return IsSameProtoType(actual.GetOptional().GetItem(), expected.GetOptional().GetItem());
        case NKikimrMiniKQL::ETypeKind::List:
            return IsSameProtoType(actual.GetList().GetItem(), expected.GetList().GetItem());
        case NKikimrMiniKQL::ETypeKind::Tuple:
            return IsSameProtoTypeImpl(actual.GetTuple(), expected.GetTuple());
        case NKikimrMiniKQL::ETypeKind::Struct:
            return IsSameProtoTypeImpl(actual.GetStruct(), expected.GetStruct());
        case NKikimrMiniKQL::ETypeKind::Dict:
            return IsSameProtoType(actual.GetDict().GetKey(), expected.GetDict().GetKey()) &&
                IsSameProtoType(actual.GetDict().GetPayload(), expected.GetDict().GetPayload());
        case NKikimrMiniKQL::ETypeKind::Variant:
            return IsSameProtoTypeImpl(actual.GetVariant(), expected.GetVariant());
        case NKikimrMiniKQL::ETypeKind::Null:
            return true;
        case NKikimrMiniKQL::ETypeKind::Pg:
            return actual.GetPg().Getoid() == expected.GetPg().Getoid();
        case NKikimrMiniKQL::ETypeKind::Unknown:
        case NKikimrMiniKQL::ETypeKind::Reserved_11:
        case NKikimrMiniKQL::ETypeKind::Reserved_12:
        case NKikimrMiniKQL::ETypeKind::Reserved_13:
        case NKikimrMiniKQL::ETypeKind::Reserved_14:
            return false;
    }
}

} // namespace NKikimr::NKqp