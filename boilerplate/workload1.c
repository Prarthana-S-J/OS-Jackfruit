#include <stdio.h>

int main() {
    volatile long i;
    for (i = 0; i < 1000000000; i++);
    printf("CPU work done\n");
    return 0;
}
