/* cr-hello (hello.h): one section a line group, each printing what it
 * found; a check that fails prints "cr-hello: FAIL <what>" and the exit
 * status is 1. The last line is "cr-hello: ok".
 *
 *   cr-hello [directory]     the filesystem section's scratch directory
 *                            (default /data/cr-hello; made and removed)
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "hello.h"

namespace fs = std::filesystem;

static std::atomic<int> failures;

static void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cout << "cr-hello: FAIL " << what << std::endl;
        failures++;
    }
}

namespace {
struct Recorder {
    explicit Recorder(const char *name) { ctor_log().push_back(name); }
};
Recorder main_global("main.cc global");
}

/* ---- static constructors ------------------------------------------------ */

static void section_ctors()
{
    std::vector<std::string> log = ctor_log();
    auto a = std::find(log.begin(), log.end(), "ctors.cc first");
    auto b = std::find(log.begin(), log.end(), "ctors.cc second");
    check(a != log.end() && b != log.end() && a < b, "a file's constructors in its order");
    std::sort(log.begin(), log.end());   /* the files' order is the linker's */
    std::cout << "ctors: " << log.size() << " ran:";
    for (const auto &s : log)
        std::cout << " [" << s << "]";
    std::cout << "\n";
    std::cout << "ctors: dynamic initialisation " << ctor_value() << "\n";
    check(ctor_value() == 385, "dynamic initialisation");
}

/* ---- exceptions ---------------------------------------------------------- */

static void section_exceptions()
{
    int unwound = 0;
    try {
        throw_at_depth(5, unwound);
        check(false, "throw_at_depth returned");
    } catch (const HelloError &e) {
        std::cout << "exceptions: caught " << e.what() << " at depth " << e.depth << ", "
                  << unwound << " frames unwound\n";
        check(unwound == 6, "every frame's destructor ran");
    }
    try {
        try {
            throw std::out_of_range("range");
        } catch (const std::logic_error &e) {   /* by a base class */
            std::cout << "exceptions: caught " << e.what() << " as logic_error\n";
            throw;
        }
    } catch (const std::exception &e) {
        std::cout << "exceptions: rethrown " << e.what() << "\n";
    }
    try {
        throw 42;
    } catch (int v) {
        std::cout << "exceptions: int " << v << "\n";
    }
    try {
        std::vector<int> v(3);
        (void)v.at(10);
        check(false, "vector::at past the end");
    } catch (const std::out_of_range &) {
        std::cout << "exceptions: vector::at threw out_of_range\n";
    }
    try {
        /* Through a volatile pointer, or the compiler may leave the
         * allocation out (it is never used). */
        static char *volatile kept_pointer;
        volatile size_t huge = size_t(1) << 62;
        kept_pointer = new char[huge];
        delete[] kept_pointer;
        check(false, "new of 4 EiB");
    } catch (const std::bad_alloc &) {
        std::cout << "exceptions: new of 4 EiB threw bad_alloc\n";
    }
    std::exception_ptr kept;
    try {
        throw std::runtime_error("kept");
    } catch (...) {
        kept = std::current_exception();
    }
    try {
        std::rethrow_exception(kept);
    } catch (const std::runtime_error &e) {
        std::cout << "exceptions: exception_ptr rethrew " << e.what() << "\n";
    }
    bool caught_in_noexcept_lambda = false;
    auto f = [&]() noexcept {
        try {
            throw HelloError("inner", 0);
        } catch (const HelloError &) {
            caught_in_noexcept_lambda = true;
        }
    };
    f();
    check(caught_in_noexcept_lambda, "a throw caught inside a noexcept function");
}

/* ---- threads -------------------------------------------------------------- */

static std::atomic<int> per_thread_gone;
struct PerThread {
    long sum = 0;
    bool used = false;
    ~PerThread()
    {
        if (used)
            per_thread_gone++;
    }
};
static thread_local PerThread per_thread;
static thread_local int tl_plain = 7;

static std::atomic<int> once_made;
struct MadeOnce {
    int value;
    MadeOnce() : value(1234)
    {
        once_made++;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));   /* others wait */
    }
};
static int made_once_value()
{
    static MadeOnce m;   /* a guarded static: one thread makes it, the rest wait */
    return m.value;
}

static void section_threads()
{
    const int n = 8;
    const long span = 100000;
    std::vector<long> sums(n);
    std::vector<int> once_seen(n);
    std::mutex m;
    std::condition_variable cv;
    int arrived = 0;
    std::atomic<long> total{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < n; t++) {
        threads.emplace_back([&, t] {
            per_thread.used = true;
            for (long i = t * span; i < (t + 1) * span; i++)
                per_thread.sum += i;
            tl_plain += t;   /* each thread its own: 7 + t */
            sums[t] = per_thread.sum;
            check(tl_plain == 7 + t, "a thread_local int is the thread's own");
            total += per_thread.sum;
            once_seen[t] = made_once_value();
            std::unique_lock<std::mutex> lock(m);   /* a barrier: all 8 alive at once */
            arrived++;
            cv.notify_all();
            cv.wait(lock, [&] { return arrived == n; });
        });
    }
    for (auto &th : threads)
        th.join();
    std::cout << "threads: " << n << " joined, total " << total.load() << "\n";
    check(total == (n * span - 1) * (n * span) / 2, "the threads' sums");
    for (int t = 0; t < n; t++)
        check(sums[t] == (2 * t * span + span - 1) * span / 2, "thread " + std::to_string(t));
    std::cout << "threads: thread_local destructors ran " << per_thread_gone.load()
              << ", main's thread_local " << tl_plain << "\n";
    check(per_thread_gone == n && tl_plain == 7, "thread_local");
    std::cout << "threads: a guarded static made " << once_made.load() << " time(s), value "
              << once_seen[n - 1] << "\n";
    check(once_made == 1 && std::all_of(once_seen.begin(), once_seen.end(),
                                        [](int v) { return v == 1234; }), "guarded static");

    std::exception_ptr carried;
    std::thread thrower([&] {
        try {
            throw HelloError("from a thread", 1);
        } catch (...) {
            carried = std::current_exception();
        }
    });
    thrower.join();
    try {
        std::rethrow_exception(carried);
    } catch (const HelloError &e) {
        std::cout << "threads: carried " << e.what() << " to main\n";
    }

    /* std::atomic's wait and notify (a futex underneath on Linux). */
    std::atomic<int> flag{0};
    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        flag.store(1);
        flag.notify_one();
    });
    flag.wait(0);
    waker.join();
    std::cout << "threads: atomic wait woke with " << flag.load() << "\n";

    std::once_flag once;
    int calls = 0;
    std::vector<std::thread> callers;
    for (int t = 0; t < 4; t++)
        callers.emplace_back([&] { std::call_once(once, [&] { calls++; }); });
    for (auto &th : callers)
        th.join();
    std::cout << "threads: call_once ran " << calls << " time(s)\n";
}

