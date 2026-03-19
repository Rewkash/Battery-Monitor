#include "ui/BatteryRuntimeEstimator.h"
#include "ui/NoiseControlUi.h"

#include <algorithm>
#include <array>

#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QStringList>

namespace battery_monitor {

namespace {

constexpr qint64 kSeriesGapBreakMs = 45LL * 60LL * 1000LL;
constexpr int kChargeJumpBreakPercent = 3;
constexpr int kBucketSizePercent = 5;
constexpr qint64 kLiveWindowMs = 60LL * 60LL * 1000LL;
constexpr int kLiveMaxPoints = 8;

struct RateAccumulator {
    double total_ms = 0.0;
    int total_steps = 0;
};

struct HistoricalLookup {
    double ms_per_percent = 0.0;
    int support_steps = 0;
    int tier = 0;
};

struct LiveRateEstimate {
    double ms_per_percent = 0.0;
    int observed_drop = 0;
    qint64 observed_ms = 0;
};

QString NormalizeToken(const QString& value) {
    return NormalizeNoiseToken(value);
}

bool IsDisconnectedLevel(int level) {
    return level <= 0;
}

int BucketForLevel(int level) {
    const int clamped = std::clamp(level, 1, 100);
    return (clamped - 1) / kBucketSizePercent;
}

QString ExactBucketKey(const QString& component, const QString& mode, const QString& submode, int bucket) {
    return component + QChar('|') + mode + QChar('|') + submode + QChar('|') + QString::number(bucket);
}

QString ExactAggregateKey(const QString& component, const QString& mode, const QString& submode) {
    return component + QChar('|') + mode + QChar('|') + submode;
}

QString ModeBucketKey(const QString& component, const QString& mode, int bucket) {
    return component + QChar('|') + mode + QChar('|') + QString::number(bucket);
}

QString ModeAggregateKey(const QString& component, const QString& mode) {
    return component + QChar('|') + mode;
}

void AddRate(QHash<QString, RateAccumulator>* store, const QString& key, double ms_per_percent) {
    if (store == nullptr || key.isEmpty() || ms_per_percent <= 0.0) {
        return;
    }

    auto& entry = (*store)[key];
    entry.total_ms += ms_per_percent;
    entry.total_steps += 1;
}

std::optional<int> SampleLevel(const BatteryHistorySample& sample, const QString& component_key) {
    const auto level_it = sample.component_levels.find(component_key);
    if (level_it == sample.component_levels.end()) {
        return std::nullopt;
    }
    return level_it.value();
}

bool IsSessionBreak(const BatteryHistorySample& previous,
                    const BatteryHistorySample& current,
                    const QString& component_key) {
    const auto previous_level = SampleLevel(previous, component_key);
    const auto current_level = SampleLevel(current, component_key);
    if (!previous_level.has_value() || !current_level.has_value()) {
        return true;
    }
    if (IsDisconnectedLevel(*previous_level) || IsDisconnectedLevel(*current_level)) {
        return true;
    }
    if ((current.timestamp_ms - previous.timestamp_ms) > kSeriesGapBreakMs) {
        return true;
    }
    if ((*current_level - *previous_level) > kChargeJumpBreakPercent) {
        return true;
    }

    const QString previous_mode = NormalizeToken(previous.device_mode);
    const QString current_mode = NormalizeToken(current.device_mode);
    const QString previous_submode = NormalizeToken(previous.device_submode);
    const QString current_submode = NormalizeToken(current.device_submode);
    return previous_mode != current_mode || previous_submode != current_submode;
}

std::vector<QString> VisibleComponents(const BatteryHistoryData& history) {
    std::vector<QString> component_keys;
    for (const auto& sample : history.samples) {
        for (auto it = sample.component_levels.cbegin(); it != sample.component_levels.cend(); ++it) {
            if (std::find(component_keys.begin(), component_keys.end(), it.key()) == component_keys.end()) {
                component_keys.push_back(it.key());
            }
        }
    }

    std::sort(component_keys.begin(), component_keys.end(), [](const QString& left, const QString& right) {
        static const std::array<QString, 4> order = {
            QStringLiteral("left"),
            QStringLiteral("right"),
            QStringLiteral("case"),
            QStringLiteral("main"),
        };
        const auto left_it = std::find(order.begin(), order.end(), left);
        const auto right_it = std::find(order.begin(), order.end(), right);
        if (left_it != right_it) {
            return left_it < right_it;
        }
        return QString::localeAwareCompare(left, right) < 0;
    });
    return component_keys;
}

struct HistoricalProfiles {
    QHash<QString, RateAccumulator> exact_buckets;
    QHash<QString, RateAccumulator> exact_aggregate;
    QHash<QString, RateAccumulator> mode_buckets;
    QHash<QString, RateAccumulator> mode_aggregate;
    QHash<QString, RateAccumulator> component_buckets;
    QHash<QString, RateAccumulator> component_aggregate;
};

HistoricalProfiles BuildHistoricalProfiles(const BatteryHistoryData& history) {
    HistoricalProfiles profiles;
    for (const auto& component_key : VisibleComponents(history)) {
        bool in_session = false;
        BatteryHistorySample previous_sample;
        int previous_level = -1;
        qint64 pending_ms = 0;

        for (const auto& sample : history.samples) {
            const auto current_level_opt = SampleLevel(sample, component_key);
            if (!current_level_opt.has_value() || IsDisconnectedLevel(*current_level_opt)) {
                in_session = false;
                pending_ms = 0;
                continue;
            }

            if (!in_session) {
                previous_sample = sample;
                previous_level = *current_level_opt;
                in_session = true;
                pending_ms = 0;
                continue;
            }

            if (IsSessionBreak(previous_sample, sample, component_key)) {
                previous_sample = sample;
                previous_level = *current_level_opt;
                pending_ms = 0;
                in_session = true;
                continue;
            }

            const qint64 delta_ms = std::max<qint64>(0, sample.timestamp_ms - previous_sample.timestamp_ms);
            pending_ms += delta_ms;
            const int current_level = *current_level_opt;

            if (current_level < previous_level) {
                const int drop = previous_level - current_level;
                const double ms_per_percent = static_cast<double>(pending_ms) / static_cast<double>(drop);
                const QString mode = NormalizeToken(sample.device_mode);
                const QString submode = NormalizeToken(sample.device_submode);
                for (int level = previous_level; level > current_level; --level) {
                    const int bucket = BucketForLevel(level);
                    AddRate(&profiles.component_buckets, component_key + QChar('|') + QString::number(bucket), ms_per_percent);
                    AddRate(&profiles.component_aggregate, component_key, ms_per_percent);
                    if (!mode.isEmpty()) {
                        AddRate(&profiles.mode_buckets, ModeBucketKey(component_key, mode, bucket), ms_per_percent);
                        AddRate(&profiles.mode_aggregate, ModeAggregateKey(component_key, mode), ms_per_percent);
                        AddRate(&profiles.exact_buckets, ExactBucketKey(component_key, mode, submode, bucket), ms_per_percent);
                        AddRate(&profiles.exact_aggregate, ExactAggregateKey(component_key, mode, submode), ms_per_percent);
                    }
                }
                pending_ms = 0;
            }

            previous_sample = sample;
            previous_level = current_level;
        }
    }

    return profiles;
}

std::optional<HistoricalLookup> MakeLookup(const RateAccumulator* accumulator, int tier) {
    if (accumulator == nullptr || accumulator->total_steps <= 0 || accumulator->total_ms <= 0.0) {
        return std::nullopt;
    }

    HistoricalLookup lookup;
    lookup.ms_per_percent = accumulator->total_ms / static_cast<double>(accumulator->total_steps);
    lookup.support_steps = accumulator->total_steps;
    lookup.tier = tier;
    return lookup;
}

std::optional<HistoricalLookup> LookupHistoricalRate(const HistoricalProfiles& profiles,
                                                     const QString& component_key,
                                                     const QString& mode,
                                                     const QString& submode,
                                                     int level) {
    const int bucket = BucketForLevel(level);
    const QString normalized_mode = NormalizeToken(mode);
    const QString normalized_submode = NormalizeToken(submode);

    if (!normalized_mode.isEmpty()) {
        const QString exact_bucket = ExactBucketKey(component_key, normalized_mode, normalized_submode, bucket);
        const auto exact_bucket_it = profiles.exact_buckets.find(exact_bucket);
        if (exact_bucket_it != profiles.exact_buckets.end() && exact_bucket_it->total_steps >= 2) {
            return MakeLookup(&exact_bucket_it.value(), 5);
        }

        const QString exact_aggregate = ExactAggregateKey(component_key, normalized_mode, normalized_submode);
        const auto exact_aggregate_it = profiles.exact_aggregate.find(exact_aggregate);
        if (exact_aggregate_it != profiles.exact_aggregate.end() && exact_aggregate_it->total_steps >= 6) {
            return MakeLookup(&exact_aggregate_it.value(), 4);
        }

        const QString mode_bucket = ModeBucketKey(component_key, normalized_mode, bucket);
        const auto mode_bucket_it = profiles.mode_buckets.find(mode_bucket);
        if (mode_bucket_it != profiles.mode_buckets.end() && mode_bucket_it->total_steps >= 3) {
            return MakeLookup(&mode_bucket_it.value(), 4);
        }

        const QString mode_aggregate = ModeAggregateKey(component_key, normalized_mode);
        const auto mode_aggregate_it = profiles.mode_aggregate.find(mode_aggregate);
        if (mode_aggregate_it != profiles.mode_aggregate.end() && mode_aggregate_it->total_steps >= 8) {
            return MakeLookup(&mode_aggregate_it.value(), 3);
        }
    }

    const QString component_bucket = component_key + QChar('|') + QString::number(bucket);
    const auto component_bucket_it = profiles.component_buckets.find(component_bucket);
    if (component_bucket_it != profiles.component_buckets.end() && component_bucket_it->total_steps >= 3) {
        return MakeLookup(&component_bucket_it.value(), 2);
    }

    const auto component_aggregate_it = profiles.component_aggregate.find(component_key);
    if (component_aggregate_it != profiles.component_aggregate.end() && component_aggregate_it->total_steps >= 8) {
        return MakeLookup(&component_aggregate_it.value(), 1);
    }

    if (component_bucket_it != profiles.component_buckets.end()) {
        return MakeLookup(&component_bucket_it.value(), 1);
    }
    if (component_aggregate_it != profiles.component_aggregate.end()) {
        return MakeLookup(&component_aggregate_it.value(), 1);
    }
    return std::nullopt;
}

std::optional<LiveRateEstimate> ComputeLiveRate(const BatteryHistoryData& history, const QString& component_key) {
    if (history.samples.isEmpty()) {
        return std::nullopt;
    }

    QVector<const BatteryHistorySample*> tail_samples;
    tail_samples.reserve(kLiveMaxPoints);
    const BatteryHistorySample* previous = nullptr;

    for (int index = history.samples.size() - 1; index >= 0; --index) {
        const auto& sample = history.samples[index];
        const auto level = SampleLevel(sample, component_key);
        if (!level.has_value() || IsDisconnectedLevel(*level)) {
            break;
        }

        if (previous != nullptr) {
            if (IsSessionBreak(sample, *previous, component_key)) {
                break;
            }
            if ((previous->timestamp_ms - sample.timestamp_ms) > kLiveWindowMs) {
                break;
            }
        }

        tail_samples.push_back(&sample);
        previous = &sample;
        if (tail_samples.size() >= kLiveMaxPoints) {
            break;
        }
    }

    if (tail_samples.size() < 3) {
        return std::nullopt;
    }

    std::reverse(tail_samples.begin(), tail_samples.end());

    qint64 pending_ms = 0;
    qint64 total_ms = 0;
    int total_drop = 0;
    for (int index = 1; index < tail_samples.size(); ++index) {
        const auto& previous_sample = *tail_samples[index - 1];
        const auto& current_sample = *tail_samples[index];
        const int previous_level = *SampleLevel(previous_sample, component_key);
        const int current_level = *SampleLevel(current_sample, component_key);
        pending_ms += std::max<qint64>(0, current_sample.timestamp_ms - previous_sample.timestamp_ms);
        if (current_level < previous_level) {
            total_ms += pending_ms;
            total_drop += previous_level - current_level;
            pending_ms = 0;
        }
    }

    if (total_drop < 2 || total_ms <= 0) {
        return std::nullopt;
    }

    LiveRateEstimate estimate;
    estimate.ms_per_percent = static_cast<double>(total_ms) / static_cast<double>(total_drop);
    estimate.observed_drop = total_drop;
    estimate.observed_ms = total_ms;
    return estimate;
}

BatteryRuntimeConfidence ScoreConfidence(int support_steps, int tier, int live_drop) {
    int score = tier * 2;
    score += std::min(3, support_steps / 8);
    score += std::min(2, live_drop / 4);

    if (score >= 11) {
        return BatteryRuntimeConfidence::High;
    }
    if (score >= 6) {
        return BatteryRuntimeConfidence::Medium;
    }
    return BatteryRuntimeConfidence::Low;
}

QString ComponentLabel(const QString& component_key) {
    if (component_key == QStringLiteral("left")) {
        return QString::fromUtf8(u8"Левый");
    }
    if (component_key == QStringLiteral("right")) {
        return QString::fromUtf8(u8"Правый");
    }
    if (component_key == QStringLiteral("case")) {
        return QString::fromUtf8(u8"Кейс");
    }
    if (component_key == QStringLiteral("main")) {
        return QString::fromUtf8(u8"Заряд");
    }
    return component_key;
}

QString ModeLabel(const QString& mode) {
    const QString normalized = NormalizeToken(mode);
    if (normalized == QStringLiteral("off")) {
        return QString::fromUtf8(u8"Выкл");
    }
    if (normalized == QStringLiteral("anc")) {
        return QString::fromUtf8(u8"Шумоподавление");
    }
    if (normalized == QStringLiteral("transparency")) {
        return QString::fromUtf8(u8"Прозрачность");
    }
    return mode.trimmed();
}

QString SubmodeLabel(const QString& submode) {
    const QString normalized = NormalizeToken(submode);
    if (normalized == QStringLiteral("balanced")) {
        return QString::fromUtf8(u8"Баланс");
    }
    if (normalized == QStringLiteral("weak")) {
        return QString::fromUtf8(u8"Слабое");
    }
    if (normalized == QStringLiteral("deep")) {
        return QString::fromUtf8(u8"Глубокое");
    }
    if (normalized == QStringLiteral("adaptive")) {
        return QString::fromUtf8(u8"Адаптивное");
    }
    if (normalized == QStringLiteral("voice")) {
        return QString::fromUtf8(u8"Усиление голоса");
    }
    if (normalized == QStringLiteral("standard")) {
        return QString::fromUtf8(u8"Обычная прозрачность");
    }
    return submode.trimmed();
}

QString ConfidenceLabel(BatteryRuntimeConfidence confidence) {
    switch (confidence) {
        case BatteryRuntimeConfidence::High:
            return QString::fromUtf8(u8"высокая");
        case BatteryRuntimeConfidence::Medium:
            return QString::fromUtf8(u8"средняя");
        case BatteryRuntimeConfidence::Low:
        default:
            return QString::fromUtf8(u8"низкая");
    }
}

struct AverageRuntimeSummary {
    qint64 total_duration_ms = 0;
    int total_drop = 0;
};

QHash<QString, AverageRuntimeSummary> ComputeAverageRuntimeSummaries(const BatteryHistoryData& history) {
    QHash<QString, AverageRuntimeSummary> summaries;
    for (const auto& component_key : VisibleComponents(history)) {
        bool have_previous = false;
        qint64 previous_timestamp_ms = 0;
        int previous_level = -1;

        for (const auto& sample : history.samples) {
            const auto current_level_opt = SampleLevel(sample, component_key);
            const int current_level = current_level_opt.value_or(-1);
            if (!have_previous) {
                previous_timestamp_ms = sample.timestamp_ms;
                previous_level = current_level;
                have_previous = true;
                continue;
            }

            const qint64 delta_ms = sample.timestamp_ms - previous_timestamp_ms;
            const bool invalid_pair = delta_ms <= 0 ||
                                      previous_level < 0 || current_level < 0 ||
                                      IsDisconnectedLevel(previous_level) || IsDisconnectedLevel(current_level) ||
                                      delta_ms > kSeriesGapBreakMs ||
                                      (current_level - previous_level) > kChargeJumpBreakPercent;

            if (!invalid_pair) {
                auto& summary = summaries[component_key];
                summary.total_duration_ms += delta_ms;
                summary.total_drop += std::max(0, previous_level - current_level);
            }

            previous_timestamp_ms = sample.timestamp_ms;
            previous_level = current_level;
        }
    }
    return summaries;
}

struct ContextRuntimeSummary {
    QString component_key;
    QString mode;
    QString submode;
    qint64 total_duration_ms = 0;
    int total_drop = 0;
    int sessions = 0;
};

QVector<ContextRuntimeSummary> ComputeContextRuntimeSummaries(const BatteryHistoryData& history) {
    QHash<QString, ContextRuntimeSummary> summaries;

    for (const auto& component_key : VisibleComponents(history)) {
        bool in_session = false;
        BatteryHistorySample previous_sample;
        int previous_level = -1;
        qint64 session_duration_ms = 0;
        int session_drop = 0;
        QString session_mode;
        QString session_submode;

        auto flush_session = [&]() {
            if (session_drop > 0 && session_duration_ms > 0 && !session_mode.trimmed().isEmpty()) {
                const QString key = component_key + QChar('|') + NormalizeToken(session_mode) + QChar('|') +
                                    NormalizeToken(session_submode);
                auto& summary = summaries[key];
                if (summary.component_key.isEmpty()) {
                    summary.component_key = component_key;
                    summary.mode = NormalizeToken(session_mode);
                    summary.submode = NormalizeToken(session_submode);
                }
                summary.total_duration_ms += session_duration_ms;
                summary.total_drop += session_drop;
                summary.sessions += 1;
            }

            session_duration_ms = 0;
            session_drop = 0;
            session_mode.clear();
            session_submode.clear();
        };

        for (const auto& sample : history.samples) {
            const auto current_level_opt = SampleLevel(sample, component_key);
            if (!current_level_opt.has_value() || IsDisconnectedLevel(*current_level_opt)) {
                flush_session();
                in_session = false;
                continue;
            }

            if (!in_session) {
                previous_sample = sample;
                previous_level = *current_level_opt;
                session_mode = sample.device_mode;
                session_submode = sample.device_submode;
                in_session = true;
                continue;
            }

            if (IsSessionBreak(previous_sample, sample, component_key)) {
                flush_session();
                previous_sample = sample;
                previous_level = *current_level_opt;
                session_mode = sample.device_mode;
                session_submode = sample.device_submode;
                in_session = true;
                continue;
            }

            const int current_level = *current_level_opt;
            session_duration_ms += std::max<qint64>(0, sample.timestamp_ms - previous_sample.timestamp_ms);
            if (current_level < previous_level) {
                session_drop += previous_level - current_level;
            }

            previous_sample = sample;
            previous_level = current_level;
        }

        flush_session();
    }

    QVector<ContextRuntimeSummary> result;
    result.reserve(summaries.size());
    for (auto it = summaries.cbegin(); it != summaries.cend(); ++it) {
        result.push_back(it.value());
    }

    std::sort(result.begin(), result.end(), [](const ContextRuntimeSummary& left, const ContextRuntimeSummary& right) {
        static const std::array<QString, 4> order = {
            QStringLiteral("left"),
            QStringLiteral("right"),
            QStringLiteral("case"),
            QStringLiteral("main"),
        };
        const auto left_it = std::find(order.begin(), order.end(), left.component_key);
        const auto right_it = std::find(order.begin(), order.end(), right.component_key);
        if (left_it != right_it) {
            return left_it < right_it;
        }
        if (left.mode != right.mode) {
            return QString::localeAwareCompare(left.mode, right.mode) < 0;
        }
        return QString::localeAwareCompare(left.submode, right.submode) < 0;
    });
    return result;
}

struct ImbalanceSummary {
    int shared_samples = 0;
    double average_diff = 0.0;
    int max_abs_gap = 0;
    std::optional<int> latest_diff;
};

ImbalanceSummary ComputeImbalanceSummary(const BatteryHistoryData& history) {
    ImbalanceSummary summary;
    double total_diff = 0.0;
    for (const auto& sample : history.samples) {
        const auto left_level = SampleLevel(sample, QStringLiteral("left"));
        const auto right_level = SampleLevel(sample, QStringLiteral("right"));
        if (!left_level.has_value() || !right_level.has_value() ||
            IsDisconnectedLevel(*left_level) || IsDisconnectedLevel(*right_level)) {
            continue;
        }

        const int diff = *left_level - *right_level;
        total_diff += static_cast<double>(diff);
        summary.shared_samples += 1;
        summary.max_abs_gap = std::max(summary.max_abs_gap, std::abs(diff));
        summary.latest_diff = diff;
    }

    if (summary.shared_samples > 0) {
        summary.average_diff = total_diff / static_cast<double>(summary.shared_samples);
    }
    return summary;
}

}  // namespace

BatteryRuntimeForecast EstimateBatteryRuntimeForecast(const BatteryHistoryData& history) {
    BatteryRuntimeForecast forecast;
    if (history.samples.isEmpty()) {
        return forecast;
    }

    const auto& latest_sample = history.samples.back();
    const QString current_mode = NormalizeToken(latest_sample.device_mode);
    const QString current_submode = NormalizeToken(latest_sample.device_submode);
    const HistoricalProfiles profiles = BuildHistoricalProfiles(history);

    for (const auto& component_key : VisibleComponents(history)) {
        const auto current_level_opt = SampleLevel(latest_sample, component_key);
        if (!current_level_opt.has_value() || IsDisconnectedLevel(*current_level_opt)) {
            continue;
        }

        const auto live_rate = ComputeLiveRate(history, component_key);
        double remaining_ms = 0.0;
        int support_steps = 0;
        int best_tier = 0;
        bool failed = false;

        for (int level = *current_level_opt; level >= 1; --level) {
            const auto historical = LookupHistoricalRate(profiles, component_key, current_mode, current_submode, level);
            if (!historical.has_value()) {
                failed = true;
                break;
            }

            double final_rate = historical->ms_per_percent;
            if (live_rate.has_value()) {
                const double live_weight =
                    live_rate->observed_drop >= 6 ? 0.60 : (live_rate->observed_drop >= 3 ? 0.40 : 0.25);
                final_rate = (historical->ms_per_percent * (1.0 - live_weight)) +
                             (live_rate->ms_per_percent * live_weight);
            }

            remaining_ms += final_rate;
            support_steps += historical->support_steps;
            best_tier = std::max(best_tier, historical->tier);
        }

        if (failed || remaining_ms <= 0.0) {
            continue;
        }

        BatteryComponentRuntimeEstimate component_estimate;
        component_estimate.component_key = component_key;
        component_estimate.remaining_ms = static_cast<qint64>(remaining_ms);
        component_estimate.confidence =
            ScoreConfidence(support_steps,
                            best_tier,
                            live_rate.has_value() ? live_rate->observed_drop : 0);
        forecast.by_component.insert(component_key, std::move(component_estimate));
    }

    const auto left_it = forecast.by_component.find(QStringLiteral("left"));
    const auto right_it = forecast.by_component.find(QStringLiteral("right"));
    if (left_it != forecast.by_component.end() && right_it != forecast.by_component.end() &&
        left_it->remaining_ms.has_value() && right_it->remaining_ms.has_value()) {
        forecast.pair_remaining_ms = std::min(*left_it->remaining_ms, *right_it->remaining_ms);
        forecast.pair_confidence =
            left_it->confidence < right_it->confidence ? left_it->confidence : right_it->confidence;
    }

    return forecast;
}

QString FormatRuntimeDurationCompact(qint64 duration_ms) {
    const qint64 total_minutes = std::max<qint64>(1, duration_ms / (60LL * 1000LL));
    const qint64 hours = total_minutes / 60LL;
    const qint64 minutes = total_minutes % 60LL;
    if (hours <= 0) {
        return QString::fromUtf8(u8"%1 м").arg(total_minutes);
    }
    if (minutes == 0) {
        return QString::fromUtf8(u8"%1 ч").arg(hours);
    }
    return QString::fromUtf8(u8"%1 ч %2 м").arg(hours).arg(minutes);
}

QString BuildRuntimeForecastSummary(const BatteryRuntimeForecast& forecast) {
    QStringList parts;
    const std::array<QString, 4> order = {
        QStringLiteral("left"),
        QStringLiteral("right"),
        QStringLiteral("case"),
        QStringLiteral("main"),
    };

    for (const auto& key : order) {
        const auto found = forecast.by_component.find(key);
        if (found == forecast.by_component.end() || !found->remaining_ms.has_value()) {
            continue;
        }

        parts.push_back(QStringLiteral("%1 %2")
                            .arg(ComponentLabel(key))
                            .arg(FormatRuntimeDurationCompact(*found->remaining_ms)));
    }

    if (forecast.pair_remaining_ms.has_value()) {
        parts.push_back(QString::fromUtf8(u8"Пара %1").arg(FormatRuntimeDurationCompact(*forecast.pair_remaining_ms)));
    }

    if (parts.isEmpty()) {
        return QString();
    }

    return QString::fromUtf8(u8"Прогноз до разрядки: %1")
        .arg(parts.join(QString::fromUtf8(u8" · ")));
}

QString BuildRuntimeForecastCompactSummary(const BatteryRuntimeForecast& forecast) {
    if (forecast.pair_remaining_ms.has_value()) {
        return QString::fromUtf8(u8"Осталось: %1")
            .arg(FormatRuntimeDurationCompact(*forecast.pair_remaining_ms));
    }

    const std::array<QString, 4> order = {
        QStringLiteral("left"),
        QStringLiteral("right"),
        QStringLiteral("main"),
        QStringLiteral("case"),
    };
    for (const auto& key : order) {
        const auto found = forecast.by_component.find(key);
        if (found == forecast.by_component.end() || !found->remaining_ms.has_value()) {
            continue;
        }
        return QString::fromUtf8(u8"Осталось: %1")
            .arg(FormatRuntimeDurationCompact(*found->remaining_ms));
    }

    return QString();
}

QString BuildBatteryStatisticsReport(const BatteryHistoryData& history) {
    if (history.samples.isEmpty()) {
        return QString::fromUtf8(u8"Статистика появится после нескольких живых обновлений.");
    }

    QStringList lines;
    const auto& latest_sample = history.samples.back();
    const QDateTime start_time = QDateTime::fromMSecsSinceEpoch(history.samples.front().timestamp_ms);
    const QDateTime end_time = QDateTime::fromMSecsSinceEpoch(history.samples.back().timestamp_ms);

    QSet<QDate> days;
    for (const auto& sample : history.samples) {
        days.insert(QDateTime::fromMSecsSinceEpoch(sample.timestamp_ms).date());
    }

    lines << QString::fromUtf8(u8"Период истории: %1 — %2")
                 .arg(start_time.toString(QStringLiteral("dd MMM yyyy HH:mm")))
                 .arg(end_time.toString(QStringLiteral("dd MMM yyyy HH:mm")));
    lines << QString::fromUtf8(u8"Дней с данными: %1 · Точек: %2")
                 .arg(days.size())
                 .arg(history.samples.size());

    lines << QString();
    lines << QString::fromUtf8(u8"Текущее состояние");
    if (!latest_sample.device_mode.trimmed().isEmpty()) {
        QString mode_line = QString::fromUtf8(u8"Режим: %1").arg(ModeLabel(latest_sample.device_mode));
        if (!latest_sample.device_submode.trimmed().isEmpty()) {
            mode_line += QString::fromUtf8(u8" · %1").arg(SubmodeLabel(latest_sample.device_submode));
        }
        lines << mode_line;
    }

    const std::array<QString, 4> component_order = {
        QStringLiteral("left"),
        QStringLiteral("right"),
        QStringLiteral("case"),
        QStringLiteral("main"),
    };
    for (const auto& component_key : component_order) {
        const auto level = SampleLevel(latest_sample, component_key);
        if (!level.has_value()) {
            continue;
        }
        lines << QStringLiteral("%1: %2%").arg(ComponentLabel(component_key)).arg(*level);
    }

    const BatteryRuntimeForecast forecast = EstimateBatteryRuntimeForecast(history);
    lines << QString();
    lines << QString::fromUtf8(u8"Прогноз до разрядки");
    bool have_forecast = false;
    if (forecast.pair_remaining_ms.has_value()) {
        lines << QString::fromUtf8(u8"Пара: %1 (%2)")
                     .arg(FormatRuntimeDurationCompact(*forecast.pair_remaining_ms))
                     .arg(ConfidenceLabel(forecast.pair_confidence));
        have_forecast = true;
    }
    for (const auto& component_key : component_order) {
        const auto found = forecast.by_component.find(component_key);
        if (found == forecast.by_component.end() || !found->remaining_ms.has_value()) {
            continue;
        }
        lines << QStringLiteral("%1: %2 (%3)")
                     .arg(ComponentLabel(component_key))
                     .arg(FormatRuntimeDurationCompact(*found->remaining_ms))
                     .arg(ConfidenceLabel(found->confidence));
        have_forecast = true;
    }
    if (!have_forecast) {
        lines << QString::fromUtf8(u8"Данных для прогноза пока недостаточно.");
    }

    lines << QString();
    lines << QString::fromUtf8(u8"Среднее время 100→0");
    const auto average_summaries = ComputeAverageRuntimeSummaries(history);
    bool have_averages = false;
    for (const auto& component_key : component_order) {
        const auto found = average_summaries.find(component_key);
        if (found == average_summaries.end() || found->total_duration_ms <= 0 || found->total_drop < 10) {
            continue;
        }

        const qint64 estimated_full_ms = static_cast<qint64>(
            (static_cast<long double>(found->total_duration_ms) * 100.0L) /
            static_cast<long double>(found->total_drop));
        lines << QStringLiteral("%1: %2")
                     .arg(ComponentLabel(component_key))
                     .arg(FormatRuntimeDurationCompact(estimated_full_ms));
        have_averages = true;
    }
    if (!have_averages) {
        lines << QString::fromUtf8(u8"Недостаточно истории.");
    }

    const auto imbalance = ComputeImbalanceSummary(history);
    if (imbalance.shared_samples > 0) {
        lines << QString();
        lines << QString::fromUtf8(u8"Баланс левого и правого");
        lines << QString::fromUtf8(u8"Общих точек: %1").arg(imbalance.shared_samples);
        lines << QString::fromUtf8(u8"Средняя разница (левый - правый): %1%")
                     .arg(QString::number(imbalance.average_diff, 'f', 1));
        lines << QString::fromUtf8(u8"Максимальный разрыв: %1%").arg(imbalance.max_abs_gap);
        if (imbalance.latest_diff.has_value()) {
            lines << QString::fromUtf8(u8"Текущая разница: %1%").arg(*imbalance.latest_diff);
        }
        if (std::abs(imbalance.average_diff) >= 10.0) {
            lines << (imbalance.average_diff > 0.0
                          ? QString::fromUtf8(u8"Вывод: правый наушник в среднем садится быстрее.")
                          : QString::fromUtf8(u8"Вывод: левый наушник в среднем садится быстрее."));
        }
    }

    lines << QString();
    lines << QString::fromUtf8(u8"Статистика по режимам");
    const auto context_summaries = ComputeContextRuntimeSummaries(history);
    bool have_context = false;
    for (const auto& summary : context_summaries) {
        if (summary.total_duration_ms <= 0 || summary.total_drop < 10) {
            continue;
        }

        const qint64 estimated_full_ms = static_cast<qint64>(
            (static_cast<long double>(summary.total_duration_ms) * 100.0L) /
            static_cast<long double>(summary.total_drop));
        QString label = QStringLiteral("%1 · %2").arg(ComponentLabel(summary.component_key), ModeLabel(summary.mode));
        if (!summary.submode.trimmed().isEmpty()) {
            label += QString::fromUtf8(u8" · %1").arg(SubmodeLabel(summary.submode));
        }
        lines << QStringLiteral("%1: %2 · сессий %3")
                     .arg(label)
                     .arg(FormatRuntimeDurationCompact(estimated_full_ms))
                     .arg(summary.sessions);
        have_context = true;
    }
    if (!have_context) {
        lines << QString::fromUtf8(u8"По режимам пока мало данных.");
    }

    return lines.join(QStringLiteral("\n"));
}

}  // namespace battery_monitor
