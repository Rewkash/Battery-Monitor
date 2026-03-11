#include "ui/BatteryRuntimeEstimator.h"

#include <algorithm>
#include <array>

#include <QMap>
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
    return value.trimmed().toLower();
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

}  // namespace battery_monitor
