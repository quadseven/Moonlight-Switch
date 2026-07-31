//
//  main.cpp
//  Moonlight
//
//  Created by XITRIX on 26.05.2021.
//

// Switch include only necessary for demo videos recording
#ifdef __SWITCH__
#include <switch.h>
#endif

#ifdef __PSV__
extern "C" {
// PVR_PSP2 allocates its EGL/GLES state from the Sony libc heap. Match the
// memory model used by working Borealis Vita applications such as wiliwili.
unsigned int _newlib_heap_size_user      = 220 * 1024 * 1024;
unsigned int _pthread_stack_default_user = 2 * 1024 * 1024;
unsigned int sceLibcHeapSize             = 24 * 1024 * 1024;
}
#endif

#include <cstdio>
#include <cstdlib>

#include <borealis.hpp>
#include <string>

#include "add_host_tab.hpp"
#include "host_tab.hpp"
#include "link_cell.hpp"
#include "main_activity.hpp"
#include "main_tabs_view.hpp"
#include "settings_tab.hpp"
#include "views/boolean_slider_cell.hpp"
#include "OtlpLogExporter.hpp"

#include "DiscoverManager.hpp"
#include "MoonlightSession.hpp"
#include "SwitchMoonlightSessionDecoderAndRenderProvider.hpp"


#if defined(_WIN32) && defined(__SDL2__)
#include <SDL.h>
#define SDL_MAIN
#endif

#if defined(__SDL3__)
#include <SDL3/SDL_main.h>
#elif defined(__SDL2__)
#include <SDL_main.h>
#endif
#include <main_args.hpp>

using namespace brls::literals; // for _i18n

#ifdef __SWITCH__
namespace {

s32 selectAllowedSwitchCore(u64 affinityMask, int ordinal) {
    s32 lastAllowedCore = -1;

    for (s32 core = 0; core < 4; core++) {
        if ((affinityMask & (1ULL << core)) == 0) {
            continue;
        }

        lastAllowedCore = core;
        if (ordinal == 0) {
            return core;
        }

        ordinal--;
    }

    return lastAllowedCore;
}

void preferSwitchCore(int ordinal) {
    s32 preferredCore = -1;
    u64 affinityMask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&preferredCore, &affinityMask, CUR_THREAD_HANDLE))) {
        return;
    }

    s32 targetCore = selectAllowedSwitchCore(affinityMask, ordinal);
    if (targetCore >= 0 && targetCore != preferredCore) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, targetCore, static_cast<u32>(affinityMask));
    }
}

} // namespace
#endif

int main(int argc, char* argv[]) {
    // Enable recording for Twitter memes
#ifdef __SWITCH__
    appletInitializeGamePlayRecording();
    appletSetWirelessPriorityMode(AppletWirelessPriorityMode_OptimizedForWlan);

    // Keep the UI loop away from the hottest streaming worker core.
    preferSwitchCore(0);

    // Keep the main thread above others so that the program stays responsive
    // when doing software decoding
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);

    // auto at = appletGetAppletType();
    // g_application_mode = at == AppletType_Application || at == AppletType_SystemApplication;

    // // To get access to /dev/nvhost-nvjpg, we need nvdrv:{a,s,t}
    // // However, nvdrv:{a,s} have limited address space for gpu mappings
    // extern u32 __nx_nv_service_type, __nx_nv_transfermem_size;
    // __nx_nv_service_type     = NvServiceType_Factory;
    // __nx_nv_transfermem_size = (g_application_mode ? 16 : 3) * 0x100000;
#endif

    // Set log level
    // We recommend to use INFO for real apps
    // DIAGNOSTIC BUILD: INFO, not DEBUG. Every line is fflush()ed to the SD
    // card, and DEBUG volume during a 60fps stream would perturb the timing
    // we are trying to observe. The focus/suspend/resume lines are all INFO.
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    registerDeepLinkHandler();

#if defined(PLATFORM_VISIONOS)
    brls::Application::setMaximumUIScale(1.0f);
#endif

    MoonlightSession::set_provider(
            new SwitchMoonlightSessionDecoderAndRenderProvider());

    brls::Application::createWindow("title"_i18n);

    // Park the log exporter across console suspends. Losing focus on this
    // platform means sleep or the HOME menu, and the OS tears the network
    // stack down underneath us; a POST caught inside curl at that moment does
    // not survive the resume. Subscribed here rather than in the streaming
    // view because it has to hold whatever the app is doing, not only while a
    // stream is up.
    Application::getWindowFocusChangedEvent()->subscribe([](bool focused) {
        OtlpLogExporter::instance().setSuspended(!focused);
    });

    auto home = Application::getPlatform()->getHomeDirectory("Moonlight-Switch");
    Settings::instance().set_working_dir(home);
    Settings::instance().set_launch_path(argc > 0 ? argv[0] : "");
    brls::Logger::info("Working dir, {}", home);

