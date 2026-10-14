/* cr-hello's thrower (hello.h): the exception crosses this file's frames
 * and the file boundary, running each frame's destructor on the way. */
#include "hello.h"

namespace {
struct Guard {
    int &unwound;
    ~Guard() { unwound++; }
};
}

void throw_at_depth(int depth, int &unwound)
{
    Guard guard{unwound};
    if (depth == 0)
        throw HelloError("deep", depth);
    throw_at_depth(depth - 1, unwound);
}
