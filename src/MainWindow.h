#pragma once

#include <QMainWindow>
#include <QThread>
#include <QVector>
#include <memory>
#include <vector>

#include "DatabaseManager.h"
#include "SearchSettings.h"
#include "VideoInfo.h"
#include "ConfigManager.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class VideoModel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(DatabaseManager* db, QWidget* parent = nullptr);
    ~MainWindow() override;

    void setDuplicateVideoGroups(const std::vector<std::vector<VideoInfo>>& groups);
    VideoModel* model() const { return m_model.get(); }

    enum DeleteOptions { List, ListDB, Disk };
    Q_ENUM(DeleteOptions)
    enum SelectOptions { AllExceptLargest, AllExceptSmallest, Smallest, Largest };
    Q_ENUM(SelectOptions)
    enum SortOptions { Size };
    Q_ENUM(SortOptions)

signals:                                       // emitted by MainWindow
    void searchRequested(SearchSettings cfg);
    void selectOptionChosen(SelectOptions option);
    void sortOptionChosen(SortOptions option);
    void sortGroupsOptionChosen(SortOptions option);
    void deleteOptionChosen(DeleteOptions option);
    void hardlinkTriggered();
    void addDirectoryRequested(QString const& path);
    void removeSelectedDirectoriesRequested(QStringList const& paths);
    void databaseLoadRequested(QString const& path);
    void databaseCreateRequested(QString const& path);

public slots:                                   // receive from controller
    void onDuplicateGroupsUpdated(const std::vector<std::vector<VideoInfo>>& groups);
    void onDirectoryListUpdated(const QStringList& dirs);
    void setCurrentDatabase(QString const& path);
    void onSearchSettingsLoaded(const SearchSettings& settings);

private slots:                
    void onSearchClicked();
    void onSelectClicked();
    void onSortClicked();
    void onSortGroupsClicked();
    void onDeleteClicked();
    void onHardlinkClicked();
    void onRowActivated(const QModelIndex&);

    void onAddDirectoryButtonClicked();
    void onPickDirectoryButtonClicked();
    void onRemoveDirectoryButtonClicked();
    void onLoadDbClicked();
    void onNewDbClicked();
    void onValidatePatternsClicked();

private:
    bool           eventFilter(QObject*, QEvent*) override;
    SearchSettings collectSearchSettings() const;

    // --- settings persistence ---
    void  applySearchSettings(SearchSettings const&);   // fill widgets
    void  saveCurrentSettings();                        // read + store
    int   m_settingsTabIdx { -1 };                      // index of Settings tab

    // data
    Ui::MainWindow*             ui   = nullptr;
    std::unique_ptr<VideoModel> m_model;

    DatabaseManager*            m_db = nullptr;          // already existing elsewhere in app

    QThread                     m_codecThread;
};

