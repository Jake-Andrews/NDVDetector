// GroupRowDelegate.cpp
#include "GroupRowDelegate.h"
#include "VideoModel.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QFont>
#include <QPainter>

GroupRowDelegate::GroupRowDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

void GroupRowDelegate::paint(QPainter* painter,
    QStyleOptionViewItem const& option,
    QModelIndex const& index) const
{
    auto model = index.model();
    auto const& rowEntry = static_cast<VideoModel const*>(model)->rowEntry(index.row());

    if (rowEntry.type != RowType::Separator) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // Only draw for column 0 to avoid multiple draws
    if (index.column() != 0)
        return;

    // Span across the entire row (all columns)
    int totalWidth = 0;
    for (int col = 0; col < model->columnCount(); ++col) {
        totalWidth += index.siblingAtColumn(col).data(Qt::SizeHintRole).toSize().width();
    }

    QRect fullRowRect(option.rect.topLeft(), QSize(option.widget->width(), option.rect.height()));

    // Background
    painter->fillRect(fullRowRect, QColor(Qt::lightGray));

    // Font
    QFont font = option.font;
    font.setBold(true);
    painter->setFont(font);

    // Text
    QString label = rowEntry.label;

    // Centered text
    painter->setPen(Qt::black);
    painter->drawText(fullRowRect, Qt::AlignCenter, label);
}

QSize GroupRowDelegate::sizeHint(QStyleOptionViewItem const& option,
    QModelIndex const& index) const
{
    auto model = index.model();
    auto const& rowEntry = static_cast<VideoModel const*>(model)->rowEntry(index.row());

    if (rowEntry.type == RowType::Separator) {
        QFontMetrics fm(option.font);
        return QSize(option.widget->width(), fm.height() + 10); // slightly padded
    }

    return QStyledItemDelegate::sizeHint(option, index);
}
