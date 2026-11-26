#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "app_prefs.hpp"
#include "live_controller.hpp"
#include "muuk/ctrader_fix/gemini_client.hpp"
#include "muuk/ctrader_fix/live_config.hpp"
#include "updater.hpp"
#include "version.hpp"
#include "system_monitor.hpp"
#include "muuk/ctrader_fix/volume_stats.hpp"

struct ImFont;

namespace muuk::desktop {

// The whole app window: header (connection), live KPI cards, a range panel
// (live range chart + Open/Close range + auto-continue + emergency
// controls), a settings panel for tuning the strategy, and tabs listing
// open positions, pending orders and the log -- each position/order with
// its own Close/Cancel button. Drawn in a frosted-glass style (glass_ui.hpp).
// Holds only UI-local state (text buffers, the settings being edited,
// theme); all live state comes from LiveController every frame.
// Assistant actions (v0.4.0 design Section 4.6) and the single enforcement
// point: every assistant-initiated write goes through
// assistantActionAllowed() before it is dispatched, so hiding a button is
// never the only protection. Owner may do everything except change keys or
// its own role (the assistant can never change keys, and it can never
// change its own role); User may only Explain and RefreshBrain. (The doc's
// Section 4.6 also carries a note saying a non-owner cannot run
// RefreshBrain either; the explicit allowed-list -- User: Explain +
// RefreshBrain -- is what is implemented, and the contradiction is flagged
// in the v0.4.0 report.)
enum class AssistantAction {
    Explain,
    ChangeDailyLossPercent,
    ChangeStopLoss,
    ChangeErMode,
    ChangeRiskAppetite,
    ChangeMaxLot,
    TrainBrain,
    RefreshBrain,
    ChangeKeys,
    ChangeRole
};
[[nodiscard]] bool assistantActionAllowed(muuk::ctrader_fix::AssistantRole role, AssistantAction action);

class DashboardWindow {
public:
    // Opens the way it was left (app_prefs.hpp): the last config file and
    // theme; falls back to findDefaultConfigPath() (config_discovery.hpp).
    DashboardWindow();

    // "Connect when the app opens" (saved preference) -- main.cpp acts on it.
    [[nodiscard]] bool autoConnect() const { return prefs_.autoConnect; }

    // Optional extra fonts (loaded by main.cpp); nullptr falls back to the
    // default font.
    void setFonts(ImFont* bold, ImFont* large, ImFont* numbers);

    // Called whenever the operator switches dark/light, e.g. so main.cpp
    // can match the native window title bar.
    void setThemeChangedCallback(std::function<void(bool dark)> callback);

    // Overrides the config path found by findDefaultConfigPath() (e.g.
    // from the command line).
    void setConfigPath(const std::string& path);
    // Shown in the Performance tab (e.g. "Direct3D 11 software (no GPU)").
    // A software renderer forces Lite mode on.
    void setRenderer(std::string name, bool software) {
        rendererName_ = std::move(name);
        softwareRenderer_ = software;
    }
    // Lite mode: flat look, no animations, and main.cpp redraws only when
    // something happens (1 frame/s idle). Always on without a GPU.
    [[nodiscard]] bool liteMode() const { return prefs_.liteMode || softwareRenderer_; }
    // For frames that aren't drawn (window hidden): keeps auto-reconnect going.
    void tickWithoutDrawing(LiveController& controller);
    [[nodiscard]] std::string configPath() const { return configPathBuffer_.data(); }

    void render(LiveController& controller);

    // Exit guard. main.cpp calls requestExit() instead of closing when the
    // window's close button is pressed: with nothing open at the broker the
    // exit is approved straight away; otherwise a dialog asks whether to
    // close everything first (and waits until the account is flat), leave
    // the positions open, or stay. The app quits once exitApproved().
    void requestExit(LiveController& controller);
    [[nodiscard]] bool exitApproved() const { return exitApproved_; }
    // Opens on this tab ("positions", "history", "audit", ...; --tab on the command line).
    void selectTabByName(const std::string& name);

private:
    enum class Confirm { None, CloseRange, CloseAllPositions, CancelAllPending, ResetData, PlaceNow, RestartMt5 };
    // The tabs on the right; a KPI card click selects one.
    enum class Tab { None, Positions, Orders, History, Risk, News, Audit, Volume, Performance, Brain, Assistant, Settings, Log };
    // Risk & capital page: account cards, lots, loss limits, guards, lock/escape, the risk table and the
    // reverse-trade calculator.
    void renderRiskPage(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                        bool connected);
    // Save & apply / Revert for the edited settings (Settings and Risk pages).
    void renderSaveBar(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                       bool connected);
    // The left menu (one row per page) and the page it shows.
    void renderPages(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                     bool connected);

