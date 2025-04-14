#pragma once

#include "VideoInfo.h"
#include <QMainWindow>
#include <memory>

class VideoModel; // forward declaration

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void setDuplicateVideoGroups(std::vector<std::vector<VideoInfo>> const& groups);


signals:
    void searchTriggered();

private slots:
    void onSearchClicked();

private:
    Ui::MainWindow *ui;
    std::unique_ptr<VideoModel> m_model;
};

