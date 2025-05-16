// CodecTestWorker.cpp  

#include "CodecTestWorker.h"
#include "DecodingFrames.h"

#include <QProcess>
#include <spdlog/spdlog.h>

CodecTestWorker::CodecTestWorker(QObject* parent)
    : QObject(parent)
{
}

// sequentially process every TestItem in m_tests
void CodecTestWorker::run()
{
    int done = 0;
    int total = m_tests.size();

    for (TestItem const& t : m_tests) {
        bool swOk = false, hwOk = false;

        // -- software path --
        try {
            auto swHashes = decode_and_hash_sw(t.path.toStdString(),
                /*skip_pct=*/0,
                /*duration_s=*/0,
                /*max_frames=*/10,
                /*on_progress=*/ {});
            swOk = !swHashes.empty();
        } catch (...) {
            spdlog::warn("[worker] SW decode/hash failed for '{}'",
                t.path.toStdString());
        }

        // -- hardware path --
        try {
            auto hwHashes = decode_and_hash_hw_gl(t.path.toStdString(),
                0, 0, 10,
                /*on_progress=*/ {});
            hwOk = !hwHashes.empty();
        } catch (...) {
            spdlog::warn("[worker] HW decode/hash failed for '{}'",
                t.path.toStdString());
        }

        emit result(t, hwOk, swOk);
        emit progress(++done, total);
    }
    emit finished();
}
