/*
Test-only tinysshd entry point for the OpenSSH session-ID rekey test.
*/

#include "main.h"

int main(int argc, char **argv) {
    return main_tinysshd(argc, argv, "tinysshnoneauthd");
}
