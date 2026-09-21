#include "analysis.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const sensor_snapshot ideal_occupied = {22.0, 45.0, 1};
    const sensor_snapshot ideal_empty = {22.0, 45.0, 0};

    assert(comfort_score(NULL) == -1.0);
    assert(comfort_score(&ideal_occupied) == 100.0);
    assert(comfort_score(&ideal_empty) == 100.0);

    puts("analysis tests passed");
    return 0;
}

