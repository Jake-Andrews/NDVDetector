// CodecTestWorker.h
#pragma once

#include <QObject>
#include <QVector>
#include <QString>


struct TestItem
{
    QString path;
    QString codec;
    QString pixFmt;
    QString profile;
    QString level;
    bool    hwOk = false;
    bool    swOk = false;
};

//  Background worker that decodes and hashes all samples 
//  handed to it from MainWindow
class CodecTestWorker : public QObject {
    Q_OBJECT

public:
    explicit CodecTestWorker(QObject* parent = nullptr);

    void setTests(QVector<TestItem> tests) { m_tests = std::move(tests); }

public slots:
    void run(); // called via queued-invoke

signals:
    void progress(int done, int total);
    void result(TestItem item, bool hwOk, bool swOk);
    void finished();

private:
    QVector<TestItem> m_tests;
};

