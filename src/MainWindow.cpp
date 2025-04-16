#include "MainWindow.h"
#include "GroupRowDelegate.h"
#include "VideoModel.h"
#include "ui_MainWindow.h"

#include <QAction>
#include <QDebug>
#include <QHeaderView>
#include <QMenu>
#include <QPushButton>
#include <QTableView>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_model(std::make_unique<VideoModel>(this))
{
    ui->setupUi(this);

    // Set up QTableView
    auto* view = ui->tableView;
    view->setModel(m_model.get());

    view->setItemDelegate(new GroupRowDelegate(this));

    // Remove grid lines so group separator rows appear continuous
    view->setShowGrid(false);

    // Entire row highlight
    view->setSelectionBehavior(QAbstractItemView::SelectRows);

    // Center screenshots in their column
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    view->setIconSize(QSize(128, 128));
    view->verticalHeader()->setDefaultSectionSize(80);

    // Connect the five bottom buttons to local slots
    connect(ui->searchButton, &QPushButton::clicked, this, &MainWindow::onSearchClicked);
    connect(ui->selectButton, &QPushButton::clicked, this, &MainWindow::onSelectClicked);
    connect(ui->sortButton, &QPushButton::clicked, this, &MainWindow::onSortClicked);
    connect(ui->sortGroupsButton, &QPushButton::clicked, this, &MainWindow::onSortGroupsClicked);
    connect(ui->deleteButton, &QPushButton::clicked, this, &MainWindow::onDeleteClicked);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setDuplicateVideoGroups(std::vector<std::vector<VideoInfo>> const& groups)
{
    m_model->setGroupedVideos(groups);
}

// This slot is called when the controller signals that new duplicates are available
void MainWindow::onDuplicateGroupsUpdated(std::vector<std::vector<VideoInfo>> const& groups)
{
    qDebug() << "[MainWindow] Received new duplicate groups with size:" << groups.size();

    setDuplicateVideoGroups(groups);
}

void MainWindow::onSearchClicked()
{
    auto rootPath = QString("./");
    emit searchTriggered(rootPath);
}

void MainWindow::onSelectClicked()
{
    QMenu menu(this);
    QAction* exceptLargest = menu.addAction("Select All Except Largest");
    QAction* exceptSmallest = menu.addAction("Select All Except Smallest");

    QAction* chosen = menu.exec(QCursor::pos());
    if (!chosen)
        return;

    if (chosen == exceptLargest) {
        emit selectOptionChosen(SelectOptions::AllExceptLargest);
    } else if (chosen == exceptSmallest) {
        emit selectOptionChosen(SelectOptions::AllExceptSmallest);
    }
}

void MainWindow::onSortClicked()
{
    QMenu menu(this);
    QAction* sortBySize = menu.addAction("Sort By Size");
    QAction* sortByCreatedAt = menu.addAction("Sort By CreatedAt");

    QAction* chosen = menu.exec(QCursor::pos());
    if (!chosen)
        return;

    if (chosen == sortBySize) {
        emit sortOptionChosen(SortOptions::Size);
    } else if (chosen == sortByCreatedAt) {
        emit sortOptionChosen(SortOptions::CreatedAt);
    }
}

void MainWindow::onSortGroupsClicked()
{
    QMenu menu(this);
    QAction* sortGroupsBySize = menu.addAction("Sort Groups By Size");
    QAction* sortGroupsByCreated = menu.addAction("Sort Groups By CreatedAt");

    QAction* chosen = menu.exec(QCursor::pos());
    if (!chosen)
        return;

    if (chosen == sortGroupsBySize) {
        emit sortGroupsOptionChosen(SortOptions::Size);
    } else if (chosen == sortGroupsByCreated) {
        emit sortGroupsOptionChosen(SortOptions::CreatedAt);
    }
}

void MainWindow::onDeleteClicked()
{
    QMenu menu(this);
    QAction* delFromList = menu.addAction("Delete From List");
    QAction* delListAndDb = menu.addAction("Delete From List + DB");
    QAction* delFromDisk = menu.addAction("Delete From Disk");

    QAction* chosen = menu.exec(QCursor::pos());
    if (!chosen)
        return;

    if (chosen == delFromList) {
        emit deleteOptionChosen(DeleteOptions::List);
    } else if (chosen == delListAndDb) {
        emit deleteOptionChosen(DeleteOptions::ListDB);
    } else if (chosen == delFromDisk) {
        emit deleteOptionChosen(DeleteOptions::Disk);
    }
}
