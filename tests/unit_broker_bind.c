#include <stdio.h>
#include <string.h>

#include "broker.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char too_long[128];

    CHECK(broker_set_bind_host(NULL) == 0);
    CHECK(broker_set_bind_host("") == 0);
    CHECK(broker_set_bind_host("127.0.0.1") == 0);

    memset(too_long, '1', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(broker_set_bind_host(too_long) < 0);

    printf("broker bind config tests passed\n");
    return 0;
}
