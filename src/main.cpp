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
    QObject::connect(&w, &MainWindow::searchTriggered, [&] {
        controller.runSearchAndDetection(w);
    });

    w.show();
    return app.exec();
}

