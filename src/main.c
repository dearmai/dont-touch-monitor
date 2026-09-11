#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <errno.h>
#include <limits.h>
#include <libproc.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>
#include "history.h"
#include "parents.h"

#define VERSION "1.2.0"

typedef struct {
    pid_t pid;
    struct proc_bsdinfo info;
    bool identified;
    char name[PROC_PIDPATHINFO_MAXSIZE];
    char path[PROC_PIDPATHINFO_MAXSIZE];
    CFMutableArrayRef assertions;
    int rank;
    Parents parents;
} Blocker;

typedef struct {
    Blocker *items;
    size_t count;
} Snapshot;

static void *allocate(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size);
    if (!p) { fputs("메모리가 부족합니다.\n", stderr); exit(1); }
    return p;
}

static long number(CFTypeRef value, long fallback) {
    long result;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
        !CFNumberGetValue(value, kCFNumberLongType, &result)) return fallback;
    return result;
}

static char *string(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return strdup("");
    CFIndex length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value),
                                                      kCFStringEncodingUTF8) + 1;
    char *buffer = allocate((size_t)length, 1);
    if (!CFStringGetCString(value, buffer, length, kCFStringEncodingUTF8)) buffer[0] = 0;
    return buffer;
}

// UserIsActive is a transient activity hint, not proof of a misbehaving app.
static int assertion_rank(CFDictionaryRef assertion, bool all) {
    if (number(CFDictionaryGetValue(assertion, kIOPMAssertionLevelKey), 0) == 0) return 0;
    char *type = string(CFDictionaryGetValue(assertion, kIOPMAssertionTypeKey));
    int rank = 0;
    if (!strcmp(type, "PreventUserIdleDisplaySleep") || !strcmp(type, "NoDisplaySleepAssertion")) rank = 1;
    else if (!strcmp(type, "UserIsActive")) rank = 2;
    else if (all && (!strcmp(type, "PreventUserIdleSystemSleep") ||
                     !strcmp(type, "PreventSystemSleep") || !strcmp(type, "NoIdleSleepAssertion"))) rank = 3;
    free(type);
    return rank;
}

static bool identify(pid_t pid, struct proc_bsdinfo *info) {
    return process_identify(pid, info);
}

