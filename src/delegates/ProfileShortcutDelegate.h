#ifndef PROFILESHORTCUTDELEGATE_H
#define PROFILESHORTCUTDELEGATE_H

#include <QStyledItemDelegate>
#include <QModelIndex>
#include <QSet>

class QWidget;
class QKeyEvent;
class QPainter;

namespace Konsole
{

class StyledBackgroundPainter
{
public:
    static void drawBackground(QPainter* painter, const QStyleOptionViewItem& option,
                               const QModelIndex& index);
};

class ShortcutItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit ShortcutItemDelegate(QObject* parent = nullptr);

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

private slots:
    void editorModified(const QKeySequence& keys);

private:
    mutable QSet<QWidget*> _modifiedEditors;
    mutable QSet<QModelIndex> _itemsBeingEdited;
};

};

#endif
