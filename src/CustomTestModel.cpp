// CustomTestModel.cpp
#include "CustomTestModel.h"
#include "DatabaseManager.h"
#include <QDir>
#include <QVector>
#include <qfileinfo.h>

using namespace Qt::StringLiterals;

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
        }
    }

    if (role == Qt::CheckStateRole) {
        if (ix.column() == HW)
            return r.hwOk ? Qt::Checked : Qt::Unchecked;
        if (ix.column() == SW)
            return r.swOk ? Qt::Checked : Qt::Unchecked;
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

CustomTestModel::CustomTestModel(DatabaseManager* db, QObject* parent)
    : QAbstractTableModel(parent)
    , m_db(db)
{
    loadInitial();
}

void CustomTestModel::append(TestItem&& t)
{
    beginInsertRows({}, m_rows.size(), m_rows.size());
    m_rows.push_back(std::move(t));
    endInsertRows();
    if (m_db)
        m_db->upsertHardwareFilter(m_rows.back());
}

//  Scan ~/Documents/NDVDetector/test_videos/ and append one TestItem per file
void CustomTestModel::loadInitial()
{
    if (!m_db)
        return;
    auto vec = m_db->loadHardwareFilters();
    beginResetModel();
    m_rows = QVector<TestItem>(vec.begin(), vec.end());
    endResetModel();
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
    if (m_db)
        m_db->updateHardwareFilterResult(p, hw, sw);
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

    case HW:
        return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;

    case SW:
        return Qt::ItemIsSelectable | Qt::ItemIsEnabled;

    case File:
    default:
        return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    }
}

bool CustomTestModel::setData(QModelIndex const& index, QVariant const& value, int role)
{
    if (!index.isValid())
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
    case HW:
        if (role == Qt::CheckStateRole) {
            item.hwOk = (value.toInt() == Qt::Checked);
        } else {
            return false;
        }
        break;
    case SW:
        return false; // Read-only
    case File:
    default:
        return false;
    }

    if (m_db)
        m_db->upsertHardwareFilter(item);

    emit dataChanged(index, index);
    return true;
}
