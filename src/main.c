#include "../include/compositor.h"
#include <stdio.h>

int main(void) {
setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);
setvbuf(stdout, NULL, _IOLBF, 0);
    Compositor comp = {0};

    if (!compositor_init(&comp)) {
        return 1;
    }

    compositor_run(&comp);
    compositor_shutdown(&comp);
    return 0;
}
