#pragma once

#include <QMainWindow>
#include <memory>
#include <vector>
#include "VideoInfo.h"

class VideoModel;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void setDuplicateVideoGroups(std::vector<std::vector<VideoInfo>> const& groups);
    VideoModel* model() const {return m_model.get();};

    enum DeleteOptions {
        List, 
        ListDB,
        Disk
    };
    Q_ENUM(DeleteOptions);

    // **Provide an option that takes into account estimated quality**
    // Example: 265 2GB vs 264 2.4GB, 265 2GB is higher "quality"
    enum SelectOptions { 
        AllExceptLargest,
        AllExceptSmallest, 
        Smallest, 
        Largest, 
    };
    Q_ENUM(SelectOptions);

    enum SortOptions {
        Size,
        CreatedAt
    };
    Q_ENUM(SortOptions);

signals:
    void searchTriggered(QString rootPath);
    void selectOptionChosen(MainWindow::SelectOptions option);
    void sortOptionChosen(MainWindow::SortOptions option);
    void sortGroupsOptionChosen(MainWindow::SortOptions option);
    void deleteOptionChosen(MainWindow::DeleteOptions option);

public slots:
    void onDuplicateGroupsUpdated(std::vector<std::vector<VideoInfo>> const& groups);

private slots:
    void onSearchClicked();
    void onSelectClicked();
    void onSortClicked();
    void onSortGroupsClicked();
    void onDeleteClicked();
    void onRowActivated(const QModelIndex& index);

private:
    Ui::MainWindow *ui;
    std::unique_ptr<VideoModel> m_model;
};


