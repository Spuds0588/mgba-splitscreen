/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_RENDEZVOUS_H
#define GBA_SIO_RENDEZVOUS_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/lockstep.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/sio.h>
#include <mgba-util/circle-buffer.h>
#include <mgba-util/table.h>
#include <mgba-util/threading.h>

#define MAX_LOCKSTEP_EVENTS 8

enum GBASIORendezvousEventType {
	SIO_EV_ATTACH,
	SIO_EV_DETACH,
	SIO_EV_HARD_SYNC,
	SIO_EV_MODE_SET,
	SIO_EV_TRANSFER_START,
};

struct GBASIORendezvousCoordinator {
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
};

struct GBASIORendezvousEvent {
	enum GBASIORendezvousEventType type;
	int32_t timestamp;
	struct GBASIORendezvousEvent* next;
	int playerId;
	union {
		enum GBASIOMode mode;
		int32_t finishCycle;
	};
};

struct GBASIORendezvousPlayer {
	struct GBASIORendezvousDriver* driver;
	int playerId;
	enum GBASIOMode mode;
	enum GBASIOMode otherModes[MAX_GBAS];
	bool asleep;
	int32_t cycleOffset;
	struct GBASIORendezvousEvent* queue;
	bool dataReceived;

	struct GBASIORendezvousEvent buffer[MAX_LOCKSTEP_EVENTS];
	struct GBASIORendezvousEvent* freeList;
};

struct GBASIORendezvousDriver {
	struct GBASIODriver d;
	struct GBASIORendezvousCoordinator* coordinator;
	struct mTimingEvent event;
	unsigned lockstepId;

	struct mLockstepUser* user;
};

void GBASIORendezvousCoordinatorInit(struct GBASIORendezvousCoordinator*);
void GBASIORendezvousCoordinatorDeinit(struct GBASIORendezvousCoordinator*);

void GBASIORendezvousCoordinatorAttach(struct GBASIORendezvousCoordinator*, struct GBASIORendezvousDriver*);
void GBASIORendezvousCoordinatorDetach(struct GBASIORendezvousCoordinator*, struct GBASIORendezvousDriver*);
size_t GBASIORendezvousCoordinatorAttached(struct GBASIORendezvousCoordinator*);

void GBASIORendezvousDriverCreate(struct GBASIORendezvousDriver*, struct mLockstepUser*);

CXX_GUARD_END

#endif
