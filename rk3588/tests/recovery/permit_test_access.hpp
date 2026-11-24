#ifndef RECOVERY_PERMIT_TEST_ACCESS_HPP
#define RECOVERY_PERMIT_TEST_ACCESS_HPP

#include "recovery/types.hpp"

namespace recovery {

class PermitHandleTestAccess {
public:
    static PermitHandle issue(RecoveryInstanceId recovery_id,
                              TransactionHash transaction_hash,
                              ActionId action_id,
                              std::uint64_t issue_nonce) {
        return PermitHandle(recovery_id, transaction_hash, action_id, issue_nonce);
    }

    static bool matches(const PermitHandle& permit,
                        const RecoveryInstanceId& recovery_id,
                        const TransactionHash& transaction_hash,
                        ActionId action_id,
                        std::uint64_t issue_nonce) noexcept {
        return permit.valid() && permit.recovery_id_ == recovery_id &&
               permit.transaction_hash_ == transaction_hash &&
               permit.action_id_ == action_id && permit.issue_nonce_ == issue_nonce;
    }

    static void consume(PermitHandle& permit) noexcept {
        permit.action_id_ = 0;
        permit.issue_nonce_ = 0;
    }
};

}  // namespace recovery

#endif
