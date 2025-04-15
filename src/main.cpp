#include "DatabaseManager.h"
#include "MainWindow.h"
#include "VideoController.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    qRegisterMetaType<MainWindow::DeleteOptions>("MainWindow::DeleteOptions");
    qRegisterMetaType<MainWindow::SelectOptions>("MainWindow::SelectOptions");
    qRegisterMetaType<MainWindow::SortOptions>("MainWindow::SortOptions");

    DatabaseManager db("videos.db");
    VideoController controller(db);

    MainWindow w;
    controller.setModel(w.model());

    bool ascendingSort = true;
    bool ascendingGroupSort = true;

    QObject::connect(&controller,
        &VideoController::duplicateGroupsUpdated,
        &w,
        &MainWindow::onDuplicateGroupsUpdated);

    QObject::connect(&w, &MainWindow::searchTriggered, [&] {
        controller.runSearchAndDetection();
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

    w.show();
    return app.exec();
}