/* ---- iostreams ------------------------------------------------------------ */

static void section_iostreams()
{
    std::ostringstream os;
    os << std::setw(6) << 42 << '|' << std::hex << 255 << std::dec << '|' << std::fixed
       << std::setprecision(3) << 3.14159 << '|' << std::scientific << 12345.678 << '|'
       << std::boolalpha << true << '|' << std::setfill('*') << std::left << std::setw(5) << "ab"
       << '|';
    std::cout << "iostreams: [" << os.str() << "]\n";
    std::istringstream is("17 -3 2.5 word");
    int a = 0, b = 0;
    double c = 0;
    std::string w;
    is >> a >> b >> c >> w;
    std::cout << "iostreams: read " << a + b << " " << c * 2 << " " << w << "\n";
    std::cout << "iostreams: to_string " << std::to_string(1234567) << ", stoi "
              << std::stoi("-89") << ", stod " << std::stod("0.25") << "\n";
    std::cout.flush();
    std::printf("iostreams: printf after cout, in order\n");
    std::fflush(stdout);
}

/* ---- the filesystem ------------------------------------------------------- */

static void section_filesystem(const fs::path &root)
{
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "a" / "b");
    {
        std::ofstream one(root / "a" / "one.txt");
        one << "one\n";
        std::ofstream two(root / "a" / "b" / "two.txt");
        two << std::string(5000, 'x');
    }
    fs::rename(root / "a" / "one.txt", root / "a" / "uno.txt");
    std::vector<std::string> names;
    std::string prefix = root.generic_string() + "/";
    for (const auto &e : fs::recursive_directory_iterator(root)) {
        std::string p = e.path().generic_string();
        names.push_back(p.substr(prefix.size()) + (e.is_directory() ? "/" : ""));
    }
    std::sort(names.begin(), names.end());
    std::cout << "filesystem:";
    for (const auto &s : names)
        std::cout << " " << s;
    std::cout << "\n";
    std::ifstream in(root / "a" / "uno.txt");
    std::string line;
    std::getline(in, line);
    std::cout << "filesystem: uno.txt says " << line << ", two.txt is "
              << fs::file_size(root / "a" / "b" / "two.txt") << " bytes, one.txt "
              << (fs::exists(root / "a" / "one.txt") ? "is" : "is not") << " there\n";
    check(line == "one", "the renamed file's text");
    in.close();
    auto removed = fs::remove_all(root);
    std::cout << "filesystem: remove_all took " << removed << ", the directory "
              << (fs::exists(root) ? "is" : "is not") << " there\n";
    check(removed == 5 && !fs::exists(root), "remove_all");
}

/* ---- the library's other parts -------------------------------------------- */

static void section_library()
{
    std::map<std::string, int> counts;
    for (const char *wd : {"pear", "apple", "pear", "fig", "apple", "pear"})
        counts[wd]++;
    std::cout << "library: map";
    for (const auto &kv : counts)
        std::cout << " " << kv.first << "=" << kv.second;
    std::cout << "\n";
    std::vector<int> v{5, 3, 9, 1, 7};
    std::sort(v.begin(), v.end(), std::greater<int>());
    std::cout << "library: sorted";
    for (int x : v)
        std::cout << " " << x;
    std::cout << "\n";
    auto shared = std::make_shared<std::string>("shared");
    auto copy = shared;
    std::unique_ptr<int[]> arr(new int[4]{1, 2, 3, 4});
    std::function<int(int)> twice = [](int x) { return 2 * x; };
    std::cout << "library: use_count " << shared.use_count() << ", unique " << arr[3]
              << ", function " << twice(21) << "\n";
    std::mt19937 gen(5489u);   /* the standard fixes mt19937's sequence */
    std::cout << "library: mt19937 " << gen() << " " << gen() << "\n";
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto slept = std::chrono::steady_clock::now() - t0;
    check(slept >= std::chrono::milliseconds(10), "sleep_for and steady_clock");
    std::cout << "library: slept at least 10 ms\n";
}

int main(int argc, char **argv)
{
    std::cout << "cr-hello: start\n";
    section_ctors();
    section_exceptions();
    section_threads();
    section_iostreams();
    section_filesystem(argc > 1 ? argv[1] : "/data/cr-hello");
    section_library();
    if (failures) {
        std::cout << "cr-hello: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "cr-hello: ok\n";
    return 0;
}
