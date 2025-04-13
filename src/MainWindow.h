#pragma once

#include <QMainWindow>
#include <vector>
#include "VideoInfo.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void setVideoInfoList(std::vector<VideoInfo> const& videos);
    void setDuplicateVideoGroups(std::vector<std::vector<VideoInfo>> const& groups);


private:
    Ui::MainWindow *ui;
};


