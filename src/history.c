#include "history.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static bool relevant_type(const char *type, bool all) {
    return !strcmp(type, "UserIsActive") ||
        !strcmp(type, "PreventUserIdleDisplaySleep") ||
        !strcmp(type, "NoDisplaySleepAssertion") ||
        (all && (!strcmp(type, "PreventUserIdleSystemSleep") ||
                 !strcmp(type, "PreventSystemSleep") || !strcmp(type, "NoIdleSleepAssertion")));
}

static void free_event(HistoryEvent *event) {
    free(event->process);
    free(event->action);
    free(event->type);
    free(event->name);
}

static bool append_event(History *history, HistoryEvent *event) {
    if (!event->process || !event->action || !event->type || !event->name) {
        free_event(event);
        return false;
    }
    if (history->count == history->capacity) {
        size_t capacity = history->capacity ? history->capacity * 2 : 128;
        HistoryEvent *items = realloc(history->items, capacity * sizeof(*items));
        if (!items) { free_event(event); return false; }
        history->items = items;
        history->capacity = capacity;
    }
    event->order = history->count;
    history->items[history->count++] = *event;
    return true;
}

static char *skip_space(char *p) {
    while (isspace((unsigned char)*p)) ++p;
    return p;
}

static bool parse_line(char *line, time_t from, time_t to, bool all, History *out) {
    struct tm date = {0};
    char *body = strptime(line, "%Y-%m-%d %H:%M:%S %z", &date);
    if (!body || body - line != 25) return true;
    // Respect the offset in each record, even across timezone or DST changes.
    long offset = date.tm_gmtoff;
    time_t timestamp = timegm(&date) - offset;
    if (timestamp < from || timestamp > to) return true;
    HistoryEvent event = {.timestamp = timestamp};
    memcpy(event.time, line, 25);
    event.time[25] = 0;
    body = skip_space(body);
    if (!strncmp(body, "Notification", 12) && isspace((unsigned char)body[12])) {
        char *message = skip_space(body + 12);
        const char *action = NULL;
        if (!strncmp(message, "Display is turned on", 20) &&
            (!message[20] || isspace((unsigned char)message[20]))) action = "DisplayOn";
        else if (!strncmp(message, "Display is turned off", 21) &&
                 (!message[21] || isspace((unsigned char)message[21]))) action = "DisplayOff";
        if (!action) return true;
        event.process = strdup("");
        event.action = strdup(action);
        event.type = strdup("DisplayState");
        event.name = strdup(!strcmp(action, "DisplayOn") ? "Display is turned on" : "Display is turned off");
        return append_event(out, &event);
    }
    if (strncmp(body, "Assertions", 10) || !isspace((unsigned char)body[10])) return true;
    body = skip_space(body + 10);
    if (strncmp(body, "PID ", 4)) return true;
    char *end;
    errno = 0;
    long pid = strtol(body + 4, &end, 10);
    if (errno || pid <= 0 || pid > INT_MAX || *end != '(') return true;
    char *process = end + 1;
    // Only new/activated assertions count as requests. Summary, release and timeout
    // records would otherwise count the same assertion repeatedly.
    char *created = strstr(process, ") Created ");
    char *activated = strstr(process, ") TurnedOn ");
    char *boundary = created;
    if (activated && (!boundary || activated < boundary)) boundary = activated;
    if (!boundary) return true;
    char *quoted_name = strchr(process, '"');
    if (quoted_name && boundary > quoted_name) return true;
    const char *action = boundary == created ? "Created" : "TurnedOn";
    char *type = skip_space(boundary + 2 + strlen(action));
    char *type_end = type;
    while (*type_end && !isspace((unsigned char)*type_end)) ++type_end;
    if (!*type_end) return true;
    *type_end = 0;
    if (!relevant_type(type, all)) return true;
    *boundary = 0;
    char *details = skip_space(type_end + 1);
    char *name = "";
    // Names may contain embedded quotes; the closing quote precedes duration/id.
    if (*details == '"') {
        char *last_quote = strrchr(details + 1, '"');
        if (!last_quote) return true;
        *last_quote = 0;
        name = details + 1;
    }
    event.pid = (pid_t)pid;
    event.process = strdup(process);
    event.action = strdup(action);
    event.type = strdup(type);
    event.name = strdup(name);
    return append_event(out, &event);
}

static int compare_events(const void *a, const void *b) {
    const HistoryEvent *x = a, *y = b;
    if (x->timestamp != y->timestamp) return (x->timestamp > y->timestamp) - (x->timestamp < y->timestamp);
    return (x->order > y->order) - (x->order < y->order);
}

bool history_read(FILE *stream, time_t from, time_t to, bool all, History *out) {
    char *line = NULL;
    size_t capacity = 0;
    bool success = true;
    while (getline(&line, &capacity, stream) != -1) {
        if (!parse_line(line, from, to, all, out)) { success = false; break; }
    }
    if (ferror(stream)) success = false;
    free(line);
    if (out->count > 1) qsort(out->items, out->count, sizeof(*out->items), compare_events);
    return success;
}

bool history_load(time_t from, time_t to, bool all, History *out) {
    int descriptors[2];
    if (pipe(descriptors) != 0) { perror("전원 로그 파이프 생성 실패"); return false; }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) {
        close(descriptors[0]); close(descriptors[1]);
        fprintf(stderr, "로그 실행 준비 실패: %s\n", strerror(error));
        return false;
    }
    error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    pid_t child = 0;
    char *argv[] = {"/usr/bin/pmset", "-g", "log", NULL};
    if (!error) error = posix_spawn(&child, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(descriptors[1]);
    if (error) {
        close(descriptors[0]);
        fprintf(stderr, "pmset 실행 실패: %s\n", strerror(error));
        return false;
    }
    FILE *stream = fdopen(descriptors[0], "r");
    bool success = false;
    if (stream) { success = history_read(stream, from, to, all, out); fclose(stream); }
    else close(descriptors[0]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited == -1 && errno == EINTR);
    if (!success || waited == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fputs("전원 로그 조회 실패. /usr/bin/pmset -g log 실행을 확인하세요.\n", stderr);
        return false;
    }
    return true;
}

void history_free(History *history) {
    for (size_t i = 0; i < history->count; ++i) free_event(&history->items[i]);
    free(history->items);
    memset(history, 0, sizeof(*history));
}
