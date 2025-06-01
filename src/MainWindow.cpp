// MainWindow.cpp
#include "MainWindow.h"

#include "ConfigManager.h"
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
#include <QSpinBox>
#include <QStringList>
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

    // remember “Settings” tab index
    m_settingsTabIdx = ui->tabWidget->indexOf(ui->settingsTab);

    // --- matching-threshold radio buttons (slow tab only) ---
    auto updateThresholdWidgetsSlow = [this](bool numMode) {
        ui->matchingThresholdNumSpinBox->setEnabled(numMode);
        ui->matchingThresholdPercentSpinBox->setEnabled(!numMode);
    };
    connect(ui->fixedNumThresholdRadio, &QRadioButton::toggled,
        this, updateThresholdWidgetsSlow);
    connect(ui->percentThresholdRadio, &QRadioButton::toggled,
        this, [updateThresholdWidgetsSlow](bool on) { updateThresholdWidgetsSlow(!on); });

    // force correct initial state
    updateThresholdWidgetsSlow(ui->fixedNumThresholdRadio->isChecked());

    connect(ui->hashMethodCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        ui->hashMethodStack, &QStackedWidget::setCurrentIndex);

    // -- video list view setup --
    auto* view = ui->tableView;
    view->setModel(m_model.get());

    // delegate with dynamic row-height
    auto* delegate = new GroupRowDelegate(this);
    delegate->setThumbnailsPerVideo(ui->thumbnailsSpin->value());
    view->setItemDelegate(delegate);

    view->viewport()->installEventFilter(this);

    // allow user drag-resize of rows
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Interactive);

    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    // NEW ─ let the thumbnail column grow / shrink with its pixmap
    view->horizontalHeader()->setSectionResizeMode(
        VideoModel::Col_Screenshot, // thumbnail column
        QHeaderView::ResizeToContents);
    view->verticalHeader()->setDefaultSectionSize(120);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    constexpr int cell = 128;
    constexpr int gap = 4;
    int n = ui->thumbnailsSpin->value();
    int w = n * cell + (n - 1) * gap;
    view->setIconSize(QSize(w, cell));
    view->setFocusPolicy(Qt::StrongFocus);

    // keep model + delegate in sync with the spin-box
    connect(ui->thumbnailsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
        this, [this, delegate](int n) {
            m_model->setThumbnailsPerVideo(n);
            delegate->setThumbnailsPerVideo(n);
            constexpr int cell = 128;
            constexpr int gap = 4;
            int w = n * cell + (n - 1) * gap;
            ui->tableView->setIconSize(QSize(w, cell));
            ui->tableView->doItemsLayout(); // recompute geometry
            ui->tableView->resizeColumnToContents(
                VideoModel::Col_Screenshot);
        });

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

    /* ---- load settings from DB & populate widgets ---- */
    applySearchSettings(m_db->loadSettings());

    /* ---- save when user leaves the Settings tab ---- */
    int initialIdx = ui->tabWidget->currentIndex();
    connect(ui->tabWidget, &QTabWidget::currentChanged,
        this,
        [this, prev = initialIdx](int newIdx) mutable {
            if (prev == m_settingsTabIdx && newIdx != m_settingsTabIdx)
                saveCurrentSettings(); // user just left Settings
            prev = newIdx;
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
    ui->tableView->resizeColumnToContents( // NEW
        VideoModel::Col_Screenshot);
}
void MainWindow::onDuplicateGroupsUpdated(
    std::vector<std::vector<VideoInfo>> const& groups)
{
    spdlog::debug("[MainWindow] got {} duplicate groups", groups.size());
    setDuplicateVideoGroups(groups);
}

void MainWindow::onSearchClicked()
{
    SearchSettings s = collectSearchSettings();
    m_model->setThumbnailsPerVideo(s.thumbnailsPerVideo);
    emit searchRequested(std::move(s));
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
    QAction* selected = m.exec(QCursor::pos());
    if (!selected)
        return;

    if (selected == aSize)
        emit sortOptionChosen(Size);
}

void MainWindow::onSortGroupsClicked()
{
    QMenu m(this);
    QAction* aSize = m.addAction(tr("Sort Groups By Size"));
    QAction* selected = m.exec(QCursor::pos());
    if (!selected)
        return;

    if (selected == aSize)
        emit sortGroupsOptionChosen(Size);
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

//  Double‑click opens a video
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
void MainWindow::setCurrentDatabase(QString const& path)
{
    ui->currentDbLineEdit->setText(path);
    cfg::saveDatabasePath(path.toStdString()); // <── persist selection
}

SearchSettings MainWindow::collectSearchSettings() const
{
    SearchSettings s;
    s.useGlob = ui->globCheckBox->isChecked();
    s.caseInsensitive = ui->caseCheckBox->isChecked();

    // --- extensions ---
    QString extTxt = ui->extensionsEdit->text().trimmed();
    if (!extTxt.isEmpty()) {
        for (QString e : extTxt.split(',', Qt::SkipEmptyParts)) {
            e = e.trimmed().toLower();
            if (!e.startsWith('.'))
                e.prepend('.');
            s.extensions.push_back(e.toStdString());
        }
    }

    // --- regex / glob filters ---
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

    // --- size limits ---
    if (ui->minBytesSpin->value() > 0)
        s.minBytes = static_cast<std::uint64_t>(ui->minBytesSpin->value() * 1024 * 1024);
    if (ui->maxBytesSpin->value() > 0)
        s.maxBytes = static_cast<std::uint64_t>(ui->maxBytesSpin->value() * 1024 * 1024);

    // --- directory list & regex compilation ---
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

    // --- FFmpeg decoding ---
    s.thumbnailsPerVideo = std::clamp(ui->thumbnailsSpin->value(), 1, 4);

    // --- Fast / Slow selection ---
    bool fast = ui->hashMethodCombo->currentIndex() == 0;
    s.method = fast ? HashMethod::Fast : HashMethod::Slow;

    if (fast) {
        s.fastHash.maxFrames = ui->maxFramesSpinFast->value();
        s.fastHash.hammingDistance = ui->hammingDistanceThresholdSpinFast->value();
        s.fastHash.matchingThreshold = ui->matchingThresholdNumSpinBoxFast->value();
        s.fastHash.useKeyframesOnly = ui->keyframesOnlyCheckBoxFast->isChecked();
    } else {
        s.slowHash.maxFrames = ui->maxFramesSpin->value();
        s.slowHash.skipPercent = ui->skipSpin->value();
        s.slowHash.maxFrames = ui->maxFramesSpin->value();
        s.slowHash.hammingDistance = ui->hammingDistanceThresholdSpin->value();
        s.slowHash.usePercentThreshold = ui->percentThresholdRadio->isChecked();
        if (s.slowHash.usePercentThreshold)
            s.slowHash.matchingThresholdPct = ui->matchingThresholdPercentSpinBox->value();
        else
            s.slowHash.matchingThresholdNum = ui->matchingThresholdNumSpinBox->value();
    }

    compileAllRegexes(s);

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

// Persist current GUI values to DB
void MainWindow::saveCurrentSettings()
{
    try {
        m_db->saveSettings(collectSearchSettings());
    } catch (std::exception const& e) {
        spdlog::error("saveCurrentSettings failed: {}", e.what());
    }
}

// Populate GUI from SearchSettings structure
void MainWindow::applySearchSettings(SearchSettings const& s)
{
    // --- simple checkboxes ---
    ui->globCheckBox->setChecked(s.useGlob);
    ui->caseCheckBox->setChecked(s.caseInsensitive);

    // --- extensions line-edit ---
    QStringList extLst;
    for (auto const& e : s.extensions)
        extLst << QString::fromStdString(e);
    ui->extensionsEdit->setText(extLst.join(','));

    // --- pattern text-edits ---
    auto join = [](std::vector<std::string> const& v) {
        QStringList q;
        for (auto const& x : v)
            q << QString::fromStdString(x);
        return q.join('\n');
    };
    ui->includeFileEdit->setPlainText(join(s.includeFilePatterns));
    ui->includeDirEdit->setPlainText(join(s.includeDirPatterns));
    ui->excludeFileEdit->setPlainText(join(s.excludeFilePatterns));
    ui->excludeDirEdit->setPlainText(join(s.excludeDirPatterns));

    // --- size limits (MB) ---
    ui->minBytesSpin->setValue(s.minBytes ? *s.minBytes / (1024 * 1024) : 0);
    ui->maxBytesSpin->setValue(s.maxBytes ? *s.maxBytes / (1024 * 1024) : 0);

    // --- directory list widget ---
    ui->directoryListWidget->clear();
    for (auto const& d : s.directories) {
        auto* it = new QTreeWidgetItem(ui->directoryListWidget);
        it->setText(0, QString::fromStdString(d.path));
        it->setCheckState(1, d.recursive ? Qt::Checked : Qt::Unchecked);
    }
    if (ui->directoryListWidget->headerItem()->text(0).isEmpty()) {
        ui->directoryListWidget->setHeaderLabels({ tr("Directory"), tr("Recursive") });
        ui->directoryListWidget->setColumnCount(2);
        ui->directoryListWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        ui->directoryListWidget->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    }

    // --- thumbnails per video ---
    ui->thumbnailsSpin->setValue(s.thumbnailsPerVideo);

    // --- hash method selection ---
    bool fast = s.method == HashMethod::Fast;
    ui->hashMethodCombo->setCurrentIndex(fast ? 0 : 1);

    // --- fast-hash widgets ---
    ui->maxFramesSpinFast->setValue(s.fastHash.maxFrames);
    ui->hammingDistanceThresholdSpinFast->setValue(s.fastHash.hammingDistance);
    ui->matchingThresholdNumSpinBoxFast->setValue(s.fastHash.matchingThreshold);
    ui->keyframesOnlyCheckBoxFast->setChecked(s.fastHash.useKeyframesOnly);

    // --- slow-hash widgets ---
    ui->skipSpin->setValue(s.slowHash.skipPercent);
    ui->maxFramesSpin->setValue(s.slowHash.maxFrames);
    ui->hammingDistanceThresholdSpin->setValue(s.slowHash.hammingDistance);
    if (s.slowHash.usePercentThreshold) {
        ui->percentThresholdRadio->setChecked(true);
        ui->matchingThresholdPercentSpinBox->setValue(s.slowHash.matchingThresholdPct);
    } else {
        ui->fixedNumThresholdRadio->setChecked(true);
        ui->matchingThresholdNumSpinBox->setValue(s.slowHash.matchingThresholdNum);
    }
}
void MainWindow::onSearchSettingsLoaded(SearchSettings const& s)
{
    applySearchSettings(s);
}
