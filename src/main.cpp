#include "DatabaseManager.h"
#include "MainWindow.h"
#include "VideoController.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    DatabaseManager db("videos.db");
    VideoController controller(db);

    MainWindow w;
    controller.setModel(w.model());
    // Connect the controller's signal to the MainWindow slot
    QObject::connect(&controller,
        &VideoController::duplicateGroupsUpdated,
        &w,
        &MainWindow::onDuplicateGroupsUpdated);

    // Connect the MainWindow signals to the controller
    QObject::connect(&w, &MainWindow::searchTriggered, [&] {
        controller.runSearchAndDetection();
    });
    QObject::connect(&w, &MainWindow::selectOptionChosen,
        [&](QString option) { controller.handleSelectOption(option); });
    QObject::connect(&w, &MainWindow::sortOptionChosen,
        [&](QString option) { controller.handleSortOption(option); });
    QObject::connect(&w, &MainWindow::sortGroupsOptionChosen,
        [&](QString option) { controller.handleSortGroupsOption(option); });
    QObject::connect(&w, &MainWindow::deleteOptionChosen,
        [&](QString option) { controller.handleDeleteOption(option); });

    w.show();
    return app.exec();
}
