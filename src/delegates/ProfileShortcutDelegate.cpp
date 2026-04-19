#include "ProfileShortcutDelegate.h"

#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <KKeySequenceWidget>

using namespace Konsole;


void ShortcutItemDelegate::editorModified(const QKeySequence& keys)
{
    Q_UNUSED(keys);
    //kDebug() << keys.toString();

    KKeySequenceWidget* editor = qobject_cast<KKeySequenceWidget*>(sender());
    Q_ASSERT(editor);
    _modifiedEditors.insert(editor);
    emit commitData(editor);
    emit closeEditor(editor);
}
void ShortcutItemDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
                                        const QModelIndex& index) const
{
    _itemsBeingEdited.remove(index);

    if (!_modifiedEditors.contains(editor))
        return;

    QString shortcut = qobject_cast<KKeySequenceWidget*>(editor)->keySequence().toString();
    model->setData(index, shortcut, Qt::DisplayRole);

    _modifiedEditors.remove(editor);
}

QWidget* ShortcutItemDelegate::createEditor(QWidget* aParent, const QStyleOptionViewItem&, const QModelIndex& index) const
{
    _itemsBeingEdited.insert(index);

    KKeySequenceWidget* editor = new KKeySequenceWidget(aParent);
    editor->setFocusPolicy(Qt::StrongFocus);
    editor->setModifierlessAllowed(false);
    QString shortcutString = index.data(Qt::DisplayRole).toString();
    editor->setKeySequence(QKeySequence::fromString(shortcutString));
    connect(editor, SIGNAL(keySequenceChanged(QKeySequence)), this, SLOT(editorModified(QKeySequence)));
    editor->captureKeySequence();
    return editor;
}

void ShortcutItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const
{
    if (_itemsBeingEdited.contains(index))
        StyledBackgroundPainter::drawBackground(painter, option, index);
    else
        QStyledItemDelegate::paint(painter, option, index);
}


void StyledBackgroundPainter::drawBackground(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex&)
{
    const QWidget* widget = option.widget;

    QStyle* style = widget ? widget->style() : QApplication::style();

    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter, widget);
}

