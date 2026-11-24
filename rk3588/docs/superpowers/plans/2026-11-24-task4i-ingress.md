# Task 4I — TrustedFaultIngress (G1–G22)

Design input: `62FE0D72…` (§4.6 algorithm, §14 types, §18A G1–G22). Host evidence runs in the
Ubuntu 24.04 g++ container; anything that depends on §19 target capabilities stays
`UNVERIFIED/NOT_RUN` until measured on the RK3588.

Slices are ordered by dependency. Each slice lands RED first, then GREEN, in its own commit.

| Slice | Scope | G | Status |
| --- | --- | --- | --- |
| G1 | session reader: canonical frame codec, receive+authorize in one step, PEER_EOF vs IO_ERROR | G1 | done |
| S1 | identity and type boundary: LinuxBootId / CoordinatorBootId / StoreDomainId strong types, `/proc` boot-id parser, 16-byte encoding, `DedupKey`, `AuthorizationDeliveryId`, private `report_fault` / `fail_closed_from_ingress` | G11, G19 (①③; ② kernel-call half waits for S3) | done |
| S2 | `FaultJournalStore` wrapper (UNVERIFIED/READY/POISONED), authority advance, `write_prepared` / exact `write_delivered`, startup scan + old-boot reclaim with tombstones, `JournalStartupSnapshot` / `StaleAuditSummary`, in-memory and file impls | G10, G13, G16, G21, store half of G3 | next |
| S3 | `create()` + `handle_session` / `handle_frame`: fixed slots and locked rollback, live-first lookup, owner / waiter / `DeliveredAckPermit`, `dispatch_ack` | G2, G3, G4, G5, G9, G12, G19 ② | |
| S4 | lifecycle and failure exits: RUNNING/STOPPING/STOPPED/FAILED fence, failure generation, side-effect gates, hard-watchdog child harness, shutdown slot consumption | G6, G7, G8, G18, G20 | |
| S5 | deadline scheduler: ARMING→arm-outside-lock→ARMED, CANCELLING, fire/cancel race, STALE_TICKET | G17 | |
| S6 | source registry and provenance: registration id non-reuse, spawn-time pidfd, revocation between frames, missing-capability startup failure | G14, G15, G22 | |
