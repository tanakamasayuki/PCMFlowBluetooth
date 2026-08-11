/* Platform hooks for the vendored SBC codec.
 *
 * decoder/include/oi_osinterface.h declares a handful of services that the
 * original BLUEmagic stack supplied. Bluedroid provides them from its own
 * OS layer; we are not linking Bluedroid, so we provide them here.
 *
 * The decoder only ever reaches these on a genuine internal error, which in
 * this library's usage means a malformed frame that got past the framing
 * checks. Terminating the program would be the wrong answer on a device that
 * is streaming audio, so we do not: A2dpSinkStream already treats a failed
 * decode as a counted, recoverable event and resynchronizes.
 *
 * See SPEC.md §11.3.
 */

/* sbc_config.h must come first: it carries the pcmflowbt_ renames, and these
 * definitions have to end up under the same names the vendored callers were
 * compiled against. */
#include "sbc_config.h"

#include "sbc/decoder/include/oi_stddefs.h"
#include "sbc/decoder/include/oi_status.h"
#include "sbc/decoder/include/oi_modules.h"

void OI_FatalError(OI_STATUS reason)
{
    /* Deliberately non-fatal. The caller's return value already carries the
     * failure; A2dpSinkStream counts it and resynchronizes. */
    (void)reason;
}

void OI_LogError(OI_MODULE module, OI_INT lineno, OI_STATUS status)
{
    (void)module;
    (void)lineno;
    (void)status;
}

void OI_InitDebugCodeHandler(void)
{
}
