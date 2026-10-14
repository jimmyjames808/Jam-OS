/* cr-hello: the C++ half of milestone CR-a (docs/CR-PLAN.md, track CT1):
 * one program that uses what a C++ program takes for granted, built by
 * tools/jamos-clang++ against LLVM's runtimes (ports/llvm-runtimes), run
 * on Jam OS from /data. Its output is the same on any C++ system (nothing
 * it prints depends on the library's own choices: no addresses, no hash
 * orders, no messages the library words), so tools/cr-test.sh builds it
 * for the build host too and compares both with expected.txt.
 *
 * The three files are the parts that need more than one file: an
 * exception thrown in throw.cc is caught in main.cc, and ctors.cc has
 * static constructors of its own. */
#ifndef CR_HELLO_H
#define CR_HELLO_H

#include <stdexcept>
#include <string>
#include <vector>

/* An exception type of our own, thrown in one file and caught in another. */
struct HelloError : std::runtime_error {
    int depth;
    HelloError(const std::string &what, int depth) : std::runtime_error(what), depth(depth) {}
};

/* throw.cc: recurse depth calls deep, then throw; each frame holds a guard
 * whose destructor counts into unwound as the exception passes. */
void throw_at_depth(int depth, int &unwound);

/* ctors.cc: what the static constructors of every file wrote, in the order
 * they ran, and a value its dynamic initialisation computed. */
std::vector<std::string> &ctor_log();
int ctor_value();

#endif
