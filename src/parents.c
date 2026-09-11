#include "parents.h"
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

bool process_identify(pid_t pid, struct proc_bsdinfo *info) {
    memset(info, 0, sizeof(*info));
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, info, sizeof(*info)) == sizeof(*info)) return true;
    // KERN_PROC_PID can expose basic ancestry when libproc's extended info is restricted.
    struct kinfo_proc process = {0};
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t size = sizeof(process);
    if (sysctl(mib, 4, &process, &size, NULL, 0) != 0 || size != sizeof(process) ||
        process.kp_proc.p_pid != pid) return false;
    memset(info, 0, sizeof(*info));
    info->pbi_pid = pid;
    info->pbi_ppid = process.kp_eproc.e_ppid;
    info->pbi_uid = process.kp_eproc.e_ucred.cr_uid;
    info->pbi_ruid = process.kp_eproc.e_pcred.p_ruid;
    info->pbi_start_tvsec = process.kp_proc.p_starttime.tv_sec;
    info->pbi_start_tvusec = process.kp_proc.p_starttime.tv_usec;
    strlcpy(info->pbi_name, process.kp_proc.p_comm, sizeof(info->pbi_name));
    return true;
}

static bool same_identity(const struct proc_bsdinfo *a, const struct proc_bsdinfo *b) {
    return a->pbi_pid == b->pbi_pid && a->pbi_ppid == b->pbi_ppid &&
        a->pbi_start_tvsec == b->pbi_start_tvsec && a->pbi_start_tvusec == b->pbi_start_tvusec;
}

void parents_read(pid_t pid, const struct proc_bsdinfo *identity, Parents *out) {
    memset(out, 0, sizeof(*out));
    struct proc_bsdinfo child = *identity;
    while (out->count < MAX_PARENTS && child.pbi_ppid > 0) {
        pid_t parent_pid = (pid_t)child.pbi_ppid;
        if (parent_pid == pid) break;
        bool cycle = false;
        for (size_t i = 0; i < out->count; ++i) if (out->items[i].pid == parent_pid) cycle = true;
        if (cycle) break;
        struct proc_bsdinfo parent, after;
        if (!process_identify(parent_pid, &parent)) break;
        if (parent.pbi_start_tvsec > child.pbi_start_tvsec ||
            (parent.pbi_start_tvsec == child.pbi_start_tvsec && parent.pbi_start_tvusec > child.pbi_start_tvusec)) break;
        Parent link = {.pid = parent_pid, .start_sec = parent.pbi_start_tvsec,
                       .start_usec = parent.pbi_start_tvusec};
        if (proc_pidpath(parent_pid, link.path, sizeof(link.path)) > 0) {
            const char *base = strrchr(link.path, '/');
            strlcpy(link.name, base ? base + 1 : link.path, sizeof(link.name));
        } else {
            link.path[0] = 0;
            strlcpy(link.name, parent.pbi_name[0] ? parent.pbi_name : parent.pbi_comm, sizeof(link.name));
        }
        if (!process_identify(parent_pid, &after) || !same_identity(&parent, &after) ||
            !process_identify((pid_t)child.pbi_pid, &after) || !same_identity(&child, &after)) break;
        out->items[out->count++] = link;
        child = parent;
        if (parent_pid == 1) break;
    }
    struct proc_bsdinfo after;
    if (!process_identify(pid, &after) || !same_identity(identity, &after)) out->count = 0;
}