#ifdef __SWITCH__
    // DIAGNOSTIC BUILD ONLY, not intended for upstream.
    //
    // Settings computes m_log_path as <home>/log.log but nothing ever uses
    // it, so every borealis log line goes to stdout and is discarded on a
    // console with no nxlink attached. Point the logger at that file so a
    // sleep/resume hang leaves a trail behind. borealis fflush()es after
    // every line, so whatever was written last survives the hard power off
    // that recovering from the hang requires.
    //
    // This is set up before the shipper starts so that everything from here
    // on, including the shipper's own startup line, lands in the file too.
    // The two are independent: the file always works, the shipper only if a
    // key is present, and losing the network costs you the shipper only.
    if (std::FILE* logFile =
            std::fopen(Settings::instance().log_path().c_str(), "w")) {
        brls::Logger::setLogOutput(logFile);
        brls::Logger::info("DIAGNOSTIC BUILD: file logging to {}",
                           Settings::instance().log_path());
    }
#endif

    // Opt in only: does nothing unless <working dir>/otel-endpoint exists.
    if (OtlpLogExporter::instance().start(home)) {
        brls::Logger::info("OTLP log export enabled, endpoint {}",
                           OtlpLogExporter::instance().endpoint());
    }

    // Have the application register an action on every activity that will quit
    // when you press BUTTON_START
    brls::Application::setGlobalQuit(false);
    brls::Application::setFPSStatus(false);

    // Register custom views (including tabs, which are views)
    brls::Application::registerXMLView("BooleanSliderCell", BooleanSliderCell::create);
    brls::Application::registerXMLView("LinkCell", LinkCell::create);

    brls::Application::registerXMLView("MainTabs", MainTabs::create);
    brls::Application::registerXMLView("HostTab", HostTab::create);
    brls::Application::registerXMLView("AddHostTab", AddHostTab::create);
    brls::Application::registerXMLView("SettingsTab", SettingsTab::create);

    // Add custom values to the theme
    brls::Theme::getLightTheme().addColor("captioned_image/caption",
                                   nvgRGB(2, 176, 183));
    brls::Theme::getDarkTheme().addColor("captioned_image/caption",
                                  nvgRGB(51, 186, 227));

    // Add custom values to the style
    brls::getStyle().addMetric("about/padding_top_bottom", 50);
    brls::getStyle().addMetric("about/padding_sides", 75);
    brls::getStyle().addMetric("about/description_margin", 50);

    // Create and push the main activity to the stack if cannot run game from arguments
    if (!startFromArgs(argc, argv)) {
        brls::Application::pushActivity(new MainActivity());
    }

    brls::Application::enableDebuggingView(Settings::instance().write_log());
    brls::Application::setSwapInputKeys(Settings::instance().swap_ui_keys());

    // Run the app. The Vita development loop waits for this marker so a
    // successful launch means at least one complete Borealis frame rendered.
#ifdef __PSV__
    bool vitaHealthReported = false;
#endif
    while (brls::Application::mainLoop()) {
#ifdef __PSV__
        if (!vitaHealthReported) {
            brls::Logger::info("VITA_HEALTH: READY");
            vitaHealthReported = true;
        }
#endif
    }

    // Stop FIRST, then report. stop() is the final flush and join, and a
    // summary read before it does not count anything that flush sends. That
    // ordering bug produced an exit line of sent=0 on a run where every
    // record demonstrably arrived at the backend, which reads as a transport
    // failure and sends whoever is debugging it in exactly the wrong
    // direction. The price of the correct order is that the summary itself
    // can no longer ship; it lands in the on-card log only, which is fine,
    // because the numbers it carries are about the run that just ended.
    const bool otlpWasEnabled = OtlpLogExporter::instance().enabled();
    OtlpLogExporter::instance().stop();

    if (otlpWasEnabled) {
        const auto stats = OtlpLogExporter::instance().stats();
        brls::Logger::info("OTLP: accepted={} sent={} droppedFailed={} "
                           "droppedOverflow={} postFailures={}",
                           stats.accepted, stats.sent, stats.droppedFailed,
                           stats.droppedOverflow, stats.postFailures);
        const std::string lastError = OtlpLogExporter::instance().lastError();
        if (!lastError.empty()) {
            brls::Logger::error("OTLP: last transport error: {}", lastError);
        }
    }

    // Exit
#if defined(PLATFORM_TVOS)
    exit(0);
#endif
    
    return EXIT_SUCCESS;
}
