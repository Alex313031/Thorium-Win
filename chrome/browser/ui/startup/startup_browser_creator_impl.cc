// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/startup/startup_browser_creator_impl.h"

#include <stddef.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/values.h"
#include "build/branding_buildflags.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/browser/apps/platform_apps/install_chrome_app.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/buildflags.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/custom_handlers/protocol_handler_registry.h"
#include "chrome/browser/custom_handlers/protocol_handler_registry_factory.h"
#include "chrome/browser/defaults.h"
#include "chrome/browser/infobars/simple_alert_infobar_creator.h"
#include "chrome/browser/obsolete_system/obsolete_system.h"
#include "chrome/browser/prefs/session_startup_pref.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_io_data.h"
#include "chrome/browser/sessions/app_session_service.h"
#include "chrome/browser/sessions/app_session_service_factory.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/session_crashed_bubble.h"
#include "chrome/browser/ui/startup/automation_infobar_delegate.h"
#include "chrome/browser/ui/startup/bad_flags_prompt.h"
#include "chrome/browser/ui/startup/default_browser_prompt.h"
#include "chrome/browser/ui/startup/google_api_keys_infobar_delegate.h"
#include "chrome/browser/ui/startup/launch_mode_recorder.h"
#include "chrome/browser/ui/startup/obsolete_system_infobar_delegate.h"
#include "chrome/browser/ui/startup/startup_browser_creator.h"
#include "chrome/browser/ui/startup/startup_tab_provider.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/welcome/helpers.h"
#include "chrome/browser/ui/webui/whats_new/whats_new_util.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chrome/browser/web_applications/web_app_provider_factory.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/url_constants.h"
#include "components/infobars/content/content_infobar_manager.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/dom_storage_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/content_switches.h"
#include "google_apis/google_api_keys.h"
#include "rlz/buildflags/buildflags.h"
#include "ui/base/buildflags.h"

#if defined(OS_MAC)
#include "base/mac/mac_util.h"
#include "chrome/browser/ui/cocoa/keystone_infobar_delegate.h"
#endif

#if defined(OS_WIN) && BUILDFLAG(GOOGLE_CHROME_BRANDING)
#include "chrome/browser/win/conflicts/incompatible_applications_updater.h"
#endif

#if BUILDFLAG(ENABLE_RLZ)
#include "components/google/core/common/google_util.h"
#include "components/rlz/rlz_tracker.h"  // nogncheck
#endif

#if BUILDFLAG(IS_CHROMEOS_ASH)
#include "chrome/browser/ash/crosapi/browser_util.h"
#include "components/app_restore/features.h"
#include "components/app_restore/full_restore_utils.h"
#endif

#if BUILDFLAG(IS_CHROMEOS_LACROS)
#include "chrome/browser/lacros/account_manager/account_manager_util.h"
#include "chromeos/lacros/lacros_service.h"
#endif

namespace {

// Utility functions ----------------------------------------------------------

// In ChromeOS, if the full restore feature is disabled, always restores apps
// unconditionally. If the full restore feature is enabled, check the previous
// apps launching history info to decide whether restore apps.
//
// In other platforms, restore apps only when the browser is automatically
// restarted.
bool ShouldRestoreApps(bool is_post_restart, Profile* profile) {
#if BUILDFLAG(IS_CHROMEOS_ASH)
  // If the full restore feature is enabled, check the full restore file.
  // Restore apps only when there are apps launched before reboot.
  if (full_restore::features::IsFullRestoreEnabled())
    return full_restore::HasAppTypeBrowser(profile->GetPath());

  // If the full restore feature is disabled, always restores apps
  // unconditionally.
  return true;
#else
  return is_post_restart;
#endif
}

void UrlsToTabs(const std::vector<GURL>& urls, StartupTabs* tabs) {
  for (const GURL& url : urls)
    tabs->push_back(StartupTab(url, false));
}

std::vector<GURL> TabsToUrls(const StartupTabs& tabs) {
  std::vector<GURL> urls;
  urls.reserve(tabs.size());
  std::transform(tabs.begin(), tabs.end(), std::back_inserter(urls),
                 [](const StartupTab& tab) { return tab.url; });
  return urls;
}

// Appends the contents of |from| to the end of |to|.
void AppendTabs(const StartupTabs& from, StartupTabs* to) {
  to->insert(to->end(), from.begin(), from.end());
}

// Prepends the contents of |from| to the beginning of |to|.
void PrependTabs(const StartupTabs& from, StartupTabs* to) {
  to->insert(to->begin(), from.begin(), from.end());
}

bool ShouldShowBadFlagsSecurityWarnings() {
#if !BUILDFLAG(IS_CHROMEOS_ASH)
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state)
    return true;

