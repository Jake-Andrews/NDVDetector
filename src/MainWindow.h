//  MainWindow.h  ── streamlined Codec‑Tests implementation (Qt 6)
#pragma once

#include <QMainWindow>
#include <QAbstractTableModel>
#include <QFileInfo>
#include <QThread>
#include <QVector>
#include <memory>
#include <vector>

#include "SearchSettings.h"
#include "VideoInfo.h"

//  Data structure for a single video sample
struct TestItem {
    QString path;
    QString codec;
    QString pixFmt;
    QString profile;
    QString level;
    bool hwOk = false;
    bool swOk = false;
};

//  Table‑model for the Custom‑Tests list 
class CustomTestModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Col { File, Codec, PixFmt, Profile, Level, HW, SW, ColCount };

    explicit CustomTestModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    // --- QAbstractItemModel --------------------------------------------------
    int rowCount (const QModelIndex& = {}) const override { return m_rows.size(); }
    int columnCount(const QModelIndex& = {}) const override { return ColCount; }
    QVariant data      (const QModelIndex& ix, int role) const override;
    QVariant headerData(int section, Qt::Orientation, int role) const override;

    // API ---------------------------------------------------------------------
    void             append(TestItem const& it);
    [[nodiscard]] int size() const { return m_rows.size(); }
    TestItem const&  at(int r) const { return m_rows[r]; }
    void updateResult(QString const& path, bool hw, bool sw);

private:
    QVector<TestItem> m_rows;
};

//  Table‑model for the Hardware‑Filter list 
class HardwareFilterModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Col { Include, File, Codec, PixFmt, Profile, Level, ColCount };

    struct Row { TestItem item; bool include = true; bool hwOk = false; bool swOk = false; };

    explicit HardwareFilterModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    // --- QAbstractItemModel --------------------------------------------------
    int rowCount (const QModelIndex& = {}) const override { return m_rows.size(); }
    int columnCount(const QModelIndex& = {}) const override { return ColCount; }
    QVariant data      (const QModelIndex& ix, int role) const override;
    bool     setData   (const QModelIndex& ix, const QVariant& v, int role) override;
    Qt::ItemFlags flags(const QModelIndex& ix) const override;
    QVariant headerData(int section, Qt::Orientation, int role) const override;

    // API ---------------------------------------------------------------------
    void            append(TestItem const& it);
    void            resetResults();
    void            updateResult(QString const& path, bool hw, bool sw);
    QVector<TestItem> includedTests() const;

private:
    QVector<Row> m_rows;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Worker that decodes samples on a background thread
// ─────────────────────────────────────────────────────────────────────────────
class CodecTestWorker : public QObject {
    Q_OBJECT
public:
    explicit CodecTestWorker(QObject* parent = nullptr);
    void setTests(QVector<TestItem> t) { m_tests = std::move(t); }

signals:
    void progress(int done, int total);           // emitted each file
    void result  (TestItem, bool hwOk, bool swOk);
    void finished();

public slots:
    void run();                                   // entry‑point from thread

private:
    bool tryDecode(const TestItem& item, bool hw);
    QVector<TestItem> m_tests;
};

// ─────────────────────────────────────────────────────────────────────────────
//  MainWindow declaration
// ─────────────────────────────────────────────────────────────────────────────
QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class VideoModel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // duplicate‑detection helper
    void setDuplicateVideoGroups(const std::vector<std::vector<VideoInfo>>& groups);
    VideoModel* model() const { return m_model.get(); }

    // Enum types used by the rest of the app (unchanged)
    enum DeleteOptions { List, ListDB, Disk };
    Q_ENUM(DeleteOptions)
    enum SelectOptions { AllExceptLargest, AllExceptSmallest, Smallest, Largest };
    Q_ENUM(SelectOptions)
    enum SortOptions { Size, CreatedAt };
    Q_ENUM(SortOptions)

signals:
    // (core app signals – unchanged)
    void searchRequested(SearchSettings settings);
    void selectOptionChosen(MainWindow::SelectOptions);
    void sortOptionChosen(MainWindow::SortOptions);
    void sortGroupsOptionChosen(MainWindow::SortOptions);
    void deleteOptionChosen(MainWindow::DeleteOptions);
    void hardlinkTriggered();

    // DB / directory panel signals
    void addDirectoryRequested(QString const& dirPath);
    void removeSelectedDirectoriesRequested(QStringList const& toRemove);
    void databaseLoadRequested(QString const& file);
    void databaseCreateRequested(QString const& file);

public slots:
    void onDirectoryListUpdated(QStringList const& dirs);
    void onDuplicateGroupsUpdated(const std::vector<std::vector<VideoInfo>>& groups);
    void setCurrentDatabase(QString const& path);

