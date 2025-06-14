#include "ConfigManager.h"
#include "DatabaseManager.h"
#include "MainWindow.h"
#include "SearchSettings.h"
#include "VideoController.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMessageBox>
#include <spdlog/spdlog.h>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    // -- Qt meta types for signal/slot compatibility --
    qRegisterMetaType<MainWindow::DeleteOptions>("MainWindow::DeleteOptions");
    qRegisterMetaType<MainWindow::SelectOptions>("MainWindow::SelectOptions");
    qRegisterMetaType<MainWindow::SortOptions>("MainWindow::SortOptions");
    qRegisterMetaType<SearchSettings>("SearchSettings");

    // --- determine which database to open ---
    std::string dbPath = cfg::loadDatabasePath().value_or(cfg::defaultDatabasePath());

    DatabaseManager db(dbPath);
    VideoController controller(db);
    MainWindow w(&db);
    controller.setModel(w.model());

    bool ascendingSort = true;
    bool ascendingGroupSort = true;

    QObject::connect(&controller,
        &VideoController::duplicateGroupsUpdated,
        &w,
        &MainWindow::onDuplicateGroupsUpdated);

    QObject::connect(&w, &MainWindow::databaseLoadRequested,
        &controller, &VideoController::loadDatabase);
    QObject::connect(&w, &MainWindow::databaseCreateRequested,
        &controller, &VideoController::createDatabase);
    QObject::connect(&controller, &VideoController::databaseOpened,
        &w, &MainWindow::setCurrentDatabase);

    w.setCurrentDatabase(QString::fromStdString(dbPath));

    QObject::connect(&w, &MainWindow::searchRequested, [&controller](SearchSettings cfg) {
        controller.setSearchSettings(cfg);
        controller.startSearch();
    });

    QObject::connect(&w, &MainWindow::selectOptionChosen,
        [&](MainWindow::SelectOptions option) {
            controller.handleSelectOption(option);
        });

    QObject::connect(&w, &MainWindow::sortOptionChosen,
        [&](MainWindow::SortOptions option) {
            controller.handleSortOption(option, ascendingSort);
            ascendingSort = !ascendingSort;
        });

    QObject::connect(&w, &MainWindow::sortGroupsOptionChosen,
        [&](MainWindow::SortOptions option) {
            controller.handleSortGroupsOption(option, ascendingGroupSort);
            ascendingGroupSort = !ascendingGroupSort;
        });

    QObject::connect(&w, &MainWindow::deleteOptionChosen,
        [&](MainWindow::DeleteOptions option) {
            controller.handleDeleteOption(option);
        });

    QObject::connect(&w, &MainWindow::hardlinkTriggered,
        &controller, &VideoController::handleHardlink);

    QObject::connect(&w, &MainWindow::addDirectoryRequested,
        &controller, &VideoController::onAddDirectoryRequested);

    QObject::connect(&w, &MainWindow::removeSelectedDirectoriesRequested,
        &controller, &VideoController::onRemoveSelectedDirectoriesRequested);

    QObject::connect(&controller, &VideoController::directoryListUpdated,
        &w, &MainWindow::onDirectoryListUpdated);

    QObject::connect(&controller, &VideoController::errorOccurred,
        [&](QString msg) {
            QMessageBox::critical(&w, "Error", msg);
        });

    QObject::connect(&controller, &VideoController::searchSettingsLoaded,
        &w, &MainWindow::onSearchSettingsLoaded);

    w.show();
    return app.exec();
}
