/* Exercise Ramulator2's posted-write, split-line, retry, and drain paths. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const size_t bytes = 4u * 1024u * 1024u;
    const size_t words = bytes / sizeof(uint64_t);
    uint64_t *data = aligned_alloc(64, bytes);
    if (!data)
        return 2;
    for (uint64_t pass = 1; pass <= 4; ++pass)
        for (size_t i = 0; i < words; ++i)
            data[i] = (i * UINT64_C(0x9e3779b97f4a7c15)) ^ pass;
    uint64_t checksum = 0;
    for (size_t i = 0; i < words; i += 8)
        checksum ^= data[i];
    printf("checksum=%016llx\n", (unsigned long long)checksum);
    free(data);
    return 0;
}
