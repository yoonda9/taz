/* Cross-platform helper child process for the DETACH FAILED tests: exits
 * immediately with a caller-chosen status (argv[1], 0 when absent), with no
 * shell involved. Test-only, not installed. */

#include <stdlib.h>

int main(int argc, char **argv)
{
    return (argc > 1) ? atoi(argv[1]) : 0;
}