static int compare(const void *a, const void *b) {
    const Blocker *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static bool scan(bool all, Snapshot *out) {
    CFDictionaryRef by_pid = NULL;
    IOReturn result = IOPMCopyAssertionsByProcess(&by_pid);
    if (result != kIOReturnSuccess || !by_pid) {
        fprintf(stderr, "전원 요청 조회 실패: IOKit 0x%08x\n", result);
        return false;
    }
    CFIndex count = CFDictionaryGetCount(by_pid);
    const void **keys = allocate((size_t)count, sizeof(*keys));
    const void **values = allocate((size_t)count, sizeof(*values));
    CFDictionaryGetKeysAndValues(by_pid, keys, values);
    out->items = allocate((size_t)count, sizeof(Blocker));
    out->count = 0;
    for (CFIndex i = 0; i < count; ++i) {
        long pid = number(keys[i], -1);
        if (pid <= 0 || pid > INT_MAX || CFGetTypeID(values[i]) != CFArrayGetTypeID()) continue;
        CFArrayRef assertions = values[i];
        CFMutableArrayRef matching = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        int best_rank = 4;
        for (CFIndex j = 0; j < CFArrayGetCount(assertions); ++j) {
            CFTypeRef entry = CFArrayGetValueAtIndex(assertions, j);
            if (CFGetTypeID(entry) != CFDictionaryGetTypeID()) continue;
            int rank = assertion_rank(entry, all);
            if (rank) {
                CFArrayAppendValue(matching, entry);
                if (rank < best_rank) best_rank = rank;
            }
        }
        if (!CFArrayGetCount(matching)) { CFRelease(matching); continue; }
        Blocker *b = &out->items[out->count++];
        b->pid = (pid_t)pid;
        b->rank = best_rank;
        b->assertions = matching;
        b->identified = identify(b->pid, &b->info);
        if (proc_pidpath(b->pid, b->path, sizeof(b->path)) <= 0) b->path[0] = 0;
        if (b->path[0]) {
            const char *base = strrchr(b->path, '/');
            strlcpy(b->name, base ? base + 1 : b->path, sizeof(b->name));
        } else if (b->identified) {
            strlcpy(b->name, b->info.pbi_name[0] ? b->info.pbi_name : b->info.pbi_comm, sizeof(b->name));
        } else {
            strlcpy(b->name, "(종료됨/정보 없음)", sizeof(b->name));
        }
        if (b->identified) parents_read(b->pid, &b->info, &b->parents);
    }
    qsort(out->items, out->count, sizeof(Blocker), compare);
    free(keys);
    free(values);
    CFRelease(by_pid);
    return true;
}

static void release_snapshot(Snapshot *snapshot) {
    for (size_t i = 0; i < snapshot->count; ++i) CFRelease(snapshot->items[i].assertions);
    free(snapshot->items);
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static const char *protection(const Blocker *b) {
    if (!b->identified || !b->path[0]) return "프로세스 신원 확인 불가";
    if (b->pid <= 1 || b->pid == getpid() || b->pid == getppid()) return "자기 자신/부모/핵심 프로세스";
    if (b->info.pbi_uid == 0 || b->info.pbi_uid != getuid()) return "다른 사용자 또는 root 프로세스";
    if ((b->info.pbi_flags & PROC_FLAG_SYSTEM) || starts_with(b->path, "/System/") ||
        starts_with(b->path, "/usr/libexec/") || starts_with(b->path, "/usr/sbin/") ||
        starts_with(b->path, "/sbin/")) return "시스템 프로세스";
    return NULL;
}

// Escape terminal control characters in untrusted process names and assertion text.
static void print_string(const char *s, bool json) {
    if (json) putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (json && (*p == '"' || *p == '\\')) printf("\\%c", *p);
        else if (*p < 0x20 || *p == 0x7f) printf("\\u%04x", *p);
        else putchar(*p);
    }
    if (json) putchar('"');
}

static int cell_character(const char *text, size_t *bytes, bool *replace) {
    wchar_t character;
    mbstate_t state = {0};
    *bytes = mbrtowc(&character, text, MB_CUR_MAX, &state);
    *replace = *bytes == (size_t)-1 || *bytes == (size_t)-2;
    if (*replace) { *bytes = 1; return 1; }
    int columns = wcwidth(character);
    if (columns < 0) { *replace = true; return 1; }
    return columns;
}

// Measure terminal columns, not UTF-8 bytes, so Korean text stays aligned.
static void table_cell(const char *text, int width) {
    int total = 0;
    for (const char *p = text; *p;) {
        size_t bytes;
        bool replace;
        total += cell_character(p, &bytes, &replace);
        p += bytes;
    }
    int limit = total > width ? width - 3 : width;
    int used = 0;
    while (*text) {
        size_t bytes;
        bool replace;
        int columns = cell_character(text, &bytes, &replace);
        if (used + columns > limit) break;
        if (replace) putchar('?');
        else fwrite(text, 1, bytes, stdout);
        text += bytes;
        used += columns;
    }
    if (*text) { printf("..."); used += 3; }
    while (used++ < width) putchar(' ');
}

static void table_rule_n(const int *widths, int count) {
    putchar('+');
    for (int i = 0; i < count; ++i) {
        for (int j = 0; j < widths[i] + 2; ++j) putchar('-');
        putchar('+');
    }
    putchar('\n');
}

static void table_rule(const int widths[5]) { table_rule_n(widths, 5); }

static void table_row_n(const int *widths, int count, const char **cells) {
    putchar('|');
    for (int i = 0; i < count; ++i) {
        putchar(' ');
        table_cell(cells[i], widths[i]);
        printf(" |");
    }
    putchar('\n');
}

static void table_row(const int widths[5], const char *pid, const char *app,
                      const char *type, const char *status, const char *detail) {
    const char *cells[] = {pid, app, type, status, detail};
    table_row_n(widths, 5, cells);
}

static void print_parents(const Parents *parents) {
    putchar('[');
    for (size_t i = 0; i < parents->count; ++i) {
        const Parent *p = &parents->items[i];
        printf("%s{\"pid\":%d,\"name\":", i ? "," : "", p->pid);
        print_string(p->name, true);
        printf(",\"path\":"); print_string(p->path, true);
        printf("}");
    }
    putchar(']');
}

static void show(const Snapshot *snapshot, bool json, bool all) {
    struct winsize terminal = {0};
    int columns = 120;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &terminal) == 0 && terminal.ws_col)
        columns = terminal.ws_col;
    // Keep useful column widths even on very narrow terminals.
    if (columns < 100) columns = 100;
    if (columns > 160) columns = 160;
    int widths[] = {7, 16, 24, 13, 6, columns - 85};
    if (json) printf("{\"version\":\"%s\",\"scope\":\"%s\",\"processes\":[", VERSION, all ? "all" : "display");
    else {
        puts(all ? "화면 꺼짐·시스템 잠자기 관련 전원 요청" : "화면 꺼짐 관련 전원 요청");
        putchar('\n');
        table_rule_n(widths, 6);
        table_row_n(widths, 6, (const char *[]){"PID", "앱 / 프로세스", "부모 프로세스 (PID)", "방해 유형", "종료", "요청 내용"});
        table_rule_n(widths, 6);
    }
    for (size_t i = 0; i < snapshot->count; ++i) {
        const Blocker *b = &snapshot->items[i];
        const char *reason = protection(b);
        if (json) {
            printf("%s{\"pid\":%d,\"name\":", i ? "," : "", b->pid);
            print_string(b->name, true);
            printf(",\"path\":"); print_string(b->path, true);
            printf(",\"parent_pid\":");
            if (b->parents.count) printf("%d", b->parents.items[0].pid); else printf("null");
            printf(",\"parent_name\":");
            if (b->parents.count) print_string(b->parents.items[0].name, true); else printf("null");
            printf(",\"ancestors\":"); print_parents(&b->parents);
            printf(",\"can_terminate\":%s,\"protection_reason\":", reason ? "false" : "true");
            if (reason) print_string(reason, true); else printf("null");
            printf(",\"assertions\":[");
        }
        for (CFIndex j = 0; j < CFArrayGetCount(b->assertions); ++j) {
            CFDictionaryRef a = CFArrayGetValueAtIndex(b->assertions, j);
            char *type = string(CFDictionaryGetValue(a, kIOPMAssertionTypeKey));
            char *name = string(CFDictionaryGetValue(a, kIOPMAssertionNameKey));
            int rank = assertion_rank(a, true);
            if (json) {
                printf("%s{\"type\":", j ? "," : ""); print_string(type, true);
                printf(",\"name\":"); print_string(name, true);
                printf("}");
            } else {
                char pid[16];
                snprintf(pid, sizeof(pid), "%d", b->pid);
                char parent[PROC_PIDPATHINFO_MAXSIZE + 32];
                if (b->parents.count) snprintf(parent, sizeof(parent), "%s (%d)", b->parents.items[0].name, b->parents.items[0].pid);
                else strlcpy(parent, "확인 불가", sizeof(parent));
                table_row_n(widths, 6, (const char *[]){j == 0 ? pid : "", j == 0 ? b->name : "", j == 0 ? parent : "",
                          rank == 1 ? "화면 꺼짐" : rank == 2 ? "사용자 활동" : "시스템 잠자기",
                          j == 0 ? (reason ? "보호됨" : "가능") : "", name});
            }
            free(type); free(name);
        }
        if (json) printf("]}");
        else {
            if (reason) table_row_n(widths, 6, (const char *[]){"", "", "", "종료 제한", "", reason});
            table_rule_n(widths, 6);
        }
    }
    if (json) puts("]}");
    else {
        if (!snapshot->count) puts("해당하는 활성 전원 요청이 없습니다.");
        puts("\n사용자 활동은 정상 입력도 포함하며, 화면 꺼짐 방지의 확정 원인은 아닙니다.");
        puts("생략된 전체 내용: dont-touch-monitor list --all --json");
        puts("\n종료: dont-touch-monitor kill <PID> [--force] [--dry-run]");
        if (!all) puts("시스템 잠자기 요청도 보기: dont-touch-monitor list --all");
        puts("자동 화면 꺼짐 진단이며, 수동 화면 잠금 차단 여부를 판정하지 않습니다.");
    }
}

