#pragma once

#include <cstdint>
#include <QByteArray>
#include <QString>

namespace battery_monitor {

inline constexpr int kMaximumManifestBytes = 64 * 1024;
inline constexpr std::uint64_t kMaximumPackageBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr int kMaximumBundleFiles = 4096;

[[nodiscard]] bool VerifyUpdateManifestSignature(const QByteArray& manifest,
                                                 const QByteArray& signature_base64,
                                                 QString* error);
[[nodiscard]] bool VerifySha256(const QString& file_path,
                                const QByteArray& expected_hash,
                                std::uint64_t expected_size,
                                QString* error);
[[nodiscard]] bool ExtractVerifiedUpdateBundle(const QString& bundle_path,
                                               const QString& destination,
                                               QString* error);

}  // namespace battery_monitor
