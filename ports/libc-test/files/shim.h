/* Included before every libc-test source (-include): the tests' tmpfile
 * is the runner's (lct.h), the one thing the tests are built differently
 * for. */
#define tmpfile lct_tmpfile
