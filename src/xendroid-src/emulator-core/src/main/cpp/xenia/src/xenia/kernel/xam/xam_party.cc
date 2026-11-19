/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// XPARTY_USER_LIST: a count padded to 8 bytes and 32 XPARTY_USER_INFO of
// 0x78 bytes.
constexpr size_t kPartyUserListSize = 0xF08;

dword_result_t XamPartyGetUserList_entry(dword_t player_count,
                                         lpdword_t party_list) {
  // Titles read the count without checking the result.
  if (party_list) {
    kernel_memory()->Zero(party_list.guest_address(), kPartyUserListSize);
  }
  // 5345085D wants specifically this code to skip loading party data.
  // This code is not documented in NT_STATUS code list
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartyGetUserList, kNone, kStub);

// The internal list has the same count and 32 users of 0x80 bytes.
constexpr size_t kPartyUserListInternalSize = 0x1008;

dword_result_t XamPartyGetUserListInternal_entry(lpvoid_t party_list_ptr) {
  if (party_list_ptr) {
    party_list_ptr.Zero(kPartyUserListInternalSize);
  }
  // FFFE07D1
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartyGetUserListInternal, kNone, kStub);

dword_result_t XamPartySendGameInvites_entry(dword_t r3, dword_t r4,
                                             dword_t r5) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamPartySendGameInvites, kNone, kStub);

dword_result_t XamPartySetCustomData_entry(dword_t r3, dword_t r4, dword_t r5) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamPartySetCustomData, kNone, kStub);

dword_result_t XamPartyGetBandwidth_entry(dword_t r3, dword_t r4) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamPartyGetBandwidth, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Party);
