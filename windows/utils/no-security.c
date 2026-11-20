/*
 * No-security flavour of the platform security interface: used on
 * Win32s, where the security APIs (OpenProcess, OpenProcessToken,
 * GetTokenInformation, AllocateAndInitializeSid, InitializeAcl,
 * InitializeSecurityDescriptor, and all the ACEs) are unimplemented.
 *
 * The callers all degrade gracefully when the security functions fail:
 * the named-pipe server simply refuses remote clients, Pageant skips
 * the caller-identity check on WM_COPYDATA, and the -restrict-acl
 * request goes unanswered (Win32s has no process ACLs at all).
 */

#include "putty.h"
#include "security-api.h"

PSID get_user_sid(void)
{
    return NULL;
}

bool get_object_owner(HANDLE h, PSID *owner, PSECURITY_DESCRIPTOR *psd)
{
    return false;
}

bool make_private_security_descriptor(
    DWORD permissions, PSECURITY_DESCRIPTOR *psd, PACL *acl, char **error)
{
    *psd = NULL;
    *acl = NULL;
    *error = dupstr("security descriptors are not supported on Win32s");
    return false;
}

bool restricted_acl(void)
{
    return false;
}

void restrict_process_acl(void)
{
    /* Win32s has no process ACLs to restrict */
}
