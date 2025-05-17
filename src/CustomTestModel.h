#pragma once

#include "CodecTestWorker.h"   // TestItem

#include <QAbstractTableModel>

class DatabaseManager;

class CustomTestModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Col { File, Codec, PixFmt, Profile, Level, HW, SW, ColCount };

    explicit CustomTestModel(DatabaseManager* db, QObject* parent = nullptr);

    // --- mandatory Qt overrides -------------------------------------------
    int           rowCount (const QModelIndex& = {}) const override { return m_rows.size(); }
    int           columnCount(const QModelIndex& = {}) const override { return ColCount; }
    QVariant      data(const QModelIndex& ix, int role)  const override;
    QVariant      headerData(int section, Qt::Orientation o, int role) const override;

    // --- API used by MainWindow -------------------------------------------
    void          append(TestItem&&);
    void          updateResult(QString const& path, bool hwOk, bool swOk);
    QVector<TestItem> const& items() const { return m_rows; }

    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

private:
    void loadInitial();              // NEW
    int rowForPath(QString const&) const;
    QVector<TestItem> m_rows;
    DatabaseManager* m_db = nullptr;
};

