#include "update/UpdateSecurity.h"

#include <array>
#include <limits>

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <monocypher-ed25519.h>

namespace battery_monitor {
namespace {

const QByteArray kPublicKey = QByteArray::fromBase64("bfwjZfSItKzGrGJ8TXHWilvU8QvJgJZtj+yM/DIatRc=");

bool Fail(QString* error, const QString& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool IsSafeRelativePath(const QString& path) {
    if (path.isEmpty() || path.size() > 240 || QDir::isAbsolutePath(path) ||
        path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':')) ||
        path.contains(QChar::Null)) {
        return false;
    }
    const QStringList parts = path.split(QLatin1Char('/'));
    static const QSet<QString> reserved = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"), QStringLiteral("nul"),
        QStringLiteral("clock$"),
        QStringLiteral("com1"), QStringLiteral("com2"), QStringLiteral("com3"), QStringLiteral("com4"),
        QStringLiteral("com5"), QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"),
        QStringLiteral("lpt4"), QStringLiteral("lpt5"), QStringLiteral("lpt6"), QStringLiteral("lpt7"),
        QStringLiteral("lpt8"), QStringLiteral("lpt9")};
    for (const QString& part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..") ||
            part.contains(QRegularExpression(QStringLiteral(R"([\x00-\x1f<>"|?*])"))) ||
            part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')) ||
            reserved.contains(part.section(QLatin1Char('.'), 0, 0).toLower())) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool VerifyUpdateManifestSignature(const QByteArray& manifest,
                                   const QByteArray& signature_base64,
                                   QString* error) {
    if (manifest.isEmpty() || manifest.size() > kMaximumManifestBytes || kPublicKey.size() != 32) {
        return Fail(error, QStringLiteral("Manifest or trusted public key is invalid."));
    }
    const QByteArray signature = QByteArray::fromBase64(signature_base64.trimmed(), QByteArray::AbortOnBase64DecodingErrors);
    if (signature.size() != 64) {
        return Fail(error, QStringLiteral("Manifest signature encoding is invalid."));
    }
    const int result = crypto_ed25519_check(
        reinterpret_cast<const std::uint8_t*>(signature.constData()),
        reinterpret_cast<const std::uint8_t*>(kPublicKey.constData()),
        reinterpret_cast<const std::uint8_t*>(manifest.constData()),
        static_cast<std::size_t>(manifest.size()));
    if (result != 0) {
        return Fail(error, QStringLiteral("Manifest signature verification failed."));
    }
    return true;
}

bool VerifySha256(const QString& file_path,
                  const QByteArray& expected_hash,
                  std::uint64_t expected_size,
                  QString* error) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly) || static_cast<std::uint64_t>(file.size()) != expected_size) {
        return Fail(error, QStringLiteral("Downloaded package size does not match the signed manifest."));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || hash.result() != expected_hash) {
        return Fail(error, QStringLiteral("Downloaded package SHA-256 does not match the signed manifest."));
    }
    return true;
}

bool ExtractVerifiedUpdateBundle(const QString& bundle_path,
                                 const QString& destination,
                                 QString* error) {
    QFile bundle(bundle_path);
    if (!bundle.open(QIODevice::ReadOnly)) {
        return Fail(error, QStringLiteral("Cannot open the verified update package."));
    }
    if (bundle.read(8) != QByteArrayLiteral("BMUP0001")) {
        return Fail(error, QStringLiteral("Unsupported update package format."));
    }

    QDataStream stream(&bundle);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 file_count = 0;
    stream >> file_count;
    if (stream.status() != QDataStream::Ok || file_count == 0 || file_count > kMaximumBundleFiles) {
        return Fail(error, QStringLiteral("Update package file count is invalid."));
    }

    QDir root(destination);
    if (!root.mkpath(QStringLiteral("."))) {
        return Fail(error, QStringLiteral("Cannot create update staging directory."));
    }
    QSet<QString> seen_paths;
    QSet<QString> seen_files;
    for (quint32 index = 0; index < file_count; ++index) {
        quint16 path_size = 0;
        stream >> path_size;
        const QByteArray path_bytes = bundle.read(path_size);
        quint64 file_size = 0;
        stream >> file_size;
        const QByteArray expected_hash = bundle.read(32);
        const QString relative_path = QString::fromUtf8(path_bytes);
        const QString folded = relative_path.toCaseFolded();
        if (stream.status() != QDataStream::Ok || path_bytes.size() != path_size ||
            expected_hash.size() != 32 || file_size > kMaximumPackageBytes ||
            relative_path.toUtf8() != path_bytes || !IsSafeRelativePath(relative_path) ||
            seen_paths.contains(folded)) {
            return Fail(error, QStringLiteral("Update package contains an unsafe file entry."));
        }
        QString parent = relative_path.section(QLatin1Char('/'), 0, -2).toCaseFolded();
        while (!parent.isEmpty()) {
            if (seen_files.contains(parent)) {
                return Fail(error, QStringLiteral("Update package contains a file/directory collision."));
            }
            parent = parent.section(QLatin1Char('/'), 0, -2);
        }
        for (const QString& existing : seen_files) {
            if (existing.startsWith(folded + QLatin1Char('/'))) {
                return Fail(error, QStringLiteral("Update package contains a file/directory collision."));
            }
        }
        seen_paths.insert(folded);
        seen_files.insert(folded);

        const QString output_path = root.filePath(relative_path);
        if (!QDir().mkpath(QFileInfo(output_path).absolutePath())) {
            return Fail(error, QStringLiteral("Cannot create update package directories."));
        }
        QSaveFile output(output_path);
        if (!output.open(QIODevice::WriteOnly)) {
            return Fail(error, QStringLiteral("Cannot create a staged update file."));
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        quint64 remaining = file_size;
        while (remaining > 0) {
            const qint64 chunk_size = static_cast<qint64>(std::min<quint64>(remaining, 1024 * 1024));
            const QByteArray chunk = bundle.read(chunk_size);
            if (chunk.size() != chunk_size || output.write(chunk) != chunk.size()) {
                return Fail(error, QStringLiteral("Update package is truncated."));
            }
            hash.addData(chunk);
            remaining -= static_cast<quint64>(chunk.size());
        }
        if (hash.result() != expected_hash || !output.commit()) {
            return Fail(error, QStringLiteral("A staged update file failed integrity verification."));
        }
    }
    if (!bundle.atEnd()) {
        return Fail(error, QStringLiteral("Update package contains undeclared trailing data."));
    }
    return true;
}

}  // namespace battery_monitor
