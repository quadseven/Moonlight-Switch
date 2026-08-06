#pragma once

/**
 * Arms the CPU exception handler by giving it the journal's file descriptor.
 *
 * A crash here leaves no Atmosphere crash report and no fatal report, and that
 * one observation fits three unrelated failures: a CPU fault whose report is
 * being lost, the OS terminating the process from outside after teardown
 * overran its grace period, and a hang ended by pulling the power. Those want
 * opposite investigations and nothing currently separates them.
 *
 * A "cpu.exception" line in the journal settles it. Present means the process
 * faulted, with the fault address and syndrome recorded. Absent means it did
 * not, which leaves only the other two.
 *
 * Pass fileno() of the already-open journal. Opening a file from an exception
 * handler would need the heap and the filesystem layer, either of which may be
 * the reason the handler is running.
 *
 * No-op off Switch.
 */
void switch_exception_handler_arm(int journalFd);
