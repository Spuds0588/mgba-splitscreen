/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LOCKSTEP_H
#define GBA_SIO_LOCKSTEP_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/lockstep.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/sio.h>
#include <mgba-util/circle-buffer.h>
#include <mgba-util/table.h>
#include <mgba-util/threading.h>

#define MAX_LOCKSTEP_EVENTS 8

#ifndef GBA_SIO_RENDEZVOUS_H
enum GBASIOLockstepEventType {
	SIO_EV_ATTACH,
	SIO_EV_DETACH,
	SIO_EV_HARD_SYNC,
	SIO_EV_MODE_SET,
	SIO_EV_TRANSFER_START,
};
#endif

struct GBASIOLockstepCoordinator {
	struct Table players;
	Mutex mutex;

	unsigned nextId;

	unsigned attachedPlayers[MAX_GBAS];
	int nAttached;
	uint32_t waiting;

	bool transferActive;
	enum GBASIOMode transferMode;

	int32_t cycle;
	int32_t nextHardSync;

	uint16_t multiData[4];
	uint32_t normalData[4];

	// Four Swords link-handshake assist (see lockstep.c): while the FS cart is
	// stuck in its FEFE-probe / value-checksum discovery cycle, echo each
	// recipient its own sent value in every slot so both games see agreement
	// and advance; hand off to raw pass-through once real payload data flows.
	// Gated on the ROM title plus the discovery data signature, so other
	// games are never touched.
	bool fsAssistChecked;   // cart title already probed (probe exactly once)
	bool fsSuppressed;      // host switched the assist AND the kick off entirely
	bool fsAssistEnabled;   // ROM identified as the Four Swords cart (lazy)
	bool fsAssistArmed;     // host says the games are at the link screen (frozen)
	bool fsAssistOn;        // currently echoing deliveries
	uint16_t fsLastEcho;    // last echoed value (for log dedup)
	uint32_t fsLogEvery;    // throttle: log every Nth armed transfer
	uint8_t fsHandshakeRounds; // consecutive discovery rounds seen
	uint8_t fsQuietRounds;  // consecutive real-data rounds since last discovery
	// FS deadlock kick (2026-09-07): while the games are stuck in the post-name
	// phase machine (neither acceptance scan can validate a block because the
	// recv histories never fill), inject a crafted -15-summing block into BOTH
	// recv buffers (state+40 AND state+44 -- the gate at EWRAM 0x02030950 swaps
	// them every scan call), set got12 + recvIdx, and reset sendIdx so the
	// games' own handlers re-send their tables. Self-gated on the FS cart +
	// the freeze signature, so it never touches another game and needs no host
	// arming; throttled by fsKickCountdown (lockstep events between attempts).
	uint32_t fsKickCountdown;
	uint32_t fsKicks;       // total kicks issued (log throttle)
	uint32_t fsKickLastRecv[MAX_GBAS]; // per-player recvIdx at last kick attempt
	// Isolation kill-switch: --fs7 (harness kick on lockstep) needs the
	// DRIVER's inline kick OFF so the two kick sources don't confound. When
	// false, the countdown block in the lockstep event loop is skipped.
	bool fsKickEnabled;
};

void GBASIOLockstepCoordinatorSetFSArmed(struct GBASIOLockstepCoordinator*, bool armed);
void GBASIOLockstepCoordinatorSetFSKickEnabled(struct GBASIOLockstepCoordinator*, bool enabled);
void GBASIOLockstepCoordinatorSetFSSuppressed(struct GBASIOLockstepCoordinator*, bool suppressed);

struct GBASIOLockstepEvent {
	enum GBASIOLockstepEventType type;
	int32_t timestamp;
	struct GBASIOLockstepEvent* next;
	int playerId;
	union {
		enum GBASIOMode mode;
		int32_t finishCycle;
	};
};

struct GBASIOLockstepPlayer {
	struct GBASIOLockstepDriver* driver;
	int playerId;
	enum GBASIOMode mode;
	enum GBASIOMode otherModes[MAX_GBAS];
	bool asleep;
	int32_t cycleOffset;
	struct GBASIOLockstepEvent* queue;
	bool dataReceived;

	struct GBASIOLockstepEvent buffer[MAX_LOCKSTEP_EVENTS];
	struct GBASIOLockstepEvent* freeList;
};

struct GBASIOLockstepDriver {
	struct GBASIODriver d;
	struct GBASIOLockstepCoordinator* coordinator;
	struct mTimingEvent event;
	unsigned lockstepId;

	struct mLockstepUser* user;
};

void GBASIOLockstepCoordinatorInit(struct GBASIOLockstepCoordinator*);
void GBASIOLockstepCoordinatorDeinit(struct GBASIOLockstepCoordinator*);

void GBASIOLockstepCoordinatorAttach(struct GBASIOLockstepCoordinator*, struct GBASIOLockstepDriver*);
void GBASIOLockstepCoordinatorDetach(struct GBASIOLockstepCoordinator*, struct GBASIOLockstepDriver*);
size_t GBASIOLockstepCoordinatorAttached(struct GBASIOLockstepCoordinator*);

void GBASIOLockstepDriverCreate(struct GBASIOLockstepDriver*, struct mLockstepUser*);

CXX_GUARD_END

#endif
