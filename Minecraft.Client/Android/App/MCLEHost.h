#pragma once

// The menu host: the iOS port's MinecraftViewController, with what UIKit
// did for it left to the app shell. One of it lives for the whole run and
// everything on it is called on the frame thread.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "MCLEHostSupport.h"
#include "../../../ios/Minecraft.Client/iOS/Game/MCLEKeyboard.h"

// Result codes track C4JStorage::EMessageResult used in every console
// dialog callback signature. ControlId 0..3 from the MessageBox scene's
// handlePress map to Accept, Decline, Third, Fourth respectively
// (UIScene_MessageBox.cpp:120-141). -1 is Cancelled (B press, matches
// EMessage_Cancelled).
enum MCLEDialogResult {
    MCLEDialogResultCancelled = -1,
    MCLEDialogResultAccept = 0,
    MCLEDialogResultDecline = 1,
    MCLEDialogResultThird = 2,
    MCLEDialogResultFourth = 3,
};
typedef std::function<void(MCLEDialogResult result)> MCLEDialogCallback;

// A button of the menu that's up: its instance name in the movie, its
// text and the console's eControl_* id
struct MenuButton {
    std::string name;
    std::string label;
    int id = 0;
};

// A scene the player came through, and the button that had the focus there
struct MenuStackEntry {
    std::string swf;
    int focus = 0;
    // UIScene::m_iFocusControl and m_iFocusChild as the scene was left
    int focusControl = -1;
    int focusChild = -1;
};

struct Host {
    std::string loadedSwfName;
    std::string fontStatus;
    // last seen 4J button bitmask, for edge detection
    uint32_t lastPadButtons = 0;
    // index into menuButtonConfig
    int menuFocusIndex = 0;
    // "MainMenu1080.swf", "HelpAndOptionsMenu1080.swf", etc.
    std::string currentMenuSwf;
    // Configured per-menu by the initXxx methods so the focus state machine
    // can drive any menu uniformly
    std::vector<MenuButton> menuButtonConfig;
    // The panorama, logo and tooltips are attached to the stage once and
    // stay across scene changes
    bool scenerAttached = false;
    // The yellow splash next to the logo and all the lines of splashes.txt
    std::string splashText;
    std::vector<std::string> splashes;
    // Oldest at index 0. B-back restores the scene and its focus.
    std::vector<MenuStackEntry> menuStack;

    std::string resolveSwfPath();
    static bool installPlatformSkinAliasIfNeeded();
    void platformPadChanged();
    void reloadTooltipsComponent();
    void reloadTutorialPopupComponent();
    void reloadSkin();
    void viewDidLoad();
    void pickSplash();
    void viewWillAppear();
    void viewDidDisappear();
    void applyMenuButtonConfig(const std::vector<MenuButton>& config);
    void attachMenuScenery();
    void applyLogoVisibilityForScene(const std::string& swfName);
    void seedTooltipsForScene_hidden(const std::string& swfName, bool hidden);
    void initMainMenuButtons();
    void initIntro();
    void introNavigate();
    void initSaveMessage();
    void navigateToHomeMenu();
    void initHelpAndOptionsButtons();
    void refreshFocusState();
    void initSettingsOptionsMenu();
    void initSettingsAudioMenu();
    void initSettingsControlMenu();
    void initSettingsGraphicsMenu();
    void initSettingsUIMenu();
    void loadOrJoinAddSaves();
    void initLoadOrJoinMenu();
    void loadOrJoinSaveOptions();
    void loadOrJoinSaveOptionsReturned(MCLEDialogResult result);
    void loadOrJoinDeleteSaveDialogReturned(MCLEDialogResult result);
    void loadOrJoinDeleteSaveDataReturned(bool bRes);
    void loadOrJoinRepopulate();
    void initLoadMenu();
    void loadMenuSlider_value(int sliderId, int value);
    void loadMenuPress_child(int controlId, int childId);
    void startGameSelectionChanged(int selectedId);
    bool startGameTexturePackMissing();
    void loadMenuStartSharedLaunchFlow();
    void loadMenuLaunchGame();
    void startLoadingSelectedSave();
    void initCreateWorldMenu();
    void createWorldSlider_value(int sliderId, int value);
    void createWorldPress_child(int controlId, int childId);
    void createWorldStartSharedLaunchFlow();
    void createWorldCreateGame();
    void initMoreOptionsMenu();
    void moreOptionsCheckbox_checked(int controlId, bool selected);
    void moreOptionsFocus(int controlId);
    void moreOptionsSlider_value(int sliderId, int value);
    void moreOptionsPress(int controlId);
    void initFullscreenProgress();
    void openPauseMenu();
    void initPauseMenu();
    void openInventoryMenu();
    void openCraftingMenu(bool table);
    void showContainerOverWorld();
    void openSignEntryMenu();
    void initSignEntryMenu();
    void openInGameInfoMenu();
    void initInGameInfoMenu();
    void initInGameOptionsMenu();
    void initKeyboardMenu();
    void navigateToKeyboard(const MCLEKeyboardInitData& kbData);
    void closeFrontEndKeyboard();
    void frontEndKeyboardFrame();
    void containerNavigateTo(const std::string& movie);
    void containerNavigateBack();
    void initCraftingMenu(bool table);
    void openServerContainer();
    void initServerContainer();
    void openCreativeMenu();
    void initCreativeMenu();
    void initInventoryMenu();
    void closeContainerMenu(bool backToGame);
    void containerMenuFrame();
    void openDeathMenu();
    void initDeathMenu();
    void openConnectingProgress_respawn(int stringId, bool respawn);
    void closeConnectingProgress();
    void openSaveProgress(bool autosave);
    void saveProgressTick();
    void openEndPoem();
    void initEndPoem();
    void resumeFromPause();
    void enterWorldUI();
    void pollWorldLoad();
    void requestExitGame();
    void exitGameSaveDialogReturned(MCLEDialogResult r);
    void exitGameConfirmReturned_saving(MCLEDialogResult r, bool save);
    void exitWorldSaving(bool save);
    void exitWorldAfterThumbnailSaving(bool save);
    void finishExitWorld();
    void initHowToPlayMenu();
    void initSettingsMenuButtons();
    void initLeaderboardMenu();
    void initDLCMainMenu();
    void initSkinSelectMenu();
    void closeSkinSelectMenu();
    void skinSelectMenuFrame();
    void presentDialogWithTitle_content_buttons_focusIndex_callback(const std::string& title, const std::string& content, const std::vector<std::string>& buttonLabels, int focus, MCLEDialogCallback callback);
    void populateMessageBoxSibling();
    void finishDialogWithResult(MCLEDialogResult result);
    void navigateForwardTo(const std::string& swfName);
    void navigateBack();
    void transitionToMenuNamed(const std::string& swfName);
    void tick();
};

// The one host
extern Host g_host;
