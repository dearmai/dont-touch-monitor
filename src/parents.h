#ifndef PARENTS_H
#define PARENTS_H

#include <stdbool.h>
#include <libproc.h>
#include <sys/proc_info.h>

#define MAX_PARENTS 8

typedef struct {
    pid_t pid;
    uint64_t start_sec;
    uint64_t start_usec;
    char name[PROC_PIDPATHINFO_MAXSIZE];
    char path[PROC_PIDPATHINFO_MAXSIZE];
} Parent;

typedef struct {
    Parent items[MAX_PARENTS];
    size_t count;
} Parents;

bool process_identify(pid_t pid, struct proc_bsdinfo *info);
void parents_read(pid_t pid, const struct proc_bsdinfo *identity, Parents *out);

#endif
