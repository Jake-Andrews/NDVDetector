#include "MainWindow.h"
#include "GPUVendor.h"
#include "GroupRowDelegate.h"
#include "RegexTesterDialog.h"
#include "SearchSettings.h"
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
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_model(std::make_unique<VideoModel>(this))
{
    ui->setupUi(this);

    ui->directoryPanel->setVisible(true);
    ui->currentDbLineEdit->clear();

    // Set up QTableView
    // Set up QTableView
    auto* view = ui->tableView;
    view->setModel(m_model.get());

    QHeaderView* hHeader = view->horizontalHeader();
    if (hHeader) {
        hHeader->setSectionResizeMode(QHeaderView::Stretch);
        hHeader->setStretchLastSection(true);
    }

    view->setItemDelegate(new GroupRowDelegate(this));

    ui->tableView->viewport()->installEventFilter(this);

    view->setShowGrid(false);

    // Entire row highlight
    view->setSelectionBehavior(QAbstractItemView::SelectRows);

    // Add this line to prevent clicking on separator rows from selecting them
    view->setSelectionMode(QAbstractItemView::SingleSelection);

    // Disable focus rectangles which can appear on cells
    view->setFocusPolicy(Qt::StrongFocus);

    // Center screenshots in their column
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    view->setIconSize(QSize(128, 128));
    view->verticalHeader()->setDefaultSectionSize(120);

    // Add handler for clicked signal to prevent selection of separator rows
    connect(view, &QTableView::clicked, this, [this, view](QModelIndex const& index) {
        if (!index.isValid())
            return;

        auto const& entry = m_model->rowEntry(index.row());
        if (entry.type == RowType::Separator) {
            // Clear the selection when clicking on separator rows
            view->selectionModel()->clear();
        }
    });

    connect(ui->validatePatternsButton, &QPushButton::clicked,
        this, &MainWindow::onValidatePatternsClicked);

    connect(ui->regexTesterButton, &QPushButton::clicked, this, [this] {
        RegexTesterDialog(this).exec();
    });

    connect(ui->loadDbButton, &QPushButton::clicked,
        this, &MainWindow::onLoadDbClicked);
    connect(ui->newDbButton, &QPushButton::clicked,
        this, &MainWindow::onNewDbClicked);

    connect(ui->addDirectoryButton, &QPushButton::clicked,
        this, &MainWindow::onAddDirectoryButtonClicked);
    connect(ui->pickDirectoryButton, &QToolButton::clicked,
        this, &MainWindow::onPickDirectoryButtonClicked);
    connect(ui->removeDirectoryButton, &QPushButton::clicked,
        this, &MainWindow::onRemoveDirectoryButtonClicked);

    connect(ui->searchButton, &QPushButton::clicked, this, &MainWindow::onSearchClicked);
    connect(ui->selectButton, &QPushButton::clicked, this, &MainWindow::onSelectClicked);
    connect(ui->sortButton, &QPushButton::clicked, this, &MainWindow::onSortClicked);
    connect(ui->sortGroupsButton, &QPushButton::clicked, this, &MainWindow::onSortGroupsClicked);
    connect(ui->deleteButton, &QPushButton::clicked, this, &MainWindow::onDeleteClicked);
    connect(ui->hardlinkButton, &QPushButton::clicked,
        this, &MainWindow::onHardlinkClicked);

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

void MainWindow::onDuplicateGroupsUpdated(std::vector<std::vector<VideoInfo>> const& groups)
{
    qDebug() << "[MainWindow] Received new duplicate groups with size:" << groups.size();

    setDuplicateVideoGroups(groups);
}

void MainWindow::onSearchClicked()
{
    SearchSettings cfg = collectSearchSettings();
    emit searchRequested(cfg);
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

void MainWindow::onHardlinkClicked()
{
    emit hardlinkTriggered();
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
        dirsToRemove << item->text(0);
    }
    emit removeSelectedDirectoriesRequested(dirsToRemove);
}

void MainWindow::onDirectoryListUpdated(QStringList const& directories)
{
    ui->directoryListWidget->clear();
    for (auto const& dirPath : directories) {
        auto* item = new QTreeWidgetItem(ui->directoryListWidget);
        item->setText(0, dirPath);           // Column 0: path
        item->setCheckState(1, Qt::Checked); // Default recursive enabled
    }

    // Set headers for columns if not already set
    if (ui->directoryListWidget->headerItem()->text(0).isEmpty()) {
        QStringList headers;
        headers << "Directory" << "Recursive";
        ui->directoryListWidget->setHeaderLabels(headers);
        ui->directoryListWidget->setColumnCount(2);

        ui->directoryListWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);          // "Directory"
        ui->directoryListWidget->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents); // "Recursive"
    }
}

