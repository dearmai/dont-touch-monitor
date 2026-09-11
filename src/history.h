#ifndef HISTORY_H
#define HISTORY_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

typedef struct {
    time_t timestamp;
    size_t order;
    char time[32];
    pid_t pid;  // Zero for display notifications, which do not identify a process.
    char *process;
    char *action;
    char *type;
    char *name;
} HistoryEvent;

typedef struct {
    HistoryEvent *items;
    size_t count;
    size_t capacity;
} History;

bool history_read(FILE *stream, time_t from, time_t to, bool all, History *out);
bool history_load(time_t from, time_t to, bool all, History *out);
void history_free(History *history);

#endif
