#pragma once

#include <QStyledItemDelegate>

class GroupRowDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit GroupRowDelegate(QObject* parent = nullptr);

    void setThumbnailsPerVideo(int n)          { m_thumbs = std::clamp(n,1,4); }
    int  thumbnailsPerVideo() const            { return m_thumbs; }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

    bool editorEvent(QEvent* event, QAbstractItemModel* model, 
                    const QStyleOptionViewItem& option, 
                    const QModelIndex& index) override;
private:
    int m_thumbs = 4;          // 1-4
};

