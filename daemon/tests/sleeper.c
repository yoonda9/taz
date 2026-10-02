/* Cross-platform helper child process for exec timeout/cancel tests: sleeps
 * far longer than any test's timeout, with no shell involved. Test-only, not
 * installed. */

#include <uv.h>

int main(void)
{
    uv_sleep(60000U);
    return 0;
}
