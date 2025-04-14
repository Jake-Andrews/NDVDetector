#include "MainWindow.h"
#include "VideoModel.h"
#include <QHeaderView>
#include <QPushButton>
#include <QTableView>
#include <ui_MainWindow.h>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_model(std::make_unique<VideoModel>(this))
{
    ui->setupUi(this);

    connect(ui->searchButton, &QPushButton::clicked, this, &MainWindow::onSearchClicked);

    // QTableView "tableView" in mainwindow.ui
    auto* view = ui->tableView;
    view->setModel(m_model.get());

    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    view->setIconSize(QSize(128, 128));
    view->verticalHeader()->setDefaultSectionSize(80);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::onSearchClicked()
{
    emit searchTriggered();
}

void MainWindow::setDuplicateVideoGroups(std::vector<std::vector<VideoInfo>> const& groups)
{
    m_model->setGroupedVideos(groups);
}