    void setTheme(bool dark);
    void renderHeader(LiveController& controller, bool running, bool connected);
    // Self-update button (row 2, next to Connect/Disconnect): shown when a
    // newer release exists; the download + install + restart start from it.
    void renderUpdateButton();
    void renderKpiCards(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap, bool connected);
    void renderRangePanel(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                          bool connected);
    void renderSettingsPanel(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                             bool connected);
    // The Brain tab: live status, risk appetite, hard caps, the ER gate,
    // trainer history, smart ranges -- the bot's core settings.
    void renderBrainTab(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                        bool connected);
    // v0.4.0 Section 2: the 81-cell brain grid, trainer card, layers and
    // smart switch (all display-only -- no brain decision, gate or cap
    // changes). v0.4.0 Section 4: the Assistant tab (Gemini chat).
    void renderBrainGrid(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void renderTrainerCard(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void renderBrainLayers();
    void renderSmartSwitch(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void renderAssistantTab(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                            bool connected);
    // Assistant context (Section 4.4 allowlist only -- never keys, account
    // numbers, balances or credentials) and the background Gemini call:
    // sendAssistantMessage starts it on a worker thread, pollAssistantReply
    // collects the result without ever blocking the render thread.
    [[nodiscard]] std::string buildAssistantContext(
        const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap) const;
    void sendAssistantMessage(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void pollAssistantReply();
    // One confirmed assistant write, logged to <config>.assistant_log.jsonl
    // beside the account config (same folder pattern as the brain journal).
    void appendAssistantLog(const std::string& field, double oldValue, double newValue);
    [[nodiscard]] muuk::ctrader_fix::AssistantRole assistantRole(
        const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap) const;
    void renderPositionsTab(LiveController& controller, const muuk::ctrader_fix::LiveSnapshot& snap, bool connected);
    void renderOrdersTab(LiveController& controller, const muuk::ctrader_fix::LiveSnapshot& snap, bool connected);
    void renderLogTab(LiveController& controller);
    void renderHistoryTab(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void renderSpeed(const muuk::ctrader_fix::LiveSnapshot& snap);
    void renderVolumeTab();
    // `price` = the live mid (0 when not connected), for the commission example.
    void renderLotPreview(const muuk::ctrader_fix::LiveSettings& settings, double price);
    void renderAccountSection(bool running);
    // Settings > This PC: start with Windows, auto-connect, no sleep while running.
    void renderThisPcSection();
    // MT5 accounts: every account the bridge has been on is remembered (no
    // passwords); the list lets the operator pick the one to trade, and MT5
    // is switched to it.
    void rememberMt5Account(LiveController& controller);
    void pushMt5ServerHints(LiveController& controller);
    void renderMt5AccountsPopup(LiveController& controller);
    // Saves `login`/`server` (and the account type for a known mode) as this
    // config's MT5 account and switches MT5 to it; a password typed here goes
    // with it, else MT5's own saved one is used.
    void useMt5Account(LiveController& controller, const std::string& login, const std::string& server,
                       const std::string& mode, const std::string& password);
    void loadAccountForm(const std::string& configPath);
    [[nodiscard]] muuk::ctrader_fix::AccountSettings accountFromForm() const;
    void renderPerformanceTab(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void samplePerformance(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void refreshVolume();
    void renderConfirmPopup(LiveController& controller);
    void renderExitPopup(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void flash(const std::string& message);

    // Market hours / news (see MarketHoursConfig): the status strip in the
    // header, the News tab, and their part of Settings.
    void renderMarketStrip(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                           bool connected);
    void renderNewsTab(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                       bool connected);
    void renderMarketSettings();
    // Popups opened from the Brain tab (API keys) and from the News tab /
    // Settings (market hours & news). Each carries its own SaveBar.
    void renderKeysPopup(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                         bool connected);
    void renderMarketHoursPopup(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                                bool connected);
    // Risk & capital: the account cards, lot sizing, loss limits, break-even,
    // margin guards, escape, and the level-by-level worst case (muuk::core::riskTable).
    void renderRiskCalculator(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    // Balance, equity, margin, free margin and margin level: from MT5 when it
    // reports them, else the typed-in balance and the bot's estimate (cTrader).
    void renderAccountCards(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    // Audit: every self-check with its state, and what it found / did.
    void renderAuditTab(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap, bool connected);
    // Reverse-trade calculator (muuk::core::reverseTrade) under the risk table.
    void renderReverseCalculator(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                                 const std::vector<muuk::core::RiskRow>& rows);
    int reverseLevel_ = 0;        // 0 = the open positions, else that level's worst case
    double reverseMove_ = 2.0;    // $; 0 = lock only
    void renderSessionStats();
    // The net P/L bar with its target, and the gear that sets the auto target.
    void renderTargetBar(LiveController& controller, const muuk::ctrader_fix::LiveSnapshot& snap, bool connected);
    // The market as the bot sees it (connected), or worked out here from
    // the settings being edited (offline) -- refreshed once a second.
    [[nodiscard]] muuk::ctrader_fix::MarketStatus currentMarket(LiveController& controller,
                                                                 const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                                                                 bool connected);
    // A one-field change applied at once: to the running bot, the config
    // file and the settings editor (other unsaved edits are kept).
    void quickApply(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                    bool connected, const std::function<void(muuk::ctrader_fix::LiveSettings&)>& change);

    std::array<char, 512> configPathBuffer_{};
    // API key inputs (Settings > Keys): the full key lives in
    // editSettings_.keys; these buffers are what the masked fields edit.
    // API keys are ~40 chars at most; 256 leaves room.
    std::array<char, 256> geminiKeyBuffer_{};
    std::array<char, 256> fredKeyBuffer_{};
    std::array<char, 256> finnhubKeyBuffer_{};
    std::array<char, 512> trainerHistoryBuffer_{};  // Step 5: history CSV/folder for Train (paths run long)

    // Brain grid display state (v0.4.0 Section 2): 0 mean PnL, 1 score,
    // 2 lock rate, 3 samples; -1 = no cell pinned.
    int brainGridMode_ = 0;
    bool brainTradableOnly_ = false;
    int brainPinnedCell_ = -1;
    // Assistant tab (v0.4.0 Section 4): chat history lives only for this
    // session (never persisted, so stale context is never re-sent).
    struct AssistantMessage {
        bool fromUser = false;
        std::string text;
    };
    std::vector<AssistantMessage> assistantMessages_;
    std::array<char, 512> assistantInput_{};
    std::future<muuk::ctrader_fix::GeminiResponse> assistantFuture_;
    bool assistantWaiting_ = false;
    double assistantSentAt_ = -1000.0;  // ImGui::GetTime() of the last send (5 s cooldown)
    std::string assistantStatus_;       // last error / refusal line shown in the tab
    std::string assistantCacheKey_;     // question + context of the cached reply
    std::string assistantCachedReply_;
    double assistantCacheAt_ = -1000.0;  // 5-minute cache (Section 4.4)
    std::string assistantPendingCacheKey_;  // cache key of the in-flight question
    std::size_t assistantShown_ = 0;        // messages rendered (drives chat auto-scroll)
    // A change proposed in the Assistant tab: shown in a confirmation card
    // (old -> new) and applied only on Confirm.
    int assistantChangeField_ = 0;  // 0 daily loss %, 1 stop loss, 2 max lot, 3 ER mode
    double assistantChangeValue_ = 2.0;
    bool assistantChangePending_ = false;
    AssistantAction assistantChangeAction_ = AssistantAction::Explain;
    std::string assistantChangeFieldName_;
    double assistantChangeOld_ = 0.0;
    double assistantChangeNew_ = 0.0;
    AppPrefs prefs_;
    bool darkMode_ = true;
    std::string dismissedResumeNote_;
    std::function<void(bool)> onThemeChanged_;

    muuk::ctrader_fix::LiveSettings editSettings_;
    bool settingsLoaded_ = false;  // editSettings_ holds real values (from the file or the bot)
    bool settingsDirty_ = false;   // edited in the UI but not yet applied/saved

    std::array<char, 128> logFilter_{};
    bool logAutoScroll_ = true;

    // Cached copy of controller.history(), refreshed when its revision changes.
    std::vector<muuk::ctrader_fix::BasketRecord> history_;
    uint64_t historyRevision_ = ~uint64_t{0};  // forces the first copy
    std::string historyPath_;  // config path the history was loaded for
    std::size_t historyShown_ = 500;  // baskets listed in the History tab (Show more adds 500)

    // Volume tab, recomputed when the history changes (or the day might have).
    int volumePeriod_ = 0;  // 0 day, 1 week, 2 month
    uint64_t volumeRevision_ = ~uint64_t{0};
    double volumeComputedAt_ = -1000.0;
    std::vector<muuk::ctrader_fix::VolumeBucket> volumeRows_[3];
    muuk::ctrader_fix::VolumeBucket volumeTotal_;
    std::string nowDay_, nowWeek_, nowMonth_;

    bool exitApproved_ = false;
    bool openExitRequested_ = false;

    // Self-update (docs/updater.md): one background GitHub check per app
    // run; the button appears when a newer release exists.
    updater::Updater updater_{kMuukVersion};
    bool updateCheckStarted_ = false;
    bool updateRestartDone_ = false;  // installed and waiting for us to quit
    bool exitClosing_ = false;      // "Close everything & exit" in progress
    double exitClosingSince_ = 0.0;

    Tab selectTab_ = Tab::None;  // switch to this tab on the next frame
    Tab currentTab_ = Tab::Positions;
    bool menuOpen_ = false;        // the menu drawer is out (mouse over the icon / drawer)
    bool menuSuppressed_ = false;  // a page was just picked: stay shut until the mouse leaves

    // Market hours / news, offline view (see currentMarket).
    muuk::ctrader_fix::MarketStatus uiMarket_;
    double uiMarketAt_ = -1000.0;
    double uiMarketFullAt_ = -1000.0;
    std::string uiMarketKey_;       // settings the cached status was worked out for
    bool newsStarted_ = false;
    struct SessionRow {
        std::string name;
        int baskets = 0;
        int won = 0;
        int lost = 0;
        double net = 0.0;
        double commission = 0.0;
    };
    std::vector<SessionRow> sessionRows_;  // History > By trading session
    ImVec2 targetPopupPos_{0.0f, 0.0f};    // where the gear's popup opens
    ImVec2 targetPopupPivot_{0.0f, 0.0f};
    uint64_t sessionRowsRev_ = ~uint64_t{0};
    bool newsUsdOnly_ = true;
    bool newsHideLow_ = true;
    std::array<char, 32> newsWhen_{};
    std::array<char, 96> newsTitle_{};

    // Performance tab.
    SystemMonitor monitor_;
    std::string rendererName_;
    bool softwareRenderer_ = false;
    double perfSampledAt_ = -1.0;
    uint64_t lastTicks_ = 0;
    double quotesPerSecond_ = 0.0;
    double lastQuoteSeenAt_ = -1.0;
    std::deque<float> rttHistory_;
    std::deque<float> quoteHistory_;
    muuk::core::OrderId placeNowId_ = muuk::core::kInvalidOrderId;  // waiting order awaiting "Place now" confirmation

    Confirm confirm_ = Confirm::None;
    std::string manualReopenText_;

    // Settings > Account: the broker login of the current config file.
    struct AccountForm {
        bool loaded = false;
        bool live = false;
        std::array<char, 128> pasteSenderCompId{};
        std::array<char, 64> brokerUid{};
        std::array<char, 32> traderLogin{};
        std::array<char, 128> password{};  // write-only: never filled from the file
        std::array<char, 64> targetCompId{};
        std::array<char, 128> quoteHost{};
        std::array<char, 8> quotePort{};
        std::array<char, 128> tradeHost{};
        std::array<char, 8> tradePort{};
        std::array<char, 32> symbolName{};
        std::array<char, 16> symbolId{};
        std::array<char, 96> newFileName{};
        int platform = 0;  // 0 cTrader FIX, 1 MetaTrader 5 bridge
        int mt5EaPort = muuk::ctrader_fix::instance::kDefaultEaPort;
        int mt5FixPort = muuk::ctrader_fix::instance::kDefaultFixPort;
        bool mt5AutoStart = true;
        std::array<char, 260> mt5Terminal{};
        std::array<char, 32> mt5Login{};
        std::array<char, 128> mt5Password{};  // write-only
        std::array<char, 96> mt5Server{};
        std::array<char, 32> mt5ChartSymbol{};
    };
    std::vector<muuk::mt5_bridge::Mt5Install> mt5Installs_;
    bool mt5InstallsScanned_ = false;
    std::string mt5PreparedFor_;           // config MT5 was started for (once per file)
    LiveController* controller_ = nullptr;  // this frame's controller (set in render)
    // Writes the account form to the config file right away (MT5 needs no login to fill in).
    void saveAccountNow();
    int bridgeEaPortHint_ = muuk::ctrader_fix::instance::kDefaultEaPort;
    void renderMt5AccountFields();
    // True when the MT5 the form points at (or the first one found, when
    // "auto") is this copy's own marker MT5 rather than a broker MT5.
    [[nodiscard]] bool mt5SelectedTerminalOwned() const;
    AccountForm account_;
    std::string accountLoadedFor_;
    bool accountDirty_ = false;
    std::string accountMessage_;
    std::string savedMt5Login_;   // the config file's MT5 account (not the form being edited)
    std::string savedMt5Server_;
    std::string lastSeenMt5_;     // login@server last remembered
    bool mt5HintsPushed_ = false;
    bool mt5AccountsOpen_ = false;
    std::string mt5PopupShownFor_;  // the mismatch the list already opened for by itself
    std::array<char, 32> newMt5Login_{};
    std::array<char, 128> newMt5Password_{};
    std::array<char, 96> newMt5Server_{};
    std::string mt5AccountsMessage_;
    bool openConfirmRequested_ = false;

    std::string flashMessage_;
    double flashUntil_ = 0.0;

    ImFont* boldFont_ = nullptr;
    ImFont* largeFont_ = nullptr;
    ImFont* numberFont_ = nullptr;
};

}  // namespace muuk::desktop