  const auto* pref = local_state->FindPreference(
      prefs::kCommandLineFlagSecurityWarningsEnabled);
  DCHECK(pref);

  // The warnings can only be disabled by policy. Default to show warnings.
  if (pref->IsManaged())
    return pref->GetValue()->GetBool();
#endif
  return true;
}

}  // namespace

StartupBrowserCreatorImpl::StartupBrowserCreatorImpl(
    const base::FilePath& cur_dir,
    const base::CommandLine& command_line,
    chrome::startup::IsFirstRun is_first_run)
    : cur_dir_(cur_dir),
      command_line_(command_line),
      profile_(nullptr),
      browser_creator_(nullptr),
      is_first_run_(is_first_run == chrome::startup::IS_FIRST_RUN) {}

StartupBrowserCreatorImpl::StartupBrowserCreatorImpl(
    const base::FilePath& cur_dir,
    const base::CommandLine& command_line,
    StartupBrowserCreator* browser_creator,
    chrome::startup::IsFirstRun is_first_run)
    : cur_dir_(cur_dir),
      command_line_(command_line),
      browser_creator_(browser_creator),
      is_first_run_(is_first_run == chrome::startup::IS_FIRST_RUN) {}

// static
void StartupBrowserCreatorImpl::MaybeToggleFullscreen(Browser* browser) {
  // In kiosk mode, we want to always be fullscreen.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kKioskMode) ||
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kStartFullscreen)) {
    chrome::ToggleFullscreenMode(browser);
  }
}

bool StartupBrowserCreatorImpl::Launch(
    Profile* profile,
    bool process_startup,
    std::unique_ptr<LaunchModeRecorder> launch_mode_recorder) {
  DCHECK(profile);
  profile_ = profile;

  LaunchResult launch_result = DetermineURLsAndLaunch(process_startup);

  // Check the true process command line for --try-chrome-again=N rather than
  // the one parsed for startup URLs and such.
  if (launch_mode_recorder) {
    if (!command_line_.GetSwitchValueNative(switches::kTryChromeAgain)
             .empty()) {
      launch_mode_recorder->SetLaunchMode(LaunchMode::kUserExperiment);
    } else {
      launch_mode_recorder->SetLaunchMode(launch_result ==
                                                  LaunchResult::kWithGivenUrls
                                              ? LaunchMode::kWithUrls
                                              : LaunchMode::kToBeDecided);
    }
  }

  if (command_line_.HasSwitch(switches::kInstallChromeApp)) {
    install_chrome_app::InstallChromeApp(
        command_line_.GetSwitchValueASCII(switches::kInstallChromeApp));
  }

#if defined(OS_MAC)
  if (process_startup) {
    // Check whether the auto-update system needs to be promoted from user
    // to system.
    KeystoneInfoBar::PromotionInfoBar(profile);
  }
#endif

  // It's possible for there to be no browser window, e.g. if someone
  // specified a non-sensical combination of options
  // ("--kiosk --no_startup_window"); do nothing in that case.
  Browser* browser = BrowserList::GetInstance()->GetLastActive();
  if (browser)
    MaybeToggleFullscreen(browser);

  return true;
}

Browser* StartupBrowserCreatorImpl::OpenURLsInBrowser(
    Browser* browser,
    bool process_startup,
    const std::vector<GURL>& urls) {
  StartupTabs tabs;
  UrlsToTabs(urls, &tabs);
  return OpenTabsInBrowser(browser, process_startup, tabs);
}

