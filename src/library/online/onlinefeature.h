#pragma once

#include "library/libraryfeature.h"
#include "library/treeitemmodel.h"
#include "preferences/usersettings.h"
#include "util/parented_ptr.h"

class KeyboardEventFilter;
class WLibrary;

/// Sidebar entry "Online Search": search YouTube / SoundCloud through yt-dlp,
/// download the audio as MP3 and add it to the Mixxx library.
class OnlineFeature final : public LibraryFeature {
    Q_OBJECT
  public:
    OnlineFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~OnlineFeature() override = default;

    QVariant title() override;
    void bindLibraryWidget(WLibrary* pLibraryWidget,
            KeyboardEventFilter* pKeyboard) override;
    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;

  signals:
    void focusSearch();

  private:
    parented_ptr<TreeItemModel> m_pSidebarModel;
};
