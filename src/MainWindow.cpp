#include "MainWindow.h"
#include "GroupRowDelegate.h"
#include "VideoModel.h"
#include "ui_MainWindow.h"

#include <QAction>
#include <QDebug>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QTableView>
#include <QUrl>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_model(std::make_unique<VideoModel>(this))
{
    ui->setupUi(this);

    ui->directoryPanel->setVisible(true);

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
    view->verticalHeader()->setDefaultSectionSize(120);

    // Connect the add/remove directory UI
    connect(ui->addDirectoryButton, &QPushButton::clicked,
        this, &MainWindow::onAddDirectoryButtonClicked);
    connect(ui->pickDirectoryButton, &QToolButton::clicked,
        this, &MainWindow::onPickDirectoryButtonClicked);
    connect(ui->removeDirectoryButton, &QPushButton::clicked,
        this, &MainWindow::onRemoveDirectoryButtonClicked);

    // Connect the five bottom buttons to local slots
    connect(ui->searchButton, &QPushButton::clicked, this, &MainWindow::onSearchClicked);
    connect(ui->selectButton, &QPushButton::clicked, this, &MainWindow::onSelectClicked);
    connect(ui->sortButton, &QPushButton::clicked, this, &MainWindow::onSortClicked);
    connect(ui->sortGroupsButton, &QPushButton::clicked, this, &MainWindow::onSortGroupsClicked);
    connect(ui->deleteButton, &QPushButton::clicked, this, &MainWindow::onDeleteClicked);

    connect(ui->tableView->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, [&](QItemSelection const& selected, QItemSelection const&) {
            for (QModelIndex const& index : selected.indexes()) {
                if (index.column() == 0) { // Avoid duplicate row triggers
                    m_model->selectRow(index.row());
                }
            }
        });

    connect(ui->tableView, &QTableView::activated,
        this, &MainWindow::onRowActivated);

    connect(ui->toggleDirectoryButton, &QToolButton::clicked, this, [=, this](bool checked) {
        ui->directoryPanel->setVisible(checked);
        ui->toggleDirectoryButton->setText(checked ? QString("▼ Search Directories")
                                                   : QString("▲ Search Directories"));
    });
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
    emit searchTriggered();
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

void MainWindow::onAddDirectoryButtonClicked()
{
    QString typedPath = ui->directoryLineEdit->text().trimmed();
    if (!typedPath.isEmpty()) {
        emit addDirectoryRequested(typedPath);
    }
}

void MainWindow::onPickDirectoryButtonClicked()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Choose Directory");
    if (!dir.isEmpty()) {
        ui->directoryLineEdit->setText(dir);
        emit addDirectoryRequested(dir);
    }
}

void MainWindow::onRemoveDirectoryButtonClicked()
{
    auto selectedItems = ui->directoryListWidget->selectedItems();
    if (selectedItems.isEmpty())
        return;

    QStringList dirsToRemove;
    for (auto* item : selectedItems) {
        dirsToRemove << item->text();
    }
    emit removeSelectedDirectoriesRequested(dirsToRemove);
}

void MainWindow::onDirectoryListUpdated(QStringList const& directories)
{
    ui->directoryListWidget->clear();
    for (auto const& dir : directories) {
        ui->directoryListWidget->addItem(dir);
    }
}

void MainWindow::onRowActivated(QModelIndex const& index)
{
    if (!index.isValid())
        return;

    auto const& entry = m_model->rowEntry(index.row());
    if (entry.type != RowType::Video)
        return;

    QString const filePath = QString::fromStdString(entry.video->path);
    QFileInfo info(filePath);
    if (info.exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    } else {
        qWarning() << "Cannot open. File does not exist:" << filePath;
    }
}