static Blocker *find_pid(Snapshot *s, pid_t pid) {
    for (size_t i = 0; i < s->count; ++i) if (s->items[i].pid == pid) return &s->items[i];
    return NULL;
}

static bool same_process(const Blocker *a, const Blocker *b) {
    return a->identified && b->identified && a->pid == b->pid &&
        a->info.pbi_start_tvsec == b->info.pbi_start_tvsec &&
        a->info.pbi_start_tvusec == b->info.pbi_start_tvusec &&
        a->info.pbi_uid == b->info.pbi_uid && !strcmp(a->path, b->path);
}

static int terminate_processes(Snapshot *snapshot, pid_t *pids, size_t count, bool force, bool dry_run) {
    // Validate the complete explicit target list before sending any signals.
    for (size_t i = 0; i < count; ++i) {
        Blocker *b = find_pid(snapshot, pids[i]);
        if (!b) {
            fprintf(stderr, "PID %d: 관련 활성 전원 요청이 없습니다.\n", pids[i]);
            return 1;
        }
        const char *reason = protection(b);
        if (reason) {
            fprintf(stderr, "PID %d: 종료할 수 없습니다 (%s).\n", b->pid, reason);
            return 1;
        }
    }
    int status = 0;
    for (size_t i = 0; i < count; ++i) {
        Blocker *b = find_pid(snapshot, pids[i]);
        Snapshot fresh = {0};
        if (!scan(true, &fresh)) return 1;
        Blocker *current = find_pid(&fresh, b->pid);
        if (!current || !same_process(b, current) || protection(current)) {
            fprintf(stderr, "PID %d: 프로세스 또는 전원 요청이 바뀌어 건너뜁니다.\n", b->pid);
            release_snapshot(&fresh);
            status = 1;
            continue;
        }
        // Recheck start time immediately before kill to reduce PID-reuse risk.
        struct proc_bsdinfo info;
        if (!identify(b->pid, &info) || info.pbi_start_tvsec != b->info.pbi_start_tvsec ||
            info.pbi_start_tvusec != b->info.pbi_start_tvusec || info.pbi_uid != b->info.pbi_uid) {
            fprintf(stderr, "PID %d: 신원이 바뀌어 건너뜁니다.\n", b->pid);
            status = 1;
        } else if (!dry_run && kill(b->pid, force ? SIGKILL : SIGTERM) != 0) {
            fprintf(stderr, "PID %d: 종료 신호 전송 실패: %s\n", b->pid, strerror(errno));
            status = 1;
        } else {
            printf("%sPID %d (", dry_run ? "[dry-run] " : "", b->pid);
            print_string(b->name, false);
            printf(") %s %s\n", force ? "SIGKILL" : "SIGTERM", dry_run ? "전송 예정" : "전송 완료");
        }
        release_snapshot(&fresh);
    }
    return status;
}

