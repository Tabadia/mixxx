#include "library/online/onlinefeature.h"

#include "controllers/keyboard/keyboardeventfilter.h"
#include "library/online/dlgonlinesearch.h"
#include "moc_onlinefeature.cpp"
#include "widget/wlibrary.h"

namespace {

const QString kViewName = QStringLiteral("OnlineSearch");

} // anonymous namespace

OnlineFeature::OnlineFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("online")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
}

QVariant OnlineFeature::title() {
    return QVariant(tr("Online Search"));
}

TreeItemModel* OnlineFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void OnlineFeature::bindLibraryWidget(WLibrary* pLibraryWidget,
        KeyboardEventFilter* pKeyboard) {
    Q_UNUSED(pKeyboard);
    // The view is owned and deleted by the library widget.
    auto* pView = new DlgOnlineSearch(pLibraryWidget, m_pConfig, m_pLibrary);
    pLibraryWidget->registerView(kViewName, pView);
    connect(pView,
            &DlgOnlineSearch::loadTrack,
            this,
            &OnlineFeature::loadTrack);
    connect(pView,
            &DlgOnlineSearch::loadTrackToPlayer,
            this,
            &OnlineFeature::loadTrackToPlayer);
    connect(this,
            &OnlineFeature::focusSearch,
            pView,
            &DlgOnlineSearch::focusSearchBox);
}

void OnlineFeature::activate() {
    emit switchToView(kViewName);
    // The global library search box doesn't apply here; the view has its own.
    emit disableSearch();
    emit enableCoverArtDisplay(false);
    emit focusSearch();
}
