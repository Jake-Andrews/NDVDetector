#include "GroupRowDelegate.h"
#include "VideoModel.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QTableView>

GroupRowDelegate::GroupRowDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

void GroupRowDelegate::paint(QPainter* painter,
    QStyleOptionViewItem const& option,
    QModelIndex const& index) const
{
    auto const* model = static_cast<VideoModel const*>(index.model());
    auto const& rowInfo = model->rowEntry(index.row());
    auto const& pal = option.palette;

    // Separator rows
    if (rowInfo.type == RowType::Separator) {
        if (index.column() != 0)
            return;

        auto* view = qobject_cast<QTableView const*>(option.widget);
        if (!view)
            return;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        // full‑width header
        QRect r = option.rect;
        r.setWidth(view->viewport()->width());

        // bold, slightly larger font
        QFont f = option.font;
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() + 1.5);
        painter->setFont(f);

        // main text: palette's highlighted text color
        painter->setPen(pal.color(QPalette::HighlightedText));
        painter->drawText(r, Qt::AlignCenter, rowInfo.label);

        painter->restore();
        return;
    }

    // Normal rows:
    QStyledItemDelegate::paint(painter, option, index);

    bool firstInGroup = (index.row() > 0 && model->rowEntry(index.row() - 1).type == RowType::Separator);

    // divider above first row in a group
    if (firstInGroup) {
        painter->save();
        painter->setPen(QPen(pal.color(QPalette::Midlight), 1));
        painter->drawLine(option.rect.topLeft(), option.rect.topRight());
        painter->restore();
    }
}

QSize GroupRowDelegate::sizeHint(QStyleOptionViewItem const& option,
    QModelIndex const& index) const
{
    auto const* model = static_cast<VideoModel const*>(index.model());
    auto const& rowInfo = model->rowEntry(index.row());

    if (rowInfo.type == RowType::Separator) {
        // Make group headers a bit taller for better visual separation
        QFontMetrics fm(option.font);
        return QSize(option.rect.width(), fm.height() + 16);
    }

    return QStyledItemDelegate::sizeHint(option, index);
}

bool GroupRowDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
    QStyleOptionViewItem const& option,
    QModelIndex const& index)
{
    auto const* videoModel = static_cast<VideoModel const*>(model);
    auto const& rowInfo = videoModel->rowEntry(index.row());

    // For separator rows, consume click events to prevent selection
    if (rowInfo.type == RowType::Separator) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick) {
            return true; // Consume the event
        }
    }

    // For normal rows, use default behavior
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

