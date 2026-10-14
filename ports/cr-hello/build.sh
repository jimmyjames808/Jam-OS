# cr-hello (ports/cr-hello/recipe): the three files, by jamos-clang++ (the
# port's $CXX); the output's name picks the program's list.
build() {
    $CXX -std=c++20 -O2 -Wall -Wextra -o cr-hello main.cc throw.cc ctors.cc
}
install() {
    mkdir -p "$destdir$prefix/bin" "$destdir$prefix/share/cr-hello"
    cp cr-hello "$destdir$prefix/bin/cr-hello"
    cp expected.txt "$destdir$prefix/share/cr-hello/expected.txt"
}
