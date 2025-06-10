#include <stdlib.h>
#include <stdio.h>

int main() {
    size_t N = 1 << 24;
    double *a = malloc(N * sizeof(double));
    for (size_t r = 0; r < 5; ++r) {
        for (size_t i = 0; i < N; i++) a[i] += 1.0;
    }
    return 0;
}
