#include <stdlib.h>
#include <string.h>

int main() {
    while (1) {
        void *p = malloc(1024 * 1024);
        if (!p) break;
        memset(p, 0, 1024 * 1024);
    }
    return 0;
}
