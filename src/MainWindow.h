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

signals:
    void searchTriggered();
    void selectOptionChosen(QString option);
    void sortOptionChosen(QString option);
    void sortGroupsOptionChosen(QString option);
    void deleteOptionChosen(QString option);

public slots:
    // Called when the controller finishes detecting duplicates
    void onDuplicateGroupsUpdated(std::vector<std::vector<VideoInfo>> const& groups);

private slots:
    void onSearchClicked();
    void onSelectClicked();
    void onSortClicked();
    void onSortGroupsClicked();
    void onDeleteClicked();

private:
    Ui::MainWindow *ui;
    std::unique_ptr<VideoModel> m_model;
};


