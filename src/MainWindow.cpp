// MainWindow.cpp
#include "MainWindow.h"

#include "FFProbeExtractor.h"
#include "GPUVendor.h"
#include "GroupRowDelegate.h"
#include "RegexTesterDialog.h"
#include "VideoModel.h"
#include "ui_MainWindow.h"

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QToolButton>
#include <numeric>
#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

using namespace std::string_literals;

MainWindow::MainWindow(DatabaseManager* db, QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_model(std::make_unique<VideoModel>(this))
    , m_db(db)
{
    ui->setupUi(this);

    // -- video list view setup --
    auto* view = ui->tableView;
    view->setModel(m_model.get());
    view->setItemDelegate(new GroupRowDelegate(this));
    view->viewport()->installEventFilter(this);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    view->verticalHeader()->setDefaultSectionSize(120);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setIconSize(QSize(128, 128));
    view->setFocusPolicy(Qt::StrongFocus);

    // -- hardware-accel test view setup --
    m_testModel = std::make_unique<CustomTestModel>(m_db, this);

    auto* testView = ui->customTestsView;  //
    testView->setModel(m_testModel.get()); // default delegate is OK
    testView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    testView->verticalHeader()->setDefaultSectionSize(24);
    testView->setShowGrid(false);
    testView->setSelectionBehavior(QAbstractItemView::SelectRows);
    testView->setSelectionMode(QAbstractItemView::SingleSelection);
    testView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

    m_codecWorker = new CodecTestWorker;
    m_codecWorker->moveToThread(&m_codecThread);

    connect(&m_codecThread, &QThread::finished,
        m_codecWorker, &QObject::deleteLater);

    connect(m_codecWorker, &CodecTestWorker::progress,
        this, [this](int d, int t) {
            ui->progressBar->setValue(t ? 100 * d / t : 0);
        });

    connect(m_codecWorker, &CodecTestWorker::result, this,
        [this](TestItem const& it, bool hw, bool sw) {
            m_testModel->updateResult(it.path, hw, sw);
            ui->statusLabel->setText(
                QFileInfo(it.path).fileName() + " —  HW " + (hw ? "✅" : "❌") + "  SW " + (sw ? "✅" : "❌"));
        });

    connect(m_codecWorker, &CodecTestWorker::finished,
        this, [this] {
            ui->statusLabel->setText(tr("Finished"));
            ui->runButton->setEnabled(true);
        });

    connect(m_codecWorker, &CodecTestWorker::finished,
        &m_codecThread, &QThread::quit);

    // ---------- Button hooks ------------------------------------------------
    connect(ui->addVideoButton, &QPushButton::clicked, this, &MainWindow::onAddVideoClicked);
    connect(ui->runButton, &QPushButton::clicked, this, &MainWindow::onRunTestsClicked);

    // ---------- Misc UI behaviour -------------------------------------------
    connect(view, &QTableView::clicked, this, [this, view](QModelIndex const& ix) {
        if (ix.isValid() && m_model->rowEntry(ix.row()).type == RowType::Separator)
            view->selectionModel()->clear();
    });

    connect(ui->tableView, &QTableView::activated, this, &MainWindow::onRowActivated);

    connect(ui->toggleDirectoryButton, &QToolButton::clicked, this, [this](bool checked) {
        ui->directoryPanel->setVisible(checked);
        ui->toggleDirectoryButton->setText(checked ? tr("▼ Search Directories")
                                                   : tr("▲ Search Directories"));
    });

    ui->directoryPanel->setVisible(true);
    ui->currentDbLineEdit->clear();

    // ---------- Directory panel hooks --------------------------------------
    connect(ui->addDirectoryButton, &QPushButton::clicked, this, &MainWindow::onAddDirectoryButtonClicked);
    connect(ui->pickDirectoryButton, &QToolButton::clicked, this, &MainWindow::onPickDirectoryButtonClicked);
    connect(ui->removeDirectoryButton, &QPushButton::clicked, this, &MainWindow::onRemoveDirectoryButtonClicked);

    // ---------- Search / sort etc. buttons ---------------------------------
    connect(ui->searchButton, &QPushButton::clicked, this, &MainWindow::onSearchClicked);
    connect(ui->selectButton, &QPushButton::clicked, this, &MainWindow::onSelectClicked);
    connect(ui->sortButton, &QPushButton::clicked, this, &MainWindow::onSortClicked);
    connect(ui->sortGroupsButton, &QPushButton::clicked, this, &MainWindow::onSortGroupsClicked);
    connect(ui->deleteButton, &QPushButton::clicked, this, &MainWindow::onDeleteClicked);
    connect(ui->hardlinkButton, &QPushButton::clicked, this, &MainWindow::onHardlinkClicked);

    // ---------- Patterns & DB utilities -------------------------------------
    connect(ui->validatePatternsButton, &QPushButton::clicked, this, &MainWindow::onValidatePatternsClicked);
    connect(ui->regexTesterButton, &QPushButton::clicked, this, [this] { RegexTesterDialog(this).exec(); });

    connect(ui->loadDbButton, &QPushButton::clicked, this, &MainWindow::onLoadDbClicked);
    connect(ui->newDbButton, &QPushButton::clicked, this, &MainWindow::onNewDbClicked);

    // Selection model sync with custom delegate ------------------------------
    connect(ui->tableView->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, [this](QItemSelection const& sel, QItemSelection const&) {
            for (QModelIndex const& ix : sel.indexes())
                if (ix.column() == 0)
                    m_model->selectRow(ix.row());
        });
}

MainWindow::~MainWindow()
{
    m_codecThread.quit();
    m_codecThread.wait();
    delete ui;
}

// UI table helper
void MainWindow::setDuplicateVideoGroups(
    std::vector<std::vector<VideoInfo>> const& groups)
{
    m_model->setGroupedVideos(groups);
}
void MainWindow::onDuplicateGroupsUpdated(
    std::vector<std::vector<VideoInfo>> const& groups)
{
    spdlog::debug("[MainWindow] got {} duplicate groups", groups.size());
    setDuplicateVideoGroups(groups);
}

void MainWindow::onSearchClicked()
{
    emit searchRequested(collectSearchSettings());
}

void MainWindow::onSelectClicked()
{
    QMenu m(this);
    QAction* a1 = m.addAction(tr("Select All Except Largest"));
    QAction* a2 = m.addAction(tr("Select All Except Smallest"));
    QAction* selected = m.exec(QCursor::pos());
    if (!selected)
        return;

    if (selected == a1)
        emit selectOptionChosen(AllExceptLargest);
    else if (selected == a2)
        emit selectOptionChosen(AllExceptSmallest);
}

void MainWindow::onSortClicked()
{
    QMenu m(this);
    QAction* aSize = m.addAction(tr("Sort By Size"));
    QAction* aDate = m.addAction(tr("Sort By CreatedAt"));
    QAction* selected = m.exec(QCursor::pos());
    if (!selected)
        return;

    if (selected == aSize)
        emit sortOptionChosen(Size);
    else if (selected == aDate)
        emit sortOptionChosen(CreatedAt);
}

void MainWindow::onSortGroupsClicked()
{
    QMenu m(this);
    QAction* aSize = m.addAction(tr("Sort Groups By Size"));
    QAction* aDate = m.addAction(tr("Sort Groups By CreatedAt"));
    QAction* selected = m.exec(QCursor::pos());
    if (!selected)
        return;

    if (selected == aSize)
        emit sortGroupsOptionChosen(Size);
    else if (selected == aDate)
        emit sortGroupsOptionChosen(CreatedAt);
}

void MainWindow::onDeleteClicked()
{
    QMenu m(this);
    QAction* aList = m.addAction(tr("Delete From List"));
    QAction* aListDb = m.addAction(tr("Delete From List + DB"));
    QAction* aDisk = m.addAction(tr("Delete From Disk"));
    QAction* selected = m.exec(QCursor::pos());
    if (!selected)
        return;

    if (selected == aList)
        emit deleteOptionChosen(List);
    else if (selected == aListDb)
        emit deleteOptionChosen(ListDB);
    else if (selected == aDisk)
        emit deleteOptionChosen(Disk);
}

void MainWindow::onHardlinkClicked() { emit hardlinkTriggered(); }

/*──────────────────────────────────────────────────────────────
 *  DIRECTORY PANEL
 *────────────────────────────────────────────────────────────*/
void MainWindow::onAddDirectoryButtonClicked()
{
    if (QString p = ui->directoryLineEdit->text().trimmed(); !p.isEmpty())
        emit addDirectoryRequested(p);
}
void MainWindow::onPickDirectoryButtonClicked()
{

    QString dir = QFileDialog::getExistingDirectory(this, tr("Choose Directory"), QString(), QFileDialog::DontUseNativeDialog);
    if (!dir.isEmpty()) {
        ui->directoryLineEdit->setText(dir);
        emit addDirectoryRequested(dir);
    }
}
void MainWindow::onRemoveDirectoryButtonClicked()
{
    auto selected = ui->directoryListWidget->selectedItems();
    if (selected.isEmpty())
        return;
    QStringList remove;
    for (auto* it : selected)
        remove << it->text(0);
    emit removeSelectedDirectoriesRequested(remove);
}

void MainWindow::onDirectoryListUpdated(QStringList const& dirs)
{
    ui->directoryListWidget->clear();
    for (QString const& p : dirs) {
        auto* it = new QTreeWidgetItem(ui->directoryListWidget);
        it->setText(0, p);
        it->setCheckState(1, Qt::Checked);
    }
    if (ui->directoryListWidget->headerItem()->text(0).isEmpty()) {
        ui->directoryListWidget->setHeaderLabels({ tr("Directory"), tr("Recursive") });
        ui->directoryListWidget->setColumnCount(2);
        ui->directoryListWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        ui->directoryListWidget->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    }
}

/*──────────────────────────────────────────────────────────────
 *  open video on double‑click
 *────────────────────────────────────────────────────────────*/
// ─────────────────────────────────────────────────────────────────────────────
//  Double‑click to open video
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onRowActivated(QModelIndex const& ix)
{
    if (!ix.isValid())
        return;
    auto const& e = m_model->rowEntry(ix.row());
    if (e.type != RowType::Video)
        return;

    QString file = QString::fromStdString(e.video->path);
    if (QFileInfo::exists(file))
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
    else
        qWarning() << "file not found:" << file;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Event filter: suppress clicks on separator rows
// ─────────────────────────────────────────────────────────────────────────────
bool MainWindow::eventFilter(QObject* w, QEvent* ev)
{
    if (w == ui->tableView->viewport() && (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseButtonRelease || ev->type() == QEvent::MouseButtonDblClick)) {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (QModelIndex ix = ui->tableView->indexAt(me->pos()); ix.isValid() && m_model->rowEntry(ix.row()).type == RowType::Separator)
            return true; // eat the event
    }
    return QMainWindow::eventFilter(w, ev);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Database buttons
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onLoadDbClicked()
{
    QString f = QFileDialog::getOpenFileName(this, tr("Open Database"), {}, "*.db", nullptr, QFileDialog::DontUseNativeDialog);
    if (!f.isEmpty())
        emit databaseLoadRequested(f);
}
void MainWindow::onNewDbClicked()
{
    QString f = QFileDialog::getSaveFileName(this, tr("New Database"), {}, "*.db", nullptr, QFileDialog::DontUseNativeDialog);
    if (!f.isEmpty())
        emit databaseCreateRequested(f);
}
void MainWindow::setCurrentDatabase(QString const& path) { ui->currentDbLineEdit->setText(path); }

// ─────────────────────────────────────────────────────────────────────────────
//  Codec‑tests slots
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onAddVideoClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Choose Video(s)"), {}, {}, nullptr,
        QFileDialog::DontUseNativeDialog);
    for (QString const& f : files)
        addTestFile(f);
}

void MainWindow::onRunTestsClicked()
{
    ui->runButton->setEnabled(false);
    ui->statusLabel->setText(tr("Running…"));
    ui->progressBar->setValue(0);

    startCodecWorker(buildTestList());
}

void MainWindow::startCodecWorker(QVector<TestItem> const& tests)
{
    if (tests.isEmpty()) {
        ui->statusLabel->setText(tr("Nothing to test"));
        ui->runButton->setEnabled(true);
        return;
    }

    m_codecWorker->setTests(tests);
    if (!m_codecThread.isRunning())
        m_codecThread.start();

    QMetaObject::invokeMethod(m_codecWorker, "run", Qt::QueuedConnection);
}

QVector<TestItem> MainWindow::buildTestList() const
{
    return m_testModel->items();
}

SearchSettings MainWindow::collectSearchSettings() const
{
    SearchSettings s;
    s.useGlob = ui->globCheckBox->isChecked();
    s.caseInsensitive = ui->caseCheckBox->isChecked();

    /* extensions ------------------------------------------ */
    QString extTxt = ui->extensionsEdit->text().trimmed();
    if (!extTxt.isEmpty()) {
        for (QString e : extTxt.split(',', Qt::SkipEmptyParts)) {
            e = e.trimmed().toLower();
            if (!e.startsWith('.'))
                e.prepend('.');
            s.extensions.push_back(e.toStdString());
        }
    }

    /* regex / glob filters -------------------------------- */
    auto split = [](QString const& txt) {
        QStringList ls = txt.split('\n', Qt::SkipEmptyParts);
        for (QString& l : ls)
            l = l.trimmed();
        return ls;
    };
    for (QString const& p : split(ui->includeFileEdit->toPlainText()))
        s.includeFilePatterns.push_back(p.toStdString());
    for (QString const& p : split(ui->includeDirEdit->toPlainText()))
        s.includeDirPatterns.push_back(p.toStdString());
    for (QString const& p : split(ui->excludeFileEdit->toPlainText()))
        s.excludeFilePatterns.push_back(p.toStdString());
    for (QString const& p : split(ui->excludeDirEdit->toPlainText()))
        s.excludeDirPatterns.push_back(p.toStdString());

    /* size limits ----------------------------------------- */
    if (ui->minBytesSpin->value() > 0)
        s.minBytes = static_cast<std::uint64_t>(ui->minBytesSpin->value() * 1024 * 1024);
    if (ui->maxBytesSpin->value() > 0)
        s.maxBytes = static_cast<std::uint64_t>(ui->maxBytesSpin->value() * 1024 * 1024);

    /* directory list & regex compilation ------------------ */
    auto getDirSettings = [this]() {
        std::vector<DirectoryEntry> out;
        int n = ui->directoryListWidget->topLevelItemCount();
        for (int i = 0; i < n; ++i) {
            auto* it = ui->directoryListWidget->topLevelItem(i);
            out.push_back({ it->text(0).toStdString(),
                it->checkState(1) == Qt::Checked });
        }
        return out;
    };
    s.directories = getDirSettings();
    compileAllRegexes(s);

    /* HW backend selection -------------------------------- */
    auto hwUsable = [](AVHWDeviceType t) {
        if (t == AV_HWDEVICE_TYPE_DRM)
            return false; // unsupported here
        AVBufferRef* tmp = nullptr;
        bool ok = av_hwdevice_ctx_create(&tmp, t, nullptr, nullptr, 0) >= 0;
        av_buffer_unref(&tmp);
        return ok;
    };
    auto choose = [&](GPUVendor v) {
        for (auto [_, dev] : make_priority_list(v))
            if (dev != AV_HWDEVICE_TYPE_NONE && hwUsable(dev))
                return dev;
        return AV_HWDEVICE_TYPE_NONE;
    };

    switch (ui->hwDecodeComboBox->currentIndex()) {
    case 1:
        s.hwBackend = choose(GPUVendor::Nvidia);
        break;
    case 2:
        s.hwBackend = choose(GPUVendor::Intel);
        break;
    case 3:
        s.hwBackend = choose(GPUVendor::AMD);
        break;
    case 4:
        s.hwBackend = AV_HWDEVICE_TYPE_NONE;
        break;
    default:
        s.hwBackend = choose(detect_gpu());
        break;
    }
    return s;
}

void MainWindow::onValidatePatternsClicked()
{
    SearchSettings tmp;
    tmp.useGlob = ui->globCheckBox->isChecked();
    tmp.caseInsensitive = ui->caseCheckBox->isChecked();

    auto collect = [](QPlainTextEdit* w) {
        std::vector<std::string> v;
        for (QString const& ln : w->toPlainText().split('\n', Qt::SkipEmptyParts))
            v.emplace_back(ln.trimmed().toStdString());
        return v;
    };
    tmp.includeFilePatterns = collect(ui->includeFileEdit);
    tmp.includeDirPatterns = collect(ui->includeDirEdit);
    tmp.excludeFilePatterns = collect(ui->excludeFileEdit);
    tmp.excludeDirPatterns = collect(ui->excludeDirEdit);

    auto errs = compileAllRegexes(tmp);
    if (errs.empty()) {
        QMessageBox::information(this, tr("Pattern check"),
            tr("✅ All patterns compiled successfully."));
    } else {
        QString msg = tr("The following patterns failed:\n• %1")
                          .arg(QString::fromStdString(std::accumulate(
                              std::next(errs.begin()), errs.end(), errs.front(),
                              [](std::string a, std::string const& b) {
                                  return a + "\n• " + b;
                              })));
        QMessageBox::critical(this, tr("Pattern errors"), msg);
    }
}

void MainWindow::addTestFile(QString const& path)
{
    auto info = probe_video_codec_info(path);
    if (!info)
        return;

    auto [codec, pixFmt, profile, level] = *info;
    TestItem item { path, codec, pixFmt, profile, QString::number(level) };
    m_tests.push_back(item);              // keep the in‑memory list
    m_testModel->append(std::move(item)); // paint a new row
}
