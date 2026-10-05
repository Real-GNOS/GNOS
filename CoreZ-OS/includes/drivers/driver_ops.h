#ifndef DRIVER_OPS_H
#define DRIVER_OPS_H

struct DRIVER_OPS {
    const char *name;
    int level;
    int (*init)(void);
};

extern const struct DRIVER_OPS __drivers_start[];
extern const struct DRIVER_OPS __drivers_end[];

#define DRIVER_REGISTER(name_str, level_val, init_fn)                          \
    static const struct DRIVER_OPS __driver_##init_fn                         \
        __attribute__((used, section(".drivers"))) = {                         \
            .name = (name_str), .level = (level_val), .init = (init_fn)        \
    }

void drivers_init(int min_level, int max_level);

#endif /* DRIVER_OPS_H */