void MainWindow::onRowActivated(QModelIndex const& index)
{
    if (!index.isValid())
        return;

    auto const& entry = m_model->rowEntry(index.row());
    if (entry.type != RowType::Video)
        return; // Skip activation for separator rows

    QString const filePath = QString::fromStdString(entry.video->path);
    QFileInfo info(filePath);
    if (info.exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    } else {
        qWarning() << "Cannot open. File does not exist:" << filePath;
    }
}
bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == ui->tableView->viewport()) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick) {

            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            QPoint pos = mouseEvent->pos();
            QModelIndex index = ui->tableView->indexAt(pos);

            if (index.isValid()) {
                auto const& entry = m_model->rowEntry(index.row());
                if (entry.type == RowType::Separator) {
                    // Prevent the event from being processed for separator rows
                    return true;
                }
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onLoadDbClicked()
{
    QString f = QFileDialog::getOpenFileName(this, "Open Database", {}, "*.db");
    if (!f.isEmpty())
        emit databaseLoadRequested(f);
}

void MainWindow::onNewDbClicked()
{
    QString f = QFileDialog::getSaveFileName(this, "New Database", {}, "*.db");
    if (!f.isEmpty())
        emit databaseCreateRequested(f);
}

void MainWindow::setCurrentDatabase(QString const& path)
{
    ui->currentDbLineEdit->setText(path);
}

std::vector<DirectoryEntry> MainWindow::getDirectorySettings() const
{
    std::vector<DirectoryEntry> dirs;

    for (int i = 0; i < ui->directoryListWidget->topLevelItemCount(); ++i) {
        auto* item = ui->directoryListWidget->topLevelItem(i);
        QString path = item->text(0);
        bool recursive = item->checkState(1) == Qt::Checked;
        dirs.push_back({ path.toStdString(), recursive });
    }

    return dirs;
}

SearchSettings MainWindow::collectSearchSettings() const
{
    SearchSettings s;

    s.useGlob = ui->globCheckBox->isChecked();
    s.caseInsensitive = ui->caseCheckBox->isChecked();

    // file extensions
    QString const extText = ui->extensionsEdit->text().trimmed();
    if (!extText.isEmpty()) {
        for (QString ext : extText.split(',', Qt::SkipEmptyParts)) {
            ext = ext.trimmed().toLower();
            if (!ext.startsWith('.'))
                ext.prepend('.');
            s.extensions.push_back(ext.toStdString());
        }
    }

    // regex lists
    auto splitLines = [](QString const& txt) {
        QStringList ls = txt.split('\n', Qt::SkipEmptyParts);
        for (auto& l : ls)
            l = l.trimmed();
        return ls;
    };
    for (QString const& p : splitLines(ui->includeFileEdit->toPlainText()))
        s.includeFilePatterns.push_back(p.toStdString());
    for (QString const& p : splitLines(ui->includeDirEdit->toPlainText()))
        s.includeDirPatterns.push_back(p.toStdString());
    for (QString const& p : splitLines(ui->excludeFileEdit->toPlainText()))
        s.excludeFilePatterns.push_back(p.toStdString());
    for (QString const& p : splitLines(ui->excludeDirEdit->toPlainText()))
        s.excludeDirPatterns.push_back(p.toStdString());

    // size limits
    if (ui->minBytesSpin->value() > 0) {
        s.minBytes = static_cast<std::uint64_t>(ui->minBytesSpin->value() * 1024 * 1024);
    }

    if (ui->maxBytesSpin->value() > 0) {
        s.maxBytes = static_cast<std::uint64_t>(ui->maxBytesSpin->value() * 1024 * 1024);
    }

    // directories
    s.directories = getDirectorySettings();
    compileAllRegexes(s);

    // hardware
    auto hw_usable = [](AVHWDeviceType t) -> bool {
        if (t == AV_HWDEVICE_TYPE_DRM)
            return false;
        AVBufferRef* raw = nullptr;
        bool const ok = av_hwdevice_ctx_create(&raw, t, nullptr, nullptr, 0) >= 0;
        av_buffer_unref(&raw);
        return ok;
    };

    auto choose_backend = [&](GPUVendor vendor) -> AVHWDeviceType {
        std::vector<std::pair<std::string, AVHWDeviceType>> list = make_priority_list(vendor);

        // Prefer DRM first for AMD/Intel since it enables zero-copy with VAAPI
        if (vendor == GPUVendor::AMD || vendor == GPUVendor::Intel)
            list.insert(list.begin(), { "", AV_HWDEVICE_TYPE_DRM });

        for (auto const& entry : list) {
            AVHWDeviceType dev = entry.second;
            if (dev == AV_HWDEVICE_TYPE_NONE)
                break; // sentinel

            spdlog::info("[HW] probing {}", av_hwdevice_get_type_name(dev));
            if (hw_usable(dev)) {
                spdlog::info("[HW] using {}", av_hwdevice_get_type_name(dev));
                return dev;
            }
        }

        spdlog::error("[HW] No usable backend for vendor {}, falling back to CPU",
            static_cast<int>(vendor));
        return AV_HWDEVICE_TYPE_NONE;
    };

    /* ── Map UI combo‑box selection ───────────────────────────────────── */

    switch (ui->hwDecodeComboBox->currentIndex()) {
    case 1: // Nvidia
        s.hwBackend = choose_backend(GPUVendor::Nvidia);
        break;
    case 2: // Intel
        s.hwBackend = choose_backend(GPUVendor::Intel);
        break;
    case 3: // AMD
        s.hwBackend = choose_backend(GPUVendor::AMD);
        break;
    case 4: // CPU only
        s.hwBackend = AV_HWDEVICE_TYPE_NONE;
        spdlog::info("[HW] Forced CPU decoding");
        break;
    default: { // Auto
        GPUVendor const detected = detect_gpu();
        spdlog::info("[HW] Auto‑detect found vendor {}", static_cast<int>(detected));
        s.hwBackend = choose_backend(detected);
        break;
    }
    }
    return s;
}

void MainWindow::onValidatePatternsClicked()
{
    // build a *temporary* settings object from the form
    SearchSettings s;
    s.useGlob = ui->globCheckBox->isChecked();
    s.caseInsensitive = ui->caseCheckBox->isChecked();

    auto collect = [](QPlainTextEdit* w) {
        std::vector<std::string> v;
        for (auto& ln : w->toPlainText().split('\n', Qt::SkipEmptyParts))
            v.emplace_back(ln.trimmed().toStdString());
        return v;
    };
    s.includeFilePatterns = collect(ui->includeFileEdit);
    s.includeDirPatterns = collect(ui->includeDirEdit);
    s.excludeFilePatterns = collect(ui->excludeFileEdit);
    s.excludeDirPatterns = collect(ui->excludeDirEdit);

    auto const errs = compileAllRegexes(s);

    if (errs.empty()) {
        QMessageBox::information(this, tr("Pattern check"),
            tr("✅  All patterns compiled successfully."));
    } else {
        QString msg = tr("The following patterns failed:\n• %1")
                          .arg(QString::fromStdString(std::accumulate(
                              std::next(errs.begin()), errs.end(), errs.front(),
                              [](std::string a, std::string const& b) { return a + "\n• " + b; })));
        QMessageBox::critical(this, tr("Pattern errors"), msg);
    }
}
