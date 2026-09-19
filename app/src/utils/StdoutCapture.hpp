#pragma once

#include <string>

/**
 * Sends everything written to stdout and stderr to the OTLP exporter.
 *
 * The gap this closes
 * -------------------
 * OtlpLogExporter subscribes to the borealis log event, so it sees exactly
 * what the app chose to log through brls::Logger and nothing else. Large
 * parts of a Moonlight session are not that: moonlight-common-c, ffmpeg and
 * libnx all write with printf, and none of it reaches the log event. A
 * reviewer running nxlink sees those lines; we do not, which is a real part
 * of why a defect we shipped was found with gdb on someone else's desk.
 *
 * nxlink itself is only
 *
 *     dup2(sock, STDOUT_FILENO);
 *     dup2(sock, STDERR_FILENO);
 *
 * pointing the two descriptors at a TCP socket back to a PC. This does the
 * same interception without the PC: it swaps the device behind those
 * descriptors for one that assembles lines and hands them to the exporter,
 * which is how libnx's own consoleInit redirects stdout to the screen.
 *
 * Opt in
 * ------
 * Off unless <working dir>/otel-capture-stdout exists, and a no-op if the
 * exporter has no endpoint. Being on costs bandwidth and buffer on a path
 * that can produce thousands of lines a second during a stream, so it is a
 * debugging mode rather than something to leave running.
 */
namespace StdoutCapture {

/**
 * Installs the capture if workingDir/otel-capture-stdout exists.
 *
 * Returns whether it was installed. Call after OtlpLogExporter::start, since
 * a capture with nowhere to send is pointless, and after any
 * brls::Logger::setLogOutput, because this redirects that too.
 *
 * The devoptab entry itself is never removed: it has to outlive every FILE
 * that might still be flushed during shutdown, and swapping devoptab_list
 * back while another thread is mid write is a real problem. See stop(),
 * though -- that used to mean anything written after the exporter stopped
 * was captured into a sink nothing was draining and lost, during exactly
 * the teardown window this exists to make visible. It no longer does.
 */
bool install(const std::string& workingDir);

/**
 * Stops routing captured writes to OTLP; from here on they pass straight
 * through to whatever stdout/stderr pointed at before install() ran (the
 * console, or an nxlink socket), the same as if the capture had never been
 * installed.
 *
 * Call once OtlpLogExporter::stop() has returned, and before doing anything
 * else that might printf during the remainder of the process's life. Safe to
 * call even if install() never ran or never took: a no-op unless the capture
 * is actually active.
 *
 * This only flips a flag the write path already checks; devoptab_list itself
 * is never touched again, for the same reason install() only ever sets it
 * once. The previous device saved by install() is invoked directly, so a
 * line written during this remainder of teardown reaches the same place it
 * would have without capture, instead of being buffered into an exporter
 * that has already joined its worker thread and will never send it.
 */
void stop();

/**
 * Whether the capture was asked for, without installing anything.
 *
 * Lets a caller do whatever setup the capture requires before committing to
 * it, notably moving the borealis logger off stdout.
 */
bool requested(const std::string& workingDir);

/** Whether install() put the capture in place. */
bool active();

}  // namespace StdoutCapture
