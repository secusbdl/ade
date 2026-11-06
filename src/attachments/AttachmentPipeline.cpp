#include "AttachmentPipeline.h"
#include "FileTransformer.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <utility>

namespace ade {
namespace {
QJsonObject textPart(const QString &text) { return {{"type", "text"}, {"text", text}}; }
QString cdata(QString text) { return "<![CDATA[" + text.replace("]]>", "]]]]><![CDATA[>") + "]]>"; }
QString fileText(const AttachmentRef &reference, const QString &text) {
    return "<file path=\"" + reference.path.toHtmlEscaped() + "\" original-name=\"" + reference.originalFileName.toHtmlEscaped()
        + "\"><content>" + cdata(text) + "</content></file>";
}
QString hash(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
bool validLimits(const ModelMediaCapabilities &capabilities, const AttachmentLimits &limits) {
    return limits.maxFileBytes > 0 && limits.maxFileBytes <= 100 * 1024 * 1024 && limits.maxTextChars > 0 && limits.maxTextChars <= 1024 * 1024
        && limits.previewLines > 0 && limits.previewLines <= 1000 && limits.maxAttachments > 0 && limits.maxAttachments <= 128
        && limits.maxReadChars > 0 && limits.maxReadChars <= 4 * 1024 * 1024
        && limits.maxRequestImageBytes > 0 && limits.maxRequestImageBytes <= 64 * 1024 * 1024
        && capabilities.maxImageBytes > 0 && capabilities.maxImageBytes <= 32 * 1024 * 1024 && capabilities.maxWidth > 0
        && capabilities.maxWidth <= 16384 && capabilities.maxHeight > 0 && capabilities.maxHeight <= 16384
        && capabilities.maxPixels > 0 && capabilities.maxPixels <= 64 * 1024 * 1024;
}
}
AttachmentPipeline::AttachmentPipeline(AttachmentStore store) : m_store(std::move(store)), m_originalCache(128 * 1024 * 1024) {}
QJsonObject AttachmentPipeline::capture(const QString &agentId, const QJsonArray &references, const AttachmentMounts &mounts, QString *error) {
    if (error) error->clear();
    if (references.size() > 32) { if (error) *error = "At most 32 attachments can be submitted at once."; return {}; }
    QJsonArray accepted; QJsonObject paths, pathMetadata;
    for (const auto &value : references) {
        QString failure;
        if (!value.isObject()) { if (error) *error = "Attachment references must be objects."; return {}; }
        const auto reference = AttachmentRef::fromJson(value.toObject(), &failure);
        if (!failure.isEmpty()) { if (error) *error = failure; return {}; }
        QDateTime modified;
        const auto bytes = m_store.read(agentId, reference, mounts, &failure, 64 * 1024 * 1024, &modified);
        if (!failure.isEmpty()) { if (error) *error = failure; return {}; }
        if (!m_store.saveSnapshot(agentId, reference, bytes, &failure)) { if (error) *error = failure; return {}; }
        const auto digest = hash(bytes); paths[reference.path] = digest; accepted.append(reference.toJson());
        QJsonObject captured{{"size", bytes.size()}};
        if (modified.isValid()) captured["modified"] = modified.toUTC().toString(Qt::ISODateWithMs);
        pathMetadata[reference.path] = captured;
        m_originalCache.insert(agentId + '/' + reference.path + '/' + digest, new QByteArray(bytes), qMax(1, int(bytes.size())));
    }
    return {{"attachments", accepted}, {"pathReferences", paths}, {"pathMetadata", pathMetadata}};
}
QJsonArray AttachmentPipeline::transform(const QString &agentId, const AttachmentRef &reference, const QString &expectedHash,
    const QJsonObject &capturedMetadata,
    const AttachmentMounts &mounts, const ModelMediaCapabilities &capabilities, const AttachmentLimits &limits, QString *error,const FileReadParams* explicitParams) {
    error->clear(); QByteArray bytes;
    if (!expectedHash.isEmpty() && !QRegularExpression("^[0-9a-f]{64}$").match(expectedHash).hasMatch()) {
        *error = "Invalid attachment content hash."; return {};
    }
    // Previously captured (agent, path, hash) snapshots survive source deletion and app restart.
    // A hash alone never grants access to another chat or uncaptured path.
    if (!expectedHash.isEmpty()) {
        if (const auto cached = m_originalCache.object(agentId + '/' + reference.path + '/' + expectedHash)) bytes = *cached;
        else { QString ignored; bytes = m_store.readSnapshot(agentId, reference, expectedHash, &ignored); }
    }
    if (bytes.isNull()) {
        bytes = m_store.read(agentId, reference, mounts, error, limits.maxFileBytes);
        if (!error->isEmpty()) return {};
        const auto digest = hash(bytes);
        if (!expectedHash.isEmpty() && digest != expectedHash) { *error = "The referenced file changed and its original cached bytes are unavailable."; return {}; }
        m_originalCache.insert(agentId + '/' + reference.path + '/' + digest, new QByteArray(bytes), qMax(1, int(bytes.size())));
    }
    if (bytes.size() > limits.maxFileBytes) { *error = "Attachment exceeds the current file-read bound."; return {}; }
    FileDescriptor descriptor{reference.path, reference.originalFileName, bytes.size(), {}};
    if (!capturedMetadata.isEmpty()) {
        const auto size = capturedMetadata["size"];
        if (!size.isDouble() || size.toDouble(-1) != double(bytes.size())) { *error = "Captured attachment size does not match its original bytes."; return {}; }
        if (capturedMetadata.contains("modified")) {
            const auto modified = capturedMetadata["modified"];
            if (modified.isString() && modified.toString().size() <= 40) descriptor.modified = QDateTime::fromString(modified.toString(), Qt::ISODateWithMs);
            if (!descriptor.modified.isValid() || descriptor.modified.toUTC().toString(Qt::ISODateWithMs) != modified.toString()) {
                *error = "Captured attachment modification time is invalid."; return {};
            }
        }
    }
    const auto name = reference.originalFileName.isEmpty() ? reference.path : reference.originalFileName;
    const auto extension = QFileInfo(name).suffix().toLower();
    const QStringList raster{"png", "jpg", "jpeg", "gif", "webp", "bmp", "avif", "ico"};
    QBuffer input(&bytes); input.open(QIODevice::ReadOnly); QImageReader reader(&input); reader.setAutoTransform(true);
    if (raster.contains(extension) && reader.canRead()) {
        if (!capabilities.image) return {textPart(fileText(reference, "[Image content not available to this model]"))};
        const auto original = reader.size();
        if (!original.isValid() || qint64(original.width()) * original.height() > 64 * 1024 * 1024) { *error = "Image dimensions exceed the native decoding bound."; return {}; }
        auto image = reader.read();
        if (image.isNull()) { *error = "The attachment image could not be decoded."; return {}; }
        const auto scale = qMin(1.0, qMin(qMin(double(capabilities.maxWidth) / image.width(), double(capabilities.maxHeight) / image.height()),
            std::sqrt(double(capabilities.maxPixels) / (qint64(image.width()) * image.height()))));
        if (scale < 1.0) image = image.scaled(qMax(1, int(image.width() * scale)), qMax(1, int(image.height() * scale)), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray format; QString mime;
        for (const auto &candidate : QStringList{"image/webp", "image/jpeg", "image/png"}) {
            const auto codec = candidate.mid(6).toLatin1();
            if (capabilities.imageMimeTypes.contains(candidate) && QImageWriter::supportedImageFormats().contains(codec)) { mime = candidate; format = codec; break; }
        }
        if (format.isEmpty()) { *error = "No installed image encoder matches this model's accepted formats."; return {}; }
        QByteArray encoded;
        for (const auto quality : {80, 65, 50}) {
            encoded.clear(); QBuffer output(&encoded); output.open(QIODevice::WriteOnly); QImageWriter writer(&output, format); writer.setQuality(quality);
            if (!writer.write(image)) { *error = "The attachment image could not be encoded."; return {}; }
            if (encoded.size() <= capabilities.maxImageBytes) break;
        }
        if (encoded.size() > capabilities.maxImageBytes) { *error = "The image cannot fit this model's byte limit at the supported quality levels."; return {}; }
        return {textPart("<file path=\"" + reference.path.toHtmlEscaped() + "\"><metadata>" + QString::number(image.width()) + "x" + QString::number(image.height()) + " " + mime + "</metadata>"),
            QJsonObject{{"type", "image"}, {"mediaType", mime}, {"dataBase64", QString::fromLatin1(encoded.toBase64())}}, textPart("</file>")};
    }
    FileReadParams params;if(explicitParams)params=*explicitParams;
    else if (!reference.path.startsWith("att/")) params.preview = true;
    FileTransformLimits transformLimits; transformLimits.maxFileBytes = limits.maxFileBytes;
    transformLimits.maxReadChars = limits.maxReadChars; transformLimits.maxPreviewLines = limits.previewLines;
    transformLimits.maxOutputChars = limits.maxTextChars;
    const auto converted = FileTransformer::fromBytes(bytes, descriptor, params, transformLimits);
    if (!converted.error.isEmpty()) { *error = converted.error; return {}; }
    return FileTransformer::wrappedParts(descriptor, converted);
}
QJsonArray AttachmentPipeline::prepare(const QJsonArray &history, const QString &agentId, const AttachmentMounts &mounts,
    const ModelMediaCapabilities &capabilities, const AttachmentLimits &limits, QString *error) {
    if (error) error->clear();
    if (!validLimits(capabilities, limits)) { if (error) *error = "Invalid native attachment limits or model image constraints."; return {}; }
    QJsonArray result, pendingMedia; QSet<QString> seen; qint64 imageBytes = 0; int count = 0;
    QMap<QString,QJsonObject> pendingCalls;
    auto flush = [&] { if (!pendingMedia.isEmpty()) { result.append(QJsonObject{{"role", "user"}, {"content", pendingMedia}}); pendingMedia = {}; } };
    for (int i = 0; i < history.size(); ++i) {
        auto message = history[i].toObject(); const auto role = message["role"].toString();
        if (role != "tool" && !pendingMedia.isEmpty()) flush();
        if(role!="tool"){
            pendingCalls.clear();
            if(role=="assistant")for(const auto& call:message["tool_calls"].toArray())pendingCalls.insert(call.toObject()["id"].toString(),call.toObject()["function"].toObject());
        }
        const auto metadata = message.value("metadata").toObject(); const auto references = metadata["attachments"].toArray(); const auto hashes = metadata["pathReferences"].toObject();
        const auto pathMetadataValue = metadata["pathMetadata"];
        const auto pathMetadata = pathMetadataValue.toObject();
        QJsonArray parts;
        if(metadata.contains("fileReads")){
            QString failure;FileReadCapture captured;
            const auto function=pendingCalls.take(message["tool_call_id"].toString());
            QJsonParseError parse;const auto args=QJsonDocument::fromJson(function["arguments"].toString().toUtf8(),&parse);
            if(role!="tool"||function.isEmpty()||parse.error!=QJsonParseError::NoError||!args.isObject()||message["content"].toString().startsWith("Error:"))failure="Read reference has no matching successful tool result.";
            else if(!m_readLease.belongsTo(agentId))failure="Read snapshot lease belongs to a different chat or was revoked.";
            else if(!FileReadSnapshotStore::read(m_readLease,metadata,&captured,&failure)){}
            else if(captured.tool!=function["name"].toString()||captured.inputHash!=fileReadInputHash(captured.tool,args.object()))failure="Read reference does not match its original tool input.";
            if(failure.isEmpty()){
                FileReadParams expected;const bool valid=captured.tool=="ls"?FileTransformer::listParams(args.object(),&expected,&failure):FileTransformer::readParams(args.object(),&expected,&failure);
                if(!valid||expected!=captured.params)failure="Read range does not match its original tool input.";
            }
            QJsonArray converted;
            if(failure.isEmpty()){
                if(++count>limits.maxAttachments){if(error)*error="File context exceeds its per-request count limit.";return {};}
                if(captured.directory){
                    FileTransformLimits bounds;bounds.maxOutputChars=limits.maxTextChars;
                    const auto value=FileTransformer::fromDirectory(captured.entries,captured.descriptor,captured.params,bounds);
                    if(value.error.isEmpty())converted=FileTransformer::wrappedParts(captured.descriptor,value);else failure=value.error;
                }else{
                    const auto digest=hash(captured.bytes);
                    m_originalCache.insert(agentId+'/'+captured.descriptor.mountedPath+'/'+digest,new QByteArray(captured.bytes),qMax(1,int(captured.bytes.size())));
                    QJsonObject details{{"size",captured.bytes.size()}};
                    if(captured.descriptor.modified.isValid())details["modified"]=captured.descriptor.modified.toUTC().toString(Qt::ISODateWithMs);
                    converted=transform(agentId,{captured.descriptor.mountedPath,captured.descriptor.originalFileName},digest,details,mounts,capabilities,limits,&failure,&captured.params);
                }
            }
            if(!failure.isEmpty())converted={textPart("<file><metadata>error:true</metadata><content>"+cdata("Original read could not be included: "+failure)+"</content></file>")};
            else seen.insert(captured.descriptor.mountedPath+'/'+hashes[captured.descriptor.mountedPath].toString());
            for(const auto& part:converted){
                if(part.toObject()["type"]=="image")imageBytes+=part.toObject()["dataBase64"].toString().size()*3/4;
                if(imageBytes>limits.maxRequestImageBytes){if(error)*error="File images exceed the per-request byte budget.";return {};}
                parts.append(part);
            }
        }else if(role=="tool")pendingCalls.remove(message["tool_call_id"].toString());
        for (const auto &value : references) {
            QString failure; const auto reference = AttachmentRef::fromJson(value.toObject(), &failure);
            const auto expectedHash = hashes[reference.path].toString(); const auto key = reference.path + '/' + expectedHash;
            if (seen.contains(key)) continue;
            seen.insert(key);
            if (failure.isEmpty() && ++count > limits.maxAttachments) { if (error) *error = "Attachment context exceeds its per-request count limit."; return {}; }
            if (failure.isEmpty() && !pathMetadataValue.isUndefined() && (!pathMetadataValue.isObject() || pathMetadata.size() > 128)) failure = "Invalid captured attachment metadata map.";
            const auto captured = pathMetadata[reference.path];
            if (failure.isEmpty() && !captured.isUndefined() && (!captured.isObject() || captured.toObject().isEmpty())) failure = "Invalid captured attachment metadata.";
            auto converted = failure.isEmpty() ? transform(agentId, reference, expectedHash, captured.toObject(), mounts, capabilities, limits, &failure) : QJsonArray{};
            if (!failure.isEmpty()) converted = {textPart(fileText(reference, "Attachment could not be included: " + failure))};
            for (const auto &part : converted) {
                if (part.toObject()["type"] == "image") imageBytes += part.toObject()["dataBase64"].toString().size() * 3 / 4;
                if (imageBytes > limits.maxRequestImageBytes) { if (error) *error = "Attachment images exceed the per-request byte budget."; return {}; }
                parts.append(part);
            }
        }
        if (role == "user" && !parts.isEmpty()) {
            const auto content = message["content"];
            if (content.isString()) parts.append(textPart(content.toString()));
            else for (const auto &part : content.toArray()) parts.append(part);
            message["content"] = parts;
        } else for (const auto &part : parts) pendingMedia.append(part);
        result.append(message);
    }
    flush(); return result;
}
}
