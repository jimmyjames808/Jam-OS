/* cr-hello's static constructors (hello.h): two objects whose
 * constructors run before main, in this file's order, and a value made by
 * dynamic initialisation. The log is a function's static, so it is there
 * whichever file's constructors run first. */
#include "hello.h"

std::vector<std::string> &ctor_log()
{
    static std::vector<std::string> log;
    return log;
}

namespace {
struct Recorder {
    explicit Recorder(const char *name) { ctor_log().push_back(name); }
};

Recorder first("ctors.cc first");
Recorder second("ctors.cc second");

int sum_of_squares(int n)
{
    int v = 0;
    for (int i = 1; i <= n; i++)
        v += i * i;
    return v;
}
int computed = sum_of_squares(10);   /* 385, at start-up */
}

int ctor_value()
{
    return computed;
}
