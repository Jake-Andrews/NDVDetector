// CustomTestModel.cpp
#include "CustomTestModel.h"
#include "FFProbeExtractor.h" // probe_video_codec_info
#include <QDir>
#include <qfileinfo.h>
#include <tuple>

using namespace Qt::StringLiterals;

static QString yesNo(bool ok) { return ok ? u"✅"_s : u"❌"_s; }

QVariant CustomTestModel::data(QModelIndex const& ix, int role) const
{
    if (!ix.isValid())
        return {};

    auto const& r = m_rows.at(ix.row());

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (ix.column()) {
        case File:
            return QFileInfo(r.path).fileName();
        case Codec:
            return r.codec;
        case PixFmt:
            return r.pixFmt;
        case Profile:
            return r.profile;
        case Level:
            return r.level;
        case HW:
            return yesNo(r.hwOk);
        case SW:
            return yesNo(r.swOk);
        }
    }

    return {};
}

QVariant CustomTestModel::headerData(int s, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    static QStringList names { u"File"_s, u"Codec"_s, u"PixFmt"_s, u"Profile"_s,
        u"Level"_s, u"HW"_s, u"SW"_s };
    return names.value(s);
}

void CustomTestModel::append(TestItem&& t)
{
    beginInsertRows({}, m_rows.size(), m_rows.size());
    m_rows.push_back(std::move(t));
    endInsertRows();
}

// ──────────────────────────────────────────────────────────────────────
//  Scan ~/Documents/NDVDetector/test_videos/ and append one TestItem per file
// ──────────────────────────────────────────────────────────────────────
void CustomTestModel::loadInitial()
{
    QString dirPath = QDir::homePath() + "/Documents/NDVDetector/test_videos/";
    QDir dir(dirPath);
    QStringList files = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);

    for (QString const& f : files) {
        QString full = dir.filePath(f);

        // Default placeholders
        QString codec = "?", pixFmt = "?", profile = "?", level = "?";

        if (auto info = probe_video_codec_info(full); info) {
            QString c, pf, pr;
            int lvl;
            std::tie(c, pf, pr, lvl) = *info;
            codec = c;
            pixFmt = pf;
            profile = pr;
            level = QString::number(lvl);
        }

        append(TestItem { full, codec, pixFmt, profile, level });
    }
}
int CustomTestModel::rowForPath(QString const& p) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].path == p)
            return i;
    return -1;
}
void CustomTestModel::updateResult(QString const& p, bool hw, bool sw)
{
    int row = rowForPath(p);
    if (row < 0)
        return;
    m_rows[row].hwOk = hw;
    m_rows[row].swOk = sw;
    emit dataChanged(index(row, HW), index(row, SW));
}

Qt::ItemFlags CustomTestModel::flags(QModelIndex const& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    switch (index.column()) {
    case Codec:
    case PixFmt:
    case Profile:
    case Level:
        return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable;

    case File:
    case HW:
    case SW:
    default:
        return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    }
}

bool CustomTestModel::setData(QModelIndex const& index, QVariant const& value, int role)
{
    if (!index.isValid() || role != Qt::EditRole)
        return false;

    auto& item = m_rows[index.row()];

    switch (index.column()) {
    case Codec:
        item.codec = value.toString();
        break;
    case PixFmt:
        item.pixFmt = value.toString();
        break;
    case Profile:
        item.profile = value.toString();
        break;
    case Level:
        item.level = value.toString();
        break;
    case File:
    case HW:
    case SW:
    default:
        return false;
    }

    emit dataChanged(index, index);
    return true;
}
