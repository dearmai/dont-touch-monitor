#include "../src/parents.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    struct proc_bsdinfo info;
    assert(process_identify(getpid(), &info));
    assert(info.pbi_ppid == (unsigned)getppid());
    Parents parents;
    parents_read(getpid(), &info, &parents);
    assert(parents.count > 0);
    assert(parents.items[0].pid == getppid());
    assert(parents.items[0].name[0]);
    for (size_t i = 0; i < parents.count; ++i) {
        assert(parents.items[i].pid != getpid());
        for (size_t j = 0; j < i; ++j) assert(parents.items[i].pid != parents.items[j].pid);
    }
    // A reused PID or stale identity must not inherit a newly observed parent.
    info.pbi_start_tvsec -= 1;
    parents_read(getpid(), &info, &parents);
    assert(parents.count == 0);
    assert(process_identify(getpid(), &info));
    info.pbi_ppid = 1;
    if (getppid() != 1) {
        parents_read(getpid(), &info, &parents);
        assert(parents.count == 0);
    }
    puts("Parent lookup: live ancestry and stale identity checks passed");
    return 0;
}
