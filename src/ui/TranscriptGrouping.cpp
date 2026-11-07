#include "TranscriptGrouping.h"
#include "harness/AcpToolPresentation.h"
#include <QSet>

namespace ade {
QString TranscriptPart::key() const {
    // Length framing preserves identity when row/part IDs contain delimiters.
    return QString::number(messageId.size())+":"+messageId+value["id"].toString();
}
QList<TranscriptGroup> groupTranscriptRows(const QJsonArray& rows) {
    QList<TranscriptGroup> groups;
    QSet<QString> seen;
    for(const auto& entry:rows){
        const auto row=entry.toObject();
        const auto role=row["role"].toString();
        const auto id=row["id"].toString();
        if((role!="user"&&role!="assistant")||id.isEmpty()||seen.contains(id))continue;
        seen.insert(id);
        if(groups.isEmpty()||role!="assistant"||groups.last().first["role"]!="assistant")
            groups.append({row,{},{},{}});
        auto& group=groups.last();
        group.messageIds.append(id);
        for(const auto& value:row["parts"].toArray()){const auto part=value.toObject();if(!AcpToolPresentation::suppressQuestionWrapperPart(part))group.parts.append({id,part});}
        // Native attachment chips are outside parts. Preserve every round's
        // existing visible refs without merging its authority/other metadata.
        for(const auto& attachment:row["attachments"].toArray())group.attachments.append(attachment);
    }
    return groups;
}
}
