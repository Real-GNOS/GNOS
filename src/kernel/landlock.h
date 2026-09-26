/*
 * landlock.h — the landlock sandbox syscalls and process_mrelease. (GPLv2)
 */
#ifndef GNUCOS_LANDLOCK_H
#define GNUCOS_LANDLOCK_H

#include <stdint.h>

int64_t sys_landlock_create_ruleset(uint64_t attr, uint64_t size, uint64_t flags);
int64_t sys_landlock_add_rule(uint64_t ruleset_fd, uint64_t rule_type,
                              uint64_t rule_attr, uint64_t flags);
int64_t sys_landlock_restrict_self(uint64_t ruleset_fd, uint64_t flags);
int64_t sys_process_mrelease(uint64_t pidfd, uint64_t flags);

#endif
