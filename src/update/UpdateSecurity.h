#pragma once

#include <cstdint>
#include <QByteArray>
#include <QString>

namespace battery_monitor {

inline constexpr int kMaximumManifestBytes = 64 * 1024;
inline constexpr std::uint64_t kMaximumPackageBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr int kMaximumBundleFiles = 4096;

// Network transfer caps: responses larger than these are aborted while data is
// still arriving, before the payload can be buffered or written to disk.
inline constexpr qint64 kMaximumManifestDownloadBytes = 1024 * 1024;
inline constexpr qint64 kMaximumSignatureDownloadBytes = 64 * 1024;
inline constexpr std::uint64_t kMaximumPackageDownloadBytes = 200ULL * 1024ULL * 1024ULL;

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
