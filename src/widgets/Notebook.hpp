// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"
#include "widgets/NotebookEnums.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QWidget>

#include <functional>
#include <span>
#include <vector>

namespace chatterino {

class Button;
class PixmapButton;
class SvgButton;
class Window;
class DrawnButton;
class NotebookFolderTab;
class NotebookTab;
class SplitContainer;
class Split;

using TabVisibilityFilter = std::function<bool(const NotebookTab *)>;

struct TabFolderState {
    QString id;
    QString title;
    bool expanded = true;
};

class Notebook : public BaseWidget
{
    Q_OBJECT

public:
    explicit Notebook(QWidget *parent);
    ~Notebook() override = default;

    NotebookTab *addPage(QWidget *page, QString title = QString(),
                         bool select = false);

    /**
     * @brief Adds a page to the Notebook at a given position.
     *
     * @param position if set to -1, adds the page to the end
     **/
    NotebookTab *addPageAt(QWidget *page, int position,
                           QString title = QString(), bool select = false,
                           QString folderId = QString());
    void removePage(QWidget *page);
    void duplicatePage(QWidget *page);
    void removeCurrentPage();

    /**
     * @brief Returns index of page in Notebook, or -1 if not found.
     **/
    int indexOf(QWidget *page) const;

    /**
     * @brief Returns the visible index of page in Notebook, or -1 if not found.
     * Given page should be visible according to the set TabVisibilityFilter.
     **/
    int visibleIndexOf(QWidget *page) const;

    /**
     * @brief Returns the number of visible tabs in Notebook. 
     **/
    int getVisibleTabCount() const;

    /**
     * @brief Selects the Notebook tab containing the given page.
     **/
    virtual void select(QWidget *page, bool focusPage = true);

    /**
     * @brief Selects the Notebook tab at the given index. Ignores whether tabs
     * are visible or not. 
     **/
    void selectIndex(int index, bool focusPage = true);

    /**
     * @brief Selects the index'th visible tab in the Notebook.
     * 
     * For example, selecting the 0th visible tab selects the first tab in this 
     * Notebook that is visible according to the TabVisibilityFilter. If no filter
     * is set, equivalent to Notebook::selectIndex.
     **/
    void selectVisibleIndex(int index, bool focusPage = true);

    /**
     * @brief Selects the next visible tab. Wraps to the start if required. 
     **/
    void selectNextTab(bool focusPage = true);

    /**
     * @brief Selects the previous visible tab. Wraps to the end if required. 
     **/
    void selectPreviousTab(bool focusPage = true);

    /**
     * @brief Selects the last visible tab. 
     **/
    void selectLastTab(bool focusPage = true);

    int getPageCount() const;
    QWidget *getPageAt(int index) const;
    int getSelectedIndex() const;
    QWidget *getSelectedPage() const;

    QWidget *tabAt(QPoint point, int &index, int maxWidth = 2000000000);
    bool isFolderHeaderAt(QPoint point) const;
    void rearrangePage(QWidget *page, int index);
    bool tryRearrangeBulkSelectedTabs(QWidget *draggedPage, int targetIndex);
    bool movePageIntoFolderAt(QWidget *page, QPoint point);
    void setFolderDropPreviewPage(QWidget *page);
    bool hasTabFolders() const;
    bool beginFloatingTabDrag(QWidget *page);
    void updateFloatingTabDrag(QWidget *page, QPoint point);
    bool finishFloatingTabDrag(QWidget *page, QPoint point);
    void cancelFloatingTabDrag(QWidget *page);
    void toggleBulkSelectedTab(NotebookTab *tab);
    void selectBulkRangeTo(NotebookTab *tab, bool keepExisting);
    void clearBulkSelectedTabs();
    void removeBulkSelectedTabs();
    int bulkSelectedTabCount() const;
    pajlada::Signals::NoArgSignal bulkSelectionChanged;

    bool getAllowUserTabManagement() const;
    void setAllowUserTabManagement(bool value);

    std::vector<TabFolderState> getTabFolderStates() const;
    QString folderIdOfPage(QWidget *page) const;
    void restoreTabFolder(const QString &id, const QString &title,
                          bool expanded);
    void setPageFolder(QWidget *page, const QString &folderId,
                       bool animated = true);
    virtual void createPageInFolder(const QString &folderId);
    void showAddFolderDialog();

    bool getShowAddButton() const;
    void setShowAddButton(bool value);

    void setTabLocation(NotebookTabLocation location);

    bool isNotebookLayoutLocked() const;
    virtual void setLockNotebookLayout(bool value);

    virtual void addNotebookActionsToMenu(QMenu *menu);

    // Update layout and tab visibility
    void refresh();

protected:
    bool getShowTabs() const;
    void setShowTabs(bool value);

    void scaleChangedEvent(float scale_) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *) override;

    DrawnButton *addButton_;

    template <typename T>
    T *addCustomButton(auto &&...args)
    {
        auto *btn = new T(std::forward<decltype(args)>(args)..., this);
        this->customButtons_.push_back(btn);

        return btn;
    }

    struct Item {
        NotebookTab *tab{};
        QWidget *page{};
        QWidget *selectedWidget{};
        QString folderId;
    };

    struct Folder {
        QString id;
        QString title;
        bool expanded = true;
        NotebookFolderTab *tab{};
    };

    struct LayoutEntry {
        Item *item{};
        Folder *folder{};
    };

    const QList<Item> items()
    {
        return this->items_;
    }

    /**
     * @brief Apply the given tab visibility filter
     *
     * An empty function can be provided to denote that no filter will be applied
     *
     * Tabs will be redrawn after this function is called.
     **/
    void setTabVisibilityFilter(TabVisibilityFilter filter);

