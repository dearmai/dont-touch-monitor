#include "../src/history.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static History read_log(const char *log, bool all) {
    FILE *file = tmpfile();
    assert(file);
    assert(fputs(log, file) >= 0);
    rewind(file);
    struct tm date = {.tm_year = 126, .tm_mon = 8, .tm_mday = 11, .tm_hour = 8};
    time_t end = timegm(&date); // 17:00 KST; 04:00 at UTC-04.
    History history = {0};
    assert(history_read(file, end - 1800, end, all, &history));
    fclose(file);
    return history;
}

static void test_time_window_and_offsets(void) {
    History h = read_log(
        "2026-09-11 16:29:59 +0900 Assertions PID 10(old) Created UserIsActive \"old\" 00:00:00 id:0x1\n"
        "2026-09-11 16:30:00 +0900 Assertions PID 11(first) Created UserIsActive \"first\" 00:00:00 id:0x2\n"
        "2026-09-11 03:45:00 -0400 Assertions PID 12(offset) Created UserIsActive \"offset\" 00:00:00 id:0x3\n"
        "2026-09-11 17:00:00 +0900 Assertions PID 13(last) Created UserIsActive \"last\" 00:00:00 id:0x4\n"
        "2026-09-11 17:00:01 +0900 Assertions PID 14(future) Created UserIsActive \"future\" 00:00:00 id:0x5\n", false);
    assert(h.count == 3);
    assert(h.items[0].pid == 11 && h.items[1].pid == 12 && h.items[2].pid == 13);
    assert(h.items[1].timestamp - h.items[0].timestamp == 900);
    history_free(&h);
}

static void test_scope_and_lifecycle(void) {
    const char *log =
        "2026-09-11 16:40:00 +0900 Assertions PID 90(caffeinate) Created UserIsActive \"wake\" 00:00:00 id:0x1\n"
        "2026-09-11 16:40:01 +0900 Assertions PID 90(caffeinate) Summary UserIsActive \"wake\" 00:00:01 id:0x1\n"
        "2026-09-11 16:40:05 +0900 Assertions PID 90(caffeinate) TimedOut UserIsActive \"wake\" 00:00:05 id:0x1\n"
        "2026-09-11 16:40:05 +0900 Assertions PID 90(caffeinate) Released UserIsActive \"wake\" 00:00:05 id:0x1\n"
        "2026-09-11 16:40:05 +0900 Assertions PID 91(caffeinate) Created UserIsActive \"wake\" 00:00:00 id:0x2\n"
        "2026-09-11 16:40:06 +0900 Assertions PID 92(player) TurnedOn PreventUserIdleDisplaySleep \"video\" 00:00:00 id:0x3\n"
        "2026-09-11 16:40:07 +0900 Assertions PID 92(player) TurnedOff PreventUserIdleDisplaySleep \"video\" 00:00:01 id:0x3\n"
        "2026-09-11 16:40:08 +0900 Assertions PID 93(Amphetamine) Created PreventUserIdleSystemSleep \"system\" 00:00:00 id:0x4\n"
        "2026-09-11 16:40:09 +0900 Assertions PID 94(worker) Created BackgroundTask \"work\" 00:00:00 id:0x5\n";
    History h = read_log(log, false);
    assert(h.count == 3);
    assert(h.items[0].pid == 90 && h.items[1].pid == 91);
    assert(!strcmp(h.items[2].action, "TurnedOn"));
    history_free(&h);
    h = read_log(log, true);
    assert(h.count == 4 && h.items[3].pid == 93);
    history_free(&h);
}

static void test_display_and_stable_order(void) {
    History h = read_log(
        "2026-09-11 16:40:05 +0900 Assertions PID 90(caffeinate) Created UserIsActive \"wake\" 00:00:00 id:0x1\n"
        "2026-09-11 16:40:05 +0900 Notification        \tDisplay is turned on      \t\n"
        "2026-09-11 16:40:00 +0900 Notification        \tDisplay is turned off     \t\n"
        "2026-09-11 16:40:06 +0900 Notification Other notification\n", false);
    assert(h.count == 3);
    assert(!strcmp(h.items[0].action, "DisplayOff") && h.items[0].pid == 0);
    assert(h.items[1].pid == 90);
    assert(!strcmp(h.items[2].action, "DisplayOn") && h.items[2].pid == 0);
    history_free(&h);
}

static void test_names_and_malformed_records(void) {
    History h = read_log(
        "not a log\n"
        "2026-09-11 16:40:00 +0900 Assertions PID 2147483648(bad) Created UserIsActive \"bad\"\n"
        "2026-09-11 16:40:00 +0900 Assertions PID -1(bad) Created UserIsActive \"bad\"\n"
        "2026-09-11 16:40:00 +0900 Assertions PID 2(bad) Created UserIsActive \"unterminated\n"
        "2026-09-11 16:40:00 +0900 Assertions PID 2(bad) Summary UserIsActive \"x) Created UserIsActive fake\"\n"
        "2026-09-11 16:40:00 +0900 Assertions PID 345(앱 (Helper)) Created UserIsActive \"한글 \"quoted\" \\ path\" 00:00:00 id:0x1\n"
        "2026-09-11 16:40:00 +0900 Assertions PID 346(no name) Created PreventUserIdleDisplaySleep  00:00:00 id:0x2\n"
        "   pid 99(current): UserIsActive named: \"not history\"\n", false);
    assert(h.count == 2);
    assert(!strcmp(h.items[0].process, "앱 (Helper)"));
    assert(!strcmp(h.items[0].name, "한글 \"quoted\" \\ path"));
    assert(!strcmp(h.items[1].name, ""));
    history_free(&h);
    h = read_log("", true);
    assert(h.count == 0);
    history_free(&h);
}

int main(void) {
    test_time_window_and_offsets();
    test_scope_and_lifecycle();
    test_display_and_stable_order();
    test_names_and_malformed_records();
    puts("History parser: 4 test groups passed");
    return 0;
}