static void format_time(time_t timestamp, char buffer[32]) {
    struct tm local;
    localtime_r(&timestamp, &local);
    strftime(buffer, 32, "%Y-%m-%d %H:%M:%S %z", &local);
}

static int show_history(int minutes, bool all, bool json) {
    time_t to = time(NULL), from = to - minutes * 60;
    History history = {0};
    if (isatty(STDERR_FILENO)) fputs("전원 로그를 읽는 중입니다. 로그 크기에 따라 시간이 걸릴 수 있습니다.\n", stderr);
    if (!history_load(from, to, all, &history)) { history_free(&history); return 1; }
    char from_text[32], to_text[32];
    format_time(from, from_text);
    format_time(to, to_text);
    int columns = 100;
    struct winsize terminal = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &terminal) == 0 && terminal.ws_col) columns = terminal.ws_col;
    if (columns < 80) columns = 80;
    if (columns > 120) columns = 120;
    int widths[] = {8, 7, columns < 100 ? 12 : 18, 13, 0};
    widths[4] = columns - 16 - widths[0] - widths[1] - widths[2] - widths[3];
    if (json) {
        printf("{\"version\":\"%s\",\"minutes\":%d,\"scope\":\"%s\",\"from\":", VERSION, minutes, all ? "all" : "display");
        print_string(from_text, true);
        printf(",\"to\":"); print_string(to_text, true);
        printf(",\"events\":[");
    } else {
        printf("최근 %d분 전원 요청·화면 켜짐/꺼짐 기록\n%s ~ %s\n\n", minutes, from_text, to_text);
        table_rule(widths);
        table_row(widths, "시각", "PID", "앱 / 프로세스", "이벤트", "요청 내용");
        table_rule(widths);
    }
    size_t requests = 0, display_changes = 0;
    for (size_t i = 0; i < history.count; ++i) {
        const HistoryEvent *event = &history.items[i];
        bool display = event->pid == 0;
        if (display) ++display_changes; else ++requests;
        if (json) {
            printf("%s{\"time\":", i ? "," : ""); print_string(event->time, true);
            printf(",\"timestamp\":%lld,\"pid\":", (long long)event->timestamp);
            if (display) printf("null"); else printf("%d", event->pid);
            printf(",\"process\":");
            if (display) printf("null"); else print_string(event->process, true);
            printf(",\"action\":"); print_string(event->action, true);
            printf(",\"type\":"); print_string(event->type, true);
            printf(",\"name\":"); print_string(event->name, true);
            printf("}");
        } else {
            char clock_text[9], pid[16], label[64];
            // Display all rows in the current timezone; JSON preserves original offsets.
            struct tm local;
            localtime_r(&event->timestamp, &local);
            strftime(clock_text, sizeof(clock_text), "%H:%M:%S", &local);
            snprintf(pid, sizeof(pid), "%d", event->pid);
            if (display) {
                strlcpy(label, !strcmp(event->action, "DisplayOn") ? "화면 켜짐" : "화면 꺼짐", sizeof(label));
            } else {
                const char *kind = !strcmp(event->type, "UserIsActive") ? "활동" :
                    (!strcmp(event->type, "PreventUserIdleDisplaySleep") ||
                     !strcmp(event->type, "NoDisplaySleepAssertion")) ? "화면" : "시스템";
                snprintf(label, sizeof(label), "%s %s", kind, !strcmp(event->action, "Created") ? "생성" : "활성");
            }
            table_row(widths, clock_text, display ? "-" : pid, display ? "-" : event->process,
                      label, display ? "" : event->name);
        }
    }
    if (json) puts("]}");
    else {
        table_rule(widths);
        printf("요청 %zu건 · 화면 상태 변경 %zu건\n", requests, display_changes);
        if (!history.count) puts("해당 기간에 저장된 관련 기록이 없습니다.");
        puts("생성·활성화 기록만 표시합니다. 활동 요청은 정상 입력도 포함합니다.");
        puts("과거 PID는 종료되거나 재사용될 수 있습니다. 종료 전 현재 list를 확인하세요.");
        puts("종료된 프로세스의 부모 앱 정보는 전원 로그만으로 복원할 수 없습니다.");
        puts("전체 내용: history --json / 시스템 잠자기 포함: history --all");
    }
    history_free(&history);
    return 0;
}

