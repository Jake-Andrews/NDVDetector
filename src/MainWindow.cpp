#include "MainWindow.h"
#include "ui_MainWindow.h"
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QTableWidgetItem>
#include <qnamespace.h>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // Size of screenshots
    ui->tableWidget->setIconSize(QSize(128, 128));

    // Make table columns resize with window
    ui->tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    ui->tableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setDuplicateVideoGroups(std::vector<std::vector<VideoInfo>> const& groups)
{
    auto* table = ui->tableWidget;
    table->setRowCount(0);
    table->setWordWrap(true);
    table->verticalHeader()->setDefaultSectionSize(60);

    // placeholder image
    QPixmap placeholder("sneed.png");
    if (placeholder.isNull()) {
        qWarning("Could not load sneed.png, check path!");
    }

    int totalRows = 0;
    for (auto const& grp : groups) {
        totalRows += static_cast<int>(grp.size());
        totalRows += 1; // for the separator row
    }

    table->setRowCount(totalRows);

    int currentRow = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
        auto const& grp = groups[g];

        for (auto const& v : grp) {
            // Column 0: Screenshot placeholder
            QLabel* screenshotLabel = new QLabel;
            screenshotLabel->setPixmap(placeholder.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            screenshotLabel->setAlignment(Qt::AlignCenter);
            table->setCellWidget(currentRow, 0, screenshotLabel);

            // Column 1: path
            auto* pathItem = new QTableWidgetItem(QString::fromStdString(v.path));
            pathItem->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
            table->setItem(currentRow, 1, pathItem);

            // Column 2: Tech specs (size, bit_rate, resolution, framerate, duration)
            QString specs = QString("Size: %1 KB\nBitrate: %2 kb/s\nResolution: %3x%4\nFramerate: %5\nDuration: %6s")
                                .arg(v.size / 1024)
                                .arg(v.bit_rate / 1000)
                                .arg(v.width)
                                .arg(v.height)
                                .arg(v.avg_frame_rate, 0, 'f', 2)
                                .arg(v.duration);
            auto* specsItem = new QTableWidgetItem(specs);
            specsItem->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
            table->setItem(currentRow, 2, specsItem);

            // Column 3: Codecs (video + audio)
            QString codecInfo = QString("Video: %1\nAudio: %2")
                                    .arg(QString::fromStdString(v.video_codec))
                                    .arg(QString::fromStdString(v.audio_codec));
            auto* codecItem = new QTableWidgetItem(codecInfo);
            codecItem->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
            table->setItem(currentRow, 3, codecItem);

            // Column 4: #hard_links
            auto* linkItem = new QTableWidgetItem(QString::number(v.num_hard_links));
            linkItem->setTextAlignment(Qt::AlignCenter);
            table->setItem(currentRow, 4, linkItem);

            ++currentRow;
        }

        table->setSpan(currentRow, 0, 1, 5);
        table->setRowHeight(currentRow, 15);

        // Seperator Row
        auto* sepItem = new QTableWidgetItem;
        sepItem->setText(QString("== GROUP %1 (%2 videos) ==").arg(g + 1).arg(grp.size()));
        sepItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        table->setRowHeight(currentRow, 30);

        QFont font = sepItem->font();
        font.setBold(true);
        sepItem->setFont(font);
        sepItem->setBackground(Qt::lightGray);
        table->setItem(currentRow, 0, sepItem);

        ++currentRow;
    }

    // auto-adjust row sizes to fit multiline text
    table->resizeRowsToContents();
}