private slots:
    // core UI actions (trimmed for brevity)
    void onSearchClicked();
    void onSelectClicked();
    void onSortClicked();
    void onSortGroupsClicked();
    void onDeleteClicked();
    void onHardlinkClicked();
    void onRowActivated(const QModelIndex& ix);

    // directory panel actions
    void onAddDirectoryButtonClicked();
    void onPickDirectoryButtonClicked();
    void onRemoveDirectoryButtonClicked();
    void onLoadDbClicked();
    void onNewDbClicked();
    void onValidatePatternsClicked();

    // Codec‑tests actions
    void onAddVideoClicked();            // “Add Video…”
    void onCopyToFilterClicked();        // “Copy to Hardware Filter”
    void onRunTestsClicked();            // Run Test

private:
    bool              eventFilter(QObject*, QEvent*) override;
    SearchSettings    collectSearchSettings() const;

    QVector<TestItem> buildTestList() const;   // from HardwareFilterModel
    static TestItem   probe(QString const& file);

    // UI + data members -------------------------------------------------------
    Ui::MainWindow*                   ui = nullptr;
    std::unique_ptr<VideoModel>       m_model;

    CustomTestModel*                  m_customModel   = nullptr;
    HardwareFilterModel*              m_hwFilterModel = nullptr;

    CodecTestWorker*                  m_codecWorker   = nullptr;
    QThread                           m_codecThread;
};

// -----------------------------------------------------------------------------
//  Inline implementations for small Custom / HW‑filter model methods
// -----------------------------------------------------------------------------
inline QVariant CustomTestModel::data(const QModelIndex& ix, int role) const {
    if (!ix.isValid() || role != Qt::DisplayRole)
        return {};
    TestItem const& r = m_rows[ix.row()];
    switch (ix.column()) {
    case File:    return QFileInfo(r.path).fileName();
    case Codec:   return r.codec;
    case PixFmt:  return r.pixFmt;
    case Profile: return r.profile;
    case Level:   return r.level;
    case HW:      return r.hwOk ? "✅" : "❌";
    case SW:      return r.swOk ? "✅" : "❌";
    default:      return {};
    }
}
inline QVariant CustomTestModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    static const char* names[] = { "File", "Codec", "PixFmt", "Profile", "Level", "HW", "SW" };
    return names[section];
}
inline void CustomTestModel::append(TestItem const& it) {
    beginInsertRows({}, m_rows.size(), m_rows.size());
    m_rows.push_back(it);
    endInsertRows();
}

inline void CustomTestModel::updateResult(QString const& path, bool hw, bool sw) {
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].path == path) {
            m_rows[i].hwOk = hw;
            m_rows[i].swOk = sw;
            QModelIndex topLeft = index(i, HW);
            QModelIndex bottomRight = index(i, SW);
            emit dataChanged(topLeft, bottomRight, {Qt::DisplayRole});
            break;
        }
    }
}

inline QVariant HardwareFilterModel::data(const QModelIndex& ix, int role) const {
    if (!ix.isValid()) return {};
    Row const& r = m_rows[ix.row()];
    if (ix.column() == Include) {
        if (role == Qt::CheckStateRole)
            return r.include ? Qt::Checked : Qt::Unchecked;
        return {};
    }

    enum Col { Include, File, Codec, PixFmt, Profile, Level, ColCount };

    struct Row { TestItem item; bool include = true; bool hwOk = false; bool swOk = false; };

    if (role != Qt::DisplayRole) return {};
    switch (ix.column()) {
    case File:    return QFileInfo(r.item.path).fileName();
    case Codec:   return r.item.codec;
    case PixFmt:  return r.item.pixFmt;
    case Profile: return r.item.profile;
    case Level:   return r.item.level;
    default:      return {};
    }
}
inline bool HardwareFilterModel::setData(const QModelIndex& ix, const QVariant& v, int role) {
    if (!ix.isValid() || ix.column() != Include || role != Qt::CheckStateRole)
        return false;
    m_rows[ix.row()].include = (v.toInt() == Qt::Checked);
    emit dataChanged(ix, ix, { Qt::CheckStateRole });
    return true;
}
inline Qt::ItemFlags HardwareFilterModel::flags(const QModelIndex& ix) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(ix);
    if (ix.column() == Include)
        f |= Qt::ItemIsUserCheckable;
    return f;
}
inline QVariant HardwareFilterModel::headerData(int s, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char* names[] = { "Include", "File", "Codec", "PixFmt", "Profile", "Level", "HW", "SW"};
    return names[s];
}
inline void HardwareFilterModel::append(TestItem const& it) {
    if (std::any_of(m_rows.begin(), m_rows.end(), [&](Row const& r){ return r.item.path == it.path; }))
        return;
    beginInsertRows({}, m_rows.size(), m_rows.size());
    m_rows.push_back({ it });
    endInsertRows();
}
inline void HardwareFilterModel::resetResults() {
}

inline void HardwareFilterModel::updateResult(QString const& path, bool hw, bool sw) {
}
inline QVector<TestItem> HardwareFilterModel::includedTests() const {
    QVector<TestItem> v; v.reserve(m_rows.size());
    for (auto const& r : m_rows) if (r.include) v.push_back(r.item);
    return v;
}