static void usage(void) {
    puts("dont-touch-monitor " VERSION " — macOS 화면 꺼짐 방해 앱 진단/종료\n"
         "사용법:\n"
         "  dont-touch-monitor [list] [--all] [--json]\n"
         "  dont-touch-monitor history [--minutes N] [--all] [--json]\n"
         "  dont-touch-monitor kill <PID>... [--force] [--dry-run]\n"
         "  dont-touch-monitor --help | --version\n\n"
         "  --all      시스템 잠자기 방지 요청까지 표시\n"
         "  --json     목록을 JSON으로 출력\n"
         "  --minutes  history 조회 기간, 기본 30분 (1~1440)\n"
         "  --force    SIGTERM 대신 SIGKILL로 강제 종료 (미저장 내용 손실 가능)\n"
         "  --dry-run  대상을 검증하고 전송할 신호만 출력\n\n"
         "kill은 시스템 잠자기 방지 앱도 대상으로 허용합니다.\n"
         "현재 사용자 소유의 확인된 프로세스만 종료하며 시스템 프로세스는 보호합니다.");
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    if (wcwidth(L'가') != 2) setlocale(LC_CTYPE, "en_US.UTF-8");
    bool do_kill = false, all = false, json = false, force = false, dry_run = false;
    bool history = false;
    int minutes = 30;
    int start = 1;
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { usage(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--version")) { puts(VERSION); return 0; }
    if (argc > 1 && !strcmp(argv[1], "kill")) { do_kill = true; start = 2; }
    else if (argc > 1 && !strcmp(argv[1], "history")) { history = true; start = 2; }
    else if (argc > 1 && !strcmp(argv[1], "list")) start = 2;
    pid_t *pids = allocate((size_t)argc, sizeof(pid_t));
    size_t count = 0;
    for (int i = start; i < argc; ++i) {
        if (!do_kill && !strcmp(argv[i], "--all")) all = true;
        else if (!do_kill && !strcmp(argv[i], "--json")) json = true;
        else if (history && !strcmp(argv[i], "--minutes")) {
            if (++i >= argc || argv[i][0] < '0' || argv[i][0] > '9') goto invalid;
            char *end;
            errno = 0;
            long value = strtol(argv[i], &end, 10);
            if (errno || *end || value < 1 || value > 1440) goto invalid;
            minutes = (int)value;
        }
        else if (do_kill && !strcmp(argv[i], "--force")) force = true;
        else if (do_kill && !strcmp(argv[i], "--dry-run")) dry_run = true;
        else if (do_kill && argv[i][0] >= '0' && argv[i][0] <= '9') {
            char *end;
            errno = 0;
            long value = strtol(argv[i], &end, 10);
            if (errno || *end || value <= 1 || value > INT_MAX) goto invalid;
            bool duplicate = false;
            for (size_t j = 0; j < count; ++j) if (pids[j] == value) duplicate = true;
            if (!duplicate) pids[count++] = (pid_t)value;
        } else goto invalid;
    }
    if (do_kill && !count) goto invalid;
    if (history) { free(pids); return show_history(minutes, all, json); }
    Snapshot snapshot = {0};
    if (!scan(all || do_kill, &snapshot)) { free(pids); return 1; }
    int status = 0;
    if (do_kill) status = terminate_processes(&snapshot, pids, count, force, dry_run);
    else show(&snapshot, json, all);
    release_snapshot(&snapshot);
    free(pids);
    return status;
invalid:
    fputs("인자가 올바르지 않습니다. --help를 참고하세요.\n", stderr);
    free(pids);
    return 2;
}
