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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>

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
#include "StdoutCapture.hpp"
#include "OtlpTraceExporter.hpp"
#include "OtlpMetricsExporter.hpp"

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

namespace {

/**
 * Reads <working dir>/otel-log-level, if present.
 *
 * Returns nothing when the file is absent, empty or unrecognised, which
 * leaves whatever level was already set. An unrecognised value is
 * deliberately not an error and not a silent downgrade: verbosity is a
 * debugging aid, and a typo in it should not change how the app logs.
 */
std::optional<brls::LogLevel> readOtelLogLevel(const std::string& workingDir) {
    std::ifstream file(workingDir + "/otel-log-level");
    if (!file.good()) {
        return std::nullopt;
    }

    std::string line;
    std::getline(file, line);

    const auto begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::nullopt;
    }
    const auto end = line.find_last_not_of(" \t\r\n");
    line = line.substr(begin, end - begin + 1);

    std::transform(line.begin(), line.end(), line.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (line == "error") return brls::LogLevel::LOG_ERROR;
    if (line == "warning" || line == "warn") return brls::LogLevel::LOG_WARNING;
    if (line == "info") return brls::LogLevel::LOG_INFO;
    if (line == "debug") return brls::LogLevel::LOG_DEBUG;
    if (line == "verbose" || line == "trace") return brls::LogLevel::LOG_VERBOSE;
    return std::nullopt;
}

}  // namespace

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
    bool loggerOffStdout = false;
    // Same reasoning as the span journal: relaunching to inspect a crash used
    // to destroy the log that described it. One generation is kept.
    {
        const std::string logPath = Settings::instance().log_path();
        const std::string prevPath = logPath + ".prev";
        std::remove(prevPath.c_str());
        std::rename(logPath.c_str(), prevPath.c_str());
    }
    if (std::FILE* logFile =
            std::fopen(Settings::instance().log_path().c_str(), "w")) {
        // Line buffered, because borealis only fflush()es per line under
        // __MINGW32__ (logger.hpp) and newlib hands us a fully buffered FILE
        // otherwise. Without this the tail of the log is wherever stdio
        // happened to flush, not where the process stopped, and a crash
        // truncates mid word for reasons that have nothing to do with the
        // crash. Two comments in this tree previously claimed the opposite
        // and a real investigation was built on them.
        setvbuf(logFile, nullptr, _IOLBF, 0);
        brls::Logger::setLogOutput(logFile);
        loggerOffStdout = true;
        brls::Logger::info("DIAGNOSTIC BUILD: file logging to {}",
                           Settings::instance().log_path());
    }
#endif

    // Verbosity is a file rather than a rebuild. The level set further up is
    // the default and stays INFO; <working dir>/otel-log-level overrides it
    // with one of error, warning, info, debug or verbose.
    //
    // It is applied here rather than at the original call because the working
    // directory is not known until Application::init has run. Nothing is lost
    // by the wait: the exporter subscribes below, so anything logged before
    // this point was never going to be exported at any level.
    if (const auto level = readOtelLogLevel(home)) {
        brls::Logger::setLogLevel(*level);
        brls::Logger::info("log level set from otel-log-level");
    }

    // Opt in only: does nothing unless <working dir>/otel-endpoint exists.
    if (OtlpLogExporter::instance().start(home)) {
        brls::Logger::info("OTLP log export enabled, endpoint {}",
                           OtlpLogExporter::instance().endpoint());
    }

    // Spans, for the questions logs cannot answer: what overlapped what, on
    // which thread, for how long. Reads the same otel-endpoint and posts to
    // /v1/traces. Also journals each completed span to spans.jsonl as it
    // finishes, because the failure being chased takes the whole console down
    // and nothing buffered for the network survives that.
    if (OtlpTraceExporter::instance().start(home)) {
        brls::Logger::info("OTLP trace export enabled, endpoint {}",
                           OtlpTraceExporter::instance().endpoint());
        // One trace per app run. Spans default to hanging off this, so work
        // on the detached termination thread and work on the main thread end
        // up correlated instead of in two unrelated traces.
        OtlpTraceExporter::instance().beginSession("moonlight.session");
    }

    // Numbers with a known correct answer, for the defects that produce no
    // log line and no span because nothing looked wrong at the time. A
    // subscription held twice where one was intended is a working program
    // right up until it corrupts memory.
    if (OtlpMetricsExporter::instance().start(home)) {
        brls::Logger::info("OTLP metric export enabled, endpoint {}",
                           OtlpMetricsExporter::instance().endpoint());
    }

    // Everything printf writes, which is the part nxlink shows and the log
    // event does not: moonlight-common-c, ffmpeg and libnx never touch
    // brls::Logger. Opt in via <working dir>/otel-capture-stdout, and a no-op
    // unless the exporter above actually started.
    //
    // Gated on the logger having been moved off stdout. borealis writes each
    // line to logOut before firing the event this exports through, and logOut
    // is stdout by default, so capturing while that is still true would send
    // every borealis line twice: once as a log record with a real severity,
    // once as a stdout record without one. If the log file could not be
    // opened the redirect did not happen, and capture stays off rather than
    // doubling the volume on the hottest path in the app.
