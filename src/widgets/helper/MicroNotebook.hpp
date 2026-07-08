#pragma once

#include <QHBoxLayout>
#include <QStackedLayout>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

namespace chatterino {

/// An extremely simple implementation of the regular notebook.
///
/// It's essentially a QTabWidget without the downside that the fusion style
/// provides poor contrast with it.
class MicroNotebook : public QWidget
{
public:
    MicroNotebook(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    int addPage(QWidget *page, QString name);

    void select(QWidget *page);

    bool isSelected(QWidget *page) const;

    void setShowHeader(bool showHeader);
    void onCurrentChanged(QObject *receiver, std::function<void()> callback);

private:
    struct Item {
        QString name;
        int index;
    };
    std::vector<Item> items;
    QStackedLayout layout;
    QHBoxLayout topBar;
    QWidget *topWidget = nullptr;
    QWidget *horizontalSeparator = nullptr;
};

}  // namespace chatterino
