#include "GroupRowDelegate.h"
#include "VideoModel.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QTableView>
#include <algorithm>

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

    if (rowInfo.type == RowType::Separator) {
        if (index.column() != 0)
            return;

        auto* view = qobject_cast<QTableView const*>(option.widget);
        if (!view)
            return;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        QRect r = option.rect;
        r.setWidth(view->viewport()->width());

        QFont f = option.font;
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() + 1.5);
        painter->setFont(f);

        painter->setPen(pal.color(QPalette::HighlightedText));
        painter->drawText(r, Qt::AlignCenter, rowInfo.label);

        painter->restore();
        return;
    }

    QStyledItemDelegate::paint(painter, option, index);

    bool firstInGroup = (index.row() > 0 && model->rowEntry(index.row() - 1).type == RowType::Separator);

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

    // special handling for separator rows
    if (rowInfo.type == RowType::Separator) {
        QFontMetrics fm(option.font);
        return { option.rect.width(), fm.height() + 16 };
    }

    // normal rows
    QSize sz = QStyledItemDelegate::sizeHint(option, index);

    // enlarge height for screenshot column so all thumbs fit
    if (index.column() == VideoModel::Col_Screenshot) {
        auto* tv = qobject_cast<QTableView const*>(option.widget);
        int cell = tv ? tv->iconSize().height() : 128; // single-thumb height
        sz.setHeight(cell);                            // always one row
    }
    return sz;
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