    /**
     * @brief shouldShowTab has the final say whether a tab should be visible right now.
     **/
    bool shouldShowTab(const NotebookTab *tab) const;

    void performLayout(bool animate = false);

    void sortTabsAlphabetically();

private:
    struct LayoutContext {
        int left = 0;
        int right = 0;
        int bottom = 0;
        float scale = 0;
        int tabHeight = 0;
        int minimumTabAreaSpace = 0;
        int addButtonWidth = 0;
        int lineThickness = 0;
        int tabSpacer = 0;

        int buttonWidth = 0;
        int buttonHeight = 0;

        std::span<LayoutEntry> entries;
    };

    void performHorizontalLayout(const LayoutContext &ctx, bool animated);
    void performVerticalLayout(const LayoutContext &ctx, bool animated);
    void setBulkSelectedTab(NotebookTab *tab, bool value);
    NotebookTab *bulkRangeAnchor();
    void updateSelectedTabVisualState();

    /**
     * @brief Show a popup informing the user of some big tab visibility changes
     **/
    void showTabVisibilityInfoPopup();

    /**
     * @brief Updates the visibility state of all tabs
     **/
    void updateTabVisibility();
    void resizeAddButton();

    bool containsPage(QWidget *page);
    Item *findItem(QWidget *page);
    const Item *findItem(QWidget *page) const;
    Folder *findFolder(const QString &folderId);
    const Folder *findFolder(const QString &folderId) const;
    Folder *ensureFolder(const QString &folderId, const QString &title,
                         bool expanded);
    Folder *createFolder(const QString &title, const QString &id,
                         bool expanded, bool queueSave);
    void showRenameFolderDialog(const QString &folderId);
    void showFolderContextMenu(const QString &folderId, QPoint globalPos);
    void toggleFolderExpanded(const QString &folderId);
    void removeFolder(const QString &folderId);
    bool folderContainsSelectedPage(const QString &folderId) const;
    bool folderContainsBulkSelectedPage(const QString &folderId) const;
    int folderPageCount(const QString &folderId) const;
    QString nextFolderTitle() const;
    bool isFolderDropPreviewItem(const Item &item) const;
    bool isItemVisible(const Item &item) const;
    std::vector<LayoutEntry> buildVisibleLayoutEntries();
    void updateFolderGroupRects();
    void updateFloatingTabDropPreview(QPoint point);
    void clearFloatingTabDropPreview();
    void movePageToPreviewIndex(QWidget *page, int insertionIndex,
                                const QString &folderId);
    int lastIndexInFolder(const QString &folderId) const;

    static bool containsChild(const QObject *obj, const QObject *child);
    NotebookTab *getTabFromPage(QWidget *page);

    // Returns the number of buttons in `customButtons_` that are visible
    size_t visibleButtonCount() const;

    QList<Item> items_;
    std::vector<Folder> folders_;
    std::vector<QRect> folderGroupRects_;
    QMenu *menu_ = nullptr;
    QWidget *selectedPage_ = nullptr;
    QWidget *folderDropPreviewPage_ = nullptr;
    QRect tabDropPreviewRect_;
    QRect folderDropTargetRect_;
    int tabDropPreviewIndex_ = -1;
    QString tabDropPreviewFolderId_;

    std::vector<Button *> customButtons_;
    std::vector<NotebookTab *> bulkSelectedTabs_;
    NotebookTab *bulkSelectionAnchorTab_ = nullptr;

    bool allowUserTabManagement_ = false;
    bool showTabs_ = true;
    bool showAddButton_ = false;
    int lineOffset_ = 20;
    bool lockNotebookLayout_ = false;

    bool refreshPaused_ = false;
    bool refreshRequested_ = false;

    NotebookTabLocation tabLocation_ = NotebookTabLocation::Top;

    QAction *lockNotebookLayoutAction_;
    QAction *toggleTopMostAction_;
    QAction *addTabFolderAction_;

    // This filter, if set, is used to figure out the visibility of
    // the tabs in this notebook.
    TabVisibilityFilter tabVisibilityFilter_;

    friend class NotebookFolderTab;
};

class SplitNotebook : public Notebook
{
public:
    SplitNotebook(Window *parent);

    SplitContainer *addPage(bool select = false);
    SplitContainer *addPageInFolder(const QString &folderId,
                                    bool select = false);
    SplitContainer *getOrAddSelectedPage();
    /// Returns `nullptr` when no page is selected.
    SplitContainer *getSelectedPage();
    void select(QWidget *page, bool focusPage = true) override;
    void themeChangedEvent() override;

    void addNotebookActionsToMenu(QMenu *menu) override;
    void createPageInFolder(const QString &folderId) override;

    void forEachSplit(const std::function<void(Split *)> &cb);

    /**
     * Toggles between the "Show all tabs" and "Hide all tabs" tab visibility states
     */
    void toggleTabVisibility();

    QAction *showAllTabsAction;
    QAction *onlyShowLiveTabsAction;
    QAction *hideAllTabsAction;

protected:
    void showEvent(QShowEvent *event) override;

private:
    QAction *sortTabsAlphabeticallyAction_;

    void addCustomButtons();
    void addBulkSelectionButton();
    void updateBulkSelectionButton();

    pajlada::Signals::SignalHolder signalHolder_;

    // Main window on Windows has basically a duplicate of this in Window
    SvgButton *accountButton_{};
    SvgButton *bulkClearButton_{};
    SvgButton *bulkDeleteButton_{};
    PixmapButton *streamerModeIcon_{};
    void updateAccountButtonIcon();
    void updateStreamerModeIcon();

    void setLockNotebookLayout(bool value) override;
};

}  // namespace chatterino