Browser* StartupBrowserCreatorImpl::OpenTabsInBrowser(Browser* browser,
                                                      bool process_startup,
                                                      const StartupTabs& tabs) {
  DCHECK(!tabs.empty());

  // If we don't yet have a profile, try to use the one we're given from
  // |browser|. While we may not end up actually using |browser| (since it
  // could be a popup window), we can at least use the profile.
  if (!profile_ && browser)
    profile_ = browser->profile();

  if (!browser || !browser->is_type_normal()) {
    // In some conditions a new browser object cannot be created. The most
    // common reason for not being able to create browser is having this call
    // when the browser process is shutting down. This can also fail if the
    // passed profile is of a type that is not suitable for browser creation.
    if (Browser::GetCreationStatusForProfile(profile_) !=
        Browser::CreationStatus::kOk) {
      return nullptr;
    }
    // Startup browsers are not counted as being created by a user_gesture
    // because of historical accident, even though the startup browser was
    // created in response to the user clicking on chrome. There was an
    // incomplete check on whether a user gesture created a window which looked
    // at the state of the MessageLoop.
    Browser::CreateParams params = Browser::CreateParams(profile_, false);
    params.creation_source = Browser::CreationSource::kStartupCreator;
    browser = Browser::Create(params);
  }

  bool first_tab = true;
  ProtocolHandlerRegistry* registry = profile_ ?
      ProtocolHandlerRegistryFactory::GetForBrowserContext(profile_) : NULL;
  for (size_t i = 0; i < tabs.size(); ++i) {
    // We skip URLs that we'd have to launch an external protocol handler for.
    // This avoids us getting into an infinite loop asking ourselves to open
    // a URL, should the handler be (incorrectly) configured to be us. Anyone
    // asking us to open such a URL should really ask the handler directly.
    bool handled_by_chrome = ProfileIOData::IsHandledURL(tabs[i].url) ||
        (registry && registry->IsHandledProtocol(tabs[i].url.scheme()));
    if (!process_startup && !handled_by_chrome)
      continue;

    int add_types = first_tab ? TabStripModel::ADD_ACTIVE :
                                TabStripModel::ADD_NONE;
    add_types |= TabStripModel::ADD_FORCE_INDEX;
    if (tabs[i].is_pinned)
      add_types |= TabStripModel::ADD_PINNED;

    NavigateParams params(browser, tabs[i].url,
                          ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    params.disposition = first_tab ? WindowOpenDisposition::NEW_FOREGROUND_TAB
                                   : WindowOpenDisposition::NEW_BACKGROUND_TAB;
    params.tabstrip_add_types = add_types;

#if BUILDFLAG(ENABLE_RLZ)
    if (process_startup && google_util::IsGoogleHomePageUrl(tabs[i].url)) {
      params.extra_headers = rlz::RLZTracker::GetAccessPointHttpHeader(
          rlz::RLZTracker::ChromeHomePage());
    }
#endif  // BUILDFLAG(ENABLE_RLZ)

    Navigate(&params);

    first_tab = false;
  }
  if (!browser->tab_strip_model()->GetActiveWebContents()) {
    // TODO(sky): this is a work around for 110909. Figure out why it's needed.
    if (!browser->tab_strip_model()->count())
      chrome::AddTabAt(browser, GURL(), -1, true);
    else
      browser->tab_strip_model()->ActivateTabAt(0);
  }

  browser->window()->Show();

  return browser;
}

StartupBrowserCreatorImpl::LaunchResult
StartupBrowserCreatorImpl::DetermineURLsAndLaunch(bool process_startup) {
  if (!ShouldLaunch(command_line_))
    return LaunchResult::kNormally;

  const bool is_incognito_or_guest = profile_->IsOffTheRecord();
  bool is_post_crash_launch = HasPendingUncleanExit(profile_);
  bool has_incompatible_applications = false;
#if defined(OS_WIN)
#if BUILDFLAG(GOOGLE_CHROME_BRANDING)
  if (is_post_crash_launch) {
    // Check if there are any incompatible applications cached from the last
    // Chrome run.
    has_incompatible_applications =
        IncompatibleApplicationsUpdater::HasCachedApplications();
  }
#endif
  welcome::JoinOnboardingGroup(profile_);
#endif

  // Presentation of promotional and/or educational tabs may be controlled via
  // administrative policy.
  bool promotional_tabs_enabled = true;
  const PrefService::Preference* enabled_pref = nullptr;
  PrefService* local_state = g_browser_process->local_state();
  if (local_state)
    enabled_pref = local_state->FindPreference(prefs::kPromotionalTabsEnabled);
  if (enabled_pref && enabled_pref->IsManaged()) {
    // Presentation is managed; obey the policy setting.
    promotional_tabs_enabled = enabled_pref->GetValue()->GetBool();
  } else {
    // Presentation is not managed. Infer an intent to disable if any value for
    // the RestoreOnStartup policy is mandatory or recommended.
    promotional_tabs_enabled =
        !SessionStartupPref::TypeIsManaged(profile_->GetPrefs()) &&
        !SessionStartupPref::TypeHasRecommendedValue(profile_->GetPrefs());
  }

  bool welcome_enabled = true;
#if !BUILDFLAG(IS_CHROMEOS_ASH)
  welcome_enabled =
      welcome::IsEnabled(profile_) && welcome::HasModulesToShow(profile_);
#endif  // !BUILDFLAG(IS_CHROMEOS_ASH)

#if BUILDFLAG(IS_CHROMEOS_LACROS)
  if (IsAccountManagerAvailable(profile_))
    welcome_enabled = false;
#endif  // BUILDFLAG(IS_CHROMEOS_LACROS)

  const bool whats_new_enabled =
      promotional_tabs_enabled && whats_new::ShouldShowForState(local_state);

  auto result = DetermineStartupTabs(
      StartupTabProviderImpl(), process_startup, is_incognito_or_guest,
      is_post_crash_launch, has_incompatible_applications,
      promotional_tabs_enabled, welcome_enabled, whats_new_enabled);
  StartupTabs tabs = std::move(result.tabs);

  // Return immediately if we start an async restore, since the remainder of
  // that process is self-contained.
  if (MaybeAsyncRestore(tabs, process_startup, is_post_crash_launch))
    return result.launch_result;

  BrowserOpenBehaviorOptions behavior_options = 0;
  if (process_startup)
    behavior_options |= PROCESS_STARTUP;
  if (is_post_crash_launch)
    behavior_options |= IS_POST_CRASH_LAUNCH;
  if (command_line_.HasSwitch(switches::kOpenInNewWindow))
    behavior_options |= HAS_NEW_WINDOW_SWITCH;
  if (result.launch_result == LaunchResult::kWithGivenUrls)
    behavior_options |= HAS_CMD_LINE_TABS;

  BrowserOpenBehavior behavior = DetermineBrowserOpenBehavior(
      StartupBrowserCreator::GetSessionStartupPref(command_line_, profile_),
      behavior_options);

  SessionRestore::BehaviorBitmask restore_options =
      SessionRestore::RESTORE_BROWSER;
  if (behavior == BrowserOpenBehavior::SYNCHRONOUS_RESTORE) {
#if defined(OS_MAC)
    bool was_mac_login_or_resume = base::mac::WasLaunchedAsLoginOrResumeItem();
#else
    bool was_mac_login_or_resume = false;
#endif
    restore_options = DetermineSynchronousRestoreOptions(
        browser_defaults::kAlwaysCreateTabbedBrowserOnSessionRestore,
        base::CommandLine::ForCurrentProcess()->HasSwitch(
            switches::kCreateBrowserOnStartupForTests),
        was_mac_login_or_resume);
  }

  Browser* browser = RestoreOrCreateBrowser(
      tabs, behavior, restore_options, process_startup, is_post_crash_launch);

  // Finally, add info bars.
  AddInfoBarsIfNecessary(
      browser, process_startup ? chrome::startup::IS_PROCESS_STARTUP
                               : chrome::startup::IS_NOT_PROCESS_STARTUP);
  return result.launch_result;
}

StartupBrowserCreatorImpl::DetermineStartupTabsResult::
    DetermineStartupTabsResult(StartupTabs tabs, LaunchResult launch_result)
    : tabs(std::move(tabs)), launch_result(launch_result) {}

StartupBrowserCreatorImpl::DetermineStartupTabsResult::
    DetermineStartupTabsResult(DetermineStartupTabsResult&&) = default;

StartupBrowserCreatorImpl::DetermineStartupTabsResult&
StartupBrowserCreatorImpl::DetermineStartupTabsResult::operator=(
    DetermineStartupTabsResult&&) = default;

StartupBrowserCreatorImpl::DetermineStartupTabsResult::
    ~DetermineStartupTabsResult() = default;

StartupBrowserCreatorImpl::DetermineStartupTabsResult
StartupBrowserCreatorImpl::DetermineStartupTabs(
    const StartupTabProvider& provider,
    bool process_startup,
    bool is_incognito_or_guest,
    bool is_post_crash_launch,
    bool has_incompatible_applications,
    bool promotional_tabs_enabled,
    bool welcome_enabled,
    bool whats_new_enabled) {
#if BUILDFLAG(IS_CHROMEOS_LACROS)
  {
    // If URLs are passed via crosapi, forcibly opens those tabs.
    StartupTabs crosapi_tabs = provider.GetCrosapiTabs();
    if (!crosapi_tabs.empty())
      return {std::move(crosapi_tabs), LaunchResult::kWithGivenUrls};
  }
#endif  // BUILDFLAG(IS_CHROMEOS_LACROS)

  StartupTabs tabs =
      provider.GetCommandLineTabs(command_line_, cur_dir_, profile_);
  LaunchResult launch_result =
      tabs.empty() ? LaunchResult::kNormally : LaunchResult::kWithGivenUrls;

  // Only the New Tab Page or command line URLs may be shown in incognito mode.
  // A similar policy exists for crash recovery launches, to prevent getting the
  // user stuck in a crash loop.
  if (is_incognito_or_guest || is_post_crash_launch) {
    if (!tabs.empty())
      return {std::move(tabs), launch_result};

    if (is_post_crash_launch) {
      tabs = provider.GetPostCrashTabs(has_incompatible_applications);
      if (!tabs.empty())
        return {std::move(tabs), launch_result};
    }

    return {StartupTabs({StartupTab(GURL(chrome::kChromeUINewTabURL), false)}),
            launch_result};
  }

  // A trigger on a profile may indicate that we should show a tab which
  // offers to reset the user's settings.  When this appears, it is first, and
  // may be shown alongside command-line tabs.
  StartupTabs reset_tabs = provider.GetResetTriggerTabs(profile_);

  // URLs passed on the command line supersede all others, except pinned tabs.
  PrependTabs(reset_tabs, &tabs);

  if (launch_result == LaunchResult::kNormally) {
    // An initial preferences file provided with this distribution may specify
    // tabs to be displayed on first run, overriding all non-command-line tabs,
    // including the profile reset tab.
    StartupTabs distribution_tabs =
        provider.GetDistributionFirstRunTabs(browser_creator_);
    if (!distribution_tabs.empty())
      return {std::move(distribution_tabs), launch_result};

    StartupTabs onboarding_tabs;
    if (promotional_tabs_enabled) {
#if defined(OS_WIN)
      // This is a launch from a prompt presented to an inactive user who chose
      // to open Chrome and is being brought to a specific URL for this one
      // launch. Launch the browser with the desired welcome back URL in the
      // foreground and the other ordinary URLs (e.g., a restored session) in
      // the background.
      StartupTabs welcome_back_tabs = provider.GetWelcomeBackTabs(
          profile_, browser_creator_, process_startup);
      AppendTabs(welcome_back_tabs, &tabs);
#endif  // defined(OS_WIN)

      if (welcome_enabled) {
        // Policies for welcome (e.g., first run) may show promotional and
        // introductory content depending on a number of system status factors,
        // including OS and whether or not this is First Run.
        onboarding_tabs = provider.GetOnboardingTabs(profile_);
        AppendTabs(onboarding_tabs, &tabs);
      }

      // Potentially add the What's New Page. Note that the What's New page
      // should never be shown in the same session as any first-run onboarding
      // tabs.
      if (onboarding_tabs.empty()) {
        StartupTabs new_features_tabs;
        new_features_tabs = provider.GetNewFeaturesTabs(whats_new_enabled);
        AppendTabs(new_features_tabs, &tabs);
      } else {
        // Record the current version so that What's New will not be shown until
        // after the next major version update.
        whats_new::SetLastVersion(g_browser_process->local_state());
      }
    }

    // If the user has set the preference indicating URLs to show on opening,
    // read and add those.
    StartupTabs prefs_tabs =
        provider.GetPreferencesTabs(command_line_, profile_);
    AppendTabs(prefs_tabs, &tabs);

    // Potentially add the New Tab Page. Onboarding content is designed to
    // replace (and eventually funnel the user to) the NTP. Note
    // URLs from preferences are explicitly meant to override showing the NTP.
    if (onboarding_tabs.empty() && prefs_tabs.empty())
      AppendTabs(provider.GetNewTabPageTabs(command_line_, profile_), &tabs);
  }

  // Maybe add any tabs which the user has previously pinned.
  AppendTabs(provider.GetPinnedTabs(command_line_, profile_), &tabs);

  return {std::move(tabs), launch_result};
}

bool StartupBrowserCreatorImpl::MaybeAsyncRestore(const StartupTabs& tabs,
                                                  bool process_startup,
                                                  bool is_post_crash_launch) {
  // Restore is performed synchronously on startup, and is never performed when
  // launching after crashing.
  if (process_startup || is_post_crash_launch)
    return false;

  // Note: there's no session service in incognito or guest mode.
  if (!SessionServiceFactory::GetForProfileForSessionRestore(profile_))
    return false;

  bool restore_apps =
      ShouldRestoreApps(StartupBrowserCreator::WasRestarted(), profile_);
  // Note: there's no session service in incognito or guest mode.
  SessionService* service =
      SessionServiceFactory::GetForProfileForSessionRestore(profile_);

  return service && service->RestoreIfNecessary(TabsToUrls(tabs), restore_apps);
}

Browser* StartupBrowserCreatorImpl::RestoreOrCreateBrowser(
    const StartupTabs& tabs,
    BrowserOpenBehavior behavior,
    SessionRestore::BehaviorBitmask restore_options,
    bool process_startup,
    bool is_post_crash_launch) {
  Browser* browser = nullptr;
  if (behavior == BrowserOpenBehavior::SYNCHRONOUS_RESTORE) {
    // It's worth noting that this codepath is not hit by crash restore
    // because we want to avoid a crash restore loop, so we don't
    // automatically restore after a crash.
    // Crash restores are triggered via session_crashed_bubble_view.cc
    if (ShouldRestoreApps(StartupBrowserCreator::WasRestarted(), profile_))
      restore_options |= SessionRestore::RESTORE_APPS;

    browser = SessionRestore::RestoreSession(profile_, nullptr, restore_options,
                                             TabsToUrls(tabs));
    if (browser)
      return browser;
  } else if (behavior == BrowserOpenBehavior::USE_EXISTING) {
    browser = chrome::FindTabbedBrowser(profile_, process_startup);
  }

  base::AutoReset<bool> synchronous_launch_resetter(
      &StartupBrowserCreator::in_synchronous_profile_launch_, true);

  // OpenTabsInBrowser requires at least one tab be passed. As a fallback to
  // prevent a crash, use the NTP if |tabs| is empty. This could happen if
  // we expected a session restore to happen but it did not occur/succeed.
  browser = OpenTabsInBrowser(
      browser, process_startup,
      (tabs.empty()
           ? StartupTabs({StartupTab(GURL(chrome::kChromeUINewTabURL), false)})
           : tabs));

  // Now that a restore is no longer possible, it is safe to clear DOM storage,
  // unless this is a crash recovery.
  if (!is_post_crash_launch) {
    profile_->GetDefaultStoragePartition()
        ->GetDOMStorageContext()
        ->StartScavengingUnusedSessionStorage();
  }

  return browser;
}

void StartupBrowserCreatorImpl::AddInfoBarsIfNecessary(
    Browser* browser,
    chrome::startup::IsProcessStartup is_process_startup) {
  if (!browser || !profile_ || browser->tab_strip_model()->count() == 0)
    return;

  // Show the Automation info bar unless it has been disabled by policy.
  bool show_bad_flags_security_warnings = ShouldShowBadFlagsSecurityWarnings();
  if (command_line_.HasSwitch(switches::kEnableAutomation) &&
      show_bad_flags_security_warnings) {
    AutomationInfoBarDelegate::Create();
  }

  // Do not show any other info bars in Kiosk mode, because it's unlikely that
  // the viewer can act upon or dismiss them.
  if (command_line_.HasSwitch(switches::kKioskMode))
    return;

  if (HasPendingUncleanExit(browser->profile()))
    SessionCrashedBubble::ShowIfNotOffTheRecordProfile(
        browser, /*skip_tab_checking=*/false);

  // The below info bars are only added to the first profile which is launched.
  // Other profiles might be restoring the browsing sessions asynchronously,
  // so we cannot add the info bars to the focused tabs here.
  //
  // These info bars are not shown when the browser is being controlled by
  // automated tests, so that they don't interfere with tests that assume no
  // info bars.
  if (is_process_startup == chrome::startup::IS_PROCESS_STARTUP &&
      !command_line_.HasSwitch(switches::kTestType) &&
      !command_line_.HasSwitch(switches::kEnableAutomation)) {
    content::WebContents* web_contents =
        browser->tab_strip_model()->GetActiveWebContents();
    DCHECK(web_contents);

    if (show_bad_flags_security_warnings)
      chrome::ShowBadFlagsPrompt(web_contents);

    infobars::ContentInfoBarManager* infobar_manager =
        infobars::ContentInfoBarManager::FromWebContents(web_contents);

    if (ObsoleteSystem::IsObsoleteNowOrSoon()) {
      PrefService* local_state = g_browser_process->local_state();
      if (!local_state ||
          !local_state->GetBoolean(prefs::kSuppressUnsupportedOSWarning))
        ObsoleteSystemInfoBarDelegate::Create(infobar_manager);
    }

#if !BUILDFLAG(IS_CHROMEOS_ASH)
    if (!command_line_.HasSwitch(switches::kNoDefaultBrowserCheck)) {
      // The default browser prompt should only be shown after the first run.
      if (!is_first_run_)
        ShowDefaultBrowserPrompt(profile_);
    }
#endif

  }
}

// static
StartupBrowserCreatorImpl::BrowserOpenBehavior
StartupBrowserCreatorImpl::DetermineBrowserOpenBehavior(
    const SessionStartupPref& pref,
    BrowserOpenBehaviorOptions options) {
  if (!(options & PROCESS_STARTUP)) {
    // For existing processes, restore would have happened before invoking this
    // function. If Chrome was launched with passed URLs, assume these should
    // be appended to an existing window if possible, unless overridden by a
    // switch.
    return ((options & HAS_CMD_LINE_TABS) && !(options & HAS_NEW_WINDOW_SWITCH))
               ? BrowserOpenBehavior::USE_EXISTING
               : BrowserOpenBehavior::NEW;
  }

  if (pref.type == SessionStartupPref::LAST) {
    // Don't perform a session restore on a post-crash launch, as this could
    // cause a crash loop.
    if (!(options & IS_POST_CRASH_LAUNCH))
      return BrowserOpenBehavior::SYNCHRONOUS_RESTORE;
  }

  return BrowserOpenBehavior::NEW;
}

// static
SessionRestore::BehaviorBitmask
StartupBrowserCreatorImpl::DetermineSynchronousRestoreOptions(
    bool has_create_browser_default,
    bool has_create_browser_switch,
    bool was_mac_login_or_resume) {
  SessionRestore::BehaviorBitmask options =
      SessionRestore::SYNCHRONOUS | SessionRestore::RESTORE_BROWSER;

  // Suppress the creation of a new window on Mac when restoring with no windows
  // if launching Chrome via a login item or the resume feature in OS 10.7+.
  if (!was_mac_login_or_resume &&
      (has_create_browser_default || has_create_browser_switch))
    options |= SessionRestore::ALWAYS_CREATE_TABBED_BROWSER;

  return options;
}

// static
bool StartupBrowserCreatorImpl::ShouldLaunch(
    const base::CommandLine& command_line) {
  // Don't open any browser windows if starting up in "background mode".
  if (command_line.HasSwitch(switches::kNoStartupWindow))
    return false;

#if BUILDFLAG(IS_CHROMEOS_LACROS)
  // Don't open any browser windows if Ash requested that Lacros not do so. The
  // implicit assumption is that some other code is responsible for keeping
  // Lacros running in the background
  if (chromeos::LacrosService::Get() &&
      chromeos::LacrosService::Get()->init_params()->initial_browser_action ==
          crosapi::mojom::InitialBrowserAction::kDoNotOpenWindow) {
    return false;
  }
#endif

#if BUILDFLAG(IS_CHROMEOS_ASH)
  // If Lacros is the primary web browser, do not open the browser window
  // on Chrome OS session login.
  if (crosapi::browser_util::IsLacrosPrimaryBrowser())
    return false;
#endif

  return true;
}