#ifdef __SWITCH__
    if (!loggerOffStdout) {
        brls::Logger::warning(
            "not capturing stdout: borealis is still logging to it");
    } else if (StdoutCapture::install(home)) {
        brls::Logger::info("stdout and stderr are being captured to OTLP");
    }
#endif

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
    /*
     * Breadcrumbs around borealis' own shutdown, because mainLoop() returning
     * is far too late to learn anything from.
     *
     * Application::exit() is called from inside internalMainLoop, at
     * application.cpp:209, and mainLoop() only returns false afterwards. By
     * then exitEvent has fired and the entire view tree has been destroyed:
     * clear(), then the deletion pool drain that runs every ~Activity,
     * ~AppletFrame, ~Box and ~MainTabs. That drain is where the crash report
     * recovered upstream has its innermost frames.
     *
     * So mainloop.exited, below, is a post-mortem. Everything it was meant to
     * distinguish has already happened by the time it is written, and a death
     * inside teardown produced a journal identical to being killed mid-stream.
     * These three run at the boundaries that actually matter.
     */
    brls::Application::getWindowShouldCloseEvent()->subscribe(
        [] { otlp_trace_mark("applet.exit_requested"); });
    brls::Application::getExitEvent()->subscribe(
        [] { otlp_trace_mark("borealis.teardown.entered"); });
    brls::Application::getExitDoneEvent()->subscribe(
        [] { otlp_trace_mark("borealis.teardown.done"); });

    while (brls::Application::mainLoop()) {
#ifdef __PSV__
        if (!vitaHealthReported) {
            brls::Logger::info("VITA_HEALTH: READY");
            vitaHealthReported = true;
        }
#endif
    }

    // Everything from here is teardown, and teardown is where the crash report
    // nyanpasu64 recovered puts the fault: the borealis view tree being
    // destroyed, dying in a string operation inside ~Box. Nothing down here was
    // instrumented, so a death in it produced a journal that simply stopped,
    // which is also what being killed from outside produces.
    //
    // These marks make the two distinguishable. Each is written and flushed
    // where it stands, so the last one present is the last point reached.
    //
    // mainLoop() has already returned by now, so the deletion queue borealis
    // drains on its way out has already run; a journal ending on the previous
    // run's last span with no mainloop.exited at all means it died in there.
    otlp_trace_mark("mainloop.exited");

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
                           "droppedOverflow={} postFailures={} "
                           "tracePostFailures={} metricPostFailures={}",
                           stats.accepted, stats.sent, stats.droppedFailed,
                           stats.droppedOverflow, stats.postFailures,
                           stats.tracePostFailures, stats.metricPostFailures);

        // Spans reaching the card but not the backend is a real state and used
        // to be an invisible one: the journal looks healthy while APM stays
        // empty. Saying how many were produced next to how many POSTs were
        // refused separates "nothing happened" from "nothing arrived".
        const auto traceStats = OtlpTraceExporter::instance().stats();
        brls::Logger::info("OTLP traces: started={} ended={} droppedOverflow={} "
                           "journalWriteFailed={}",
                           traceStats.started, traceStats.ended,
                           traceStats.droppedOverflow,
                           OtlpTraceExporter::instance().journalWriteFailed());
        const std::string lastError = OtlpLogExporter::instance().lastError();
        if (!lastError.empty()) {
            brls::Logger::error("OTLP: last transport error: {}", lastError);
        }
    }

    otlp_trace_mark("exporters.stopped");

    // Exit
#if defined(PLATFORM_TVOS)
    exit(0);
#endif

    // Last thing written from inside main. Anything after this is static
    // destructors and exit handlers, which is where the recovered trace has
    // its outermost frames, so a journal whose final line is this one narrows
    // the fault to that window rather than leaving it open across all of exit.
    otlp_trace_mark("main.returning");

    return EXIT_SUCCESS;
}
