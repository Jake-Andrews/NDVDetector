#pragma once

#include <QMainWindow>
#include <QThread>
#include <QVector>
#include <memory>
#include <vector>

#include "DatabaseManager.h"
#include "SearchSettings.h"
#include "VideoInfo.h"

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
    enum SortOptions { Size, CreatedAt };
    Q_ENUM(SortOptions)

signals:                     
    void searchRequested(SearchSettings);
    void selectOptionChosen(MainWindow::SelectOptions);
    void sortOptionChosen(MainWindow::SortOptions);
    void sortGroupsOptionChosen(MainWindow::SortOptions);
    void deleteOptionChosen(MainWindow::DeleteOptions);
    void hardlinkTriggered();

    void addDirectoryRequested(QString const&);
    void removeSelectedDirectoriesRequested(QStringList const&);
    void databaseLoadRequested(QString const&);
    void databaseCreateRequested(QString const&);

public slots:
    void onDirectoryListUpdated(QStringList const&);
    void onDuplicateGroupsUpdated(const std::vector<std::vector<VideoInfo>>&);
    void setCurrentDatabase(QString const&);

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

    // data
    Ui::MainWindow*             ui   = nullptr;
    std::unique_ptr<VideoModel> m_model;

    DatabaseManager*            m_db = nullptr;          // already existing elsewhere in app

    QThread                     m_codecThread;
};

