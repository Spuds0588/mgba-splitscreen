/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "rendezvous.h"

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

#define DRIVER_ID 0x646E5276
#define DRIVER_STATE_VERSION 1
#define LOCKSTEP_INTERVAL 4096
#define UNLOCKED_INTERVAL 8192
#define HARD_SYNC_INTERVAL 0x80000
#define TARGET(P) (1 << (P))
#define TARGET_ALL 0xF
#define TARGET_PRIMARY 0x1
#define TARGET_SECONDARY ((TARGET_ALL) & ~(TARGET_PRIMARY))

DECL_BITFIELD(GBASIORendezvousSerializedFlags, uint32_t);
DECL_BITS(GBASIORendezvousSerializedFlags, DriverMode, 0, 3);
DECL_BITS(GBASIORendezvousSerializedFlags, NumEvents, 3, 4);
DECL_BIT(GBASIORendezvousSerializedFlags, Asleep, 7);
DECL_BIT(GBASIORendezvousSerializedFlags, DataReceived, 8);
DECL_BIT(GBASIORendezvousSerializedFlags, EventScheduled, 9);
DECL_BITS(GBASIORendezvousSerializedFlags, Player0Mode, 10, 3);
DECL_BITS(GBASIORendezvousSerializedFlags, Player1Mode, 13, 3);
DECL_BITS(GBASIORendezvousSerializedFlags, Player2Mode, 16, 3);
DECL_BITS(GBASIORendezvousSerializedFlags, Player3Mode, 19, 3);
DECL_BITS(GBASIORendezvousSerializedFlags, TransferMode, 28, 3);
DECL_BIT(GBASIORendezvousSerializedFlags, TransferActive, 31);

DECL_BITFIELD(GBASIORendezvousSerializedEventFlags, uint32_t);
DECL_BITS(GBASIORendezvousSerializedEventFlags, Type, 0, 3);

struct GBASIORendezvousSerializedEvent {
	int32_t timestamp;
	int32_t playerId;
	GBASIORendezvousSerializedEventFlags flags;
	int32_t reserved[5];
	union {
		int32_t mode;
		int32_t finishCycle;
		int32_t padding[4];
	};
};
static_assert(sizeof(struct GBASIORendezvousSerializedEvent) == 0x30, "GBA lockstep event savestate struct sized wrong");

struct GBASIORendezvousSerializedState {
	uint32_t version;
	GBASIORendezvousSerializedFlags flags;
	uint32_t reserved[2];

	struct {
		int32_t nextEvent;
		uint32_t reservedDriver[7];
	} driver;

	struct {
		int32_t playerId;
		int32_t cycleOffset;
		uint32_t reservedPlayer[2];
		struct GBASIORendezvousSerializedEvent events[MAX_LOCKSTEP_EVENTS];
	} player;

	// playerId 0 only
	struct {
		int32_t cycle;
		uint32_t waiting;
		int32_t nextHardSync;
		uint32_t reservedCoordinator[3];
		uint16_t multiData[4];
		uint32_t normalData[4];
	} coordinator;
};
static_assert(offsetof(struct GBASIORendezvousSerializedState, driver) == 0x10, "GBA lockstep savestate driver offset wrong");
static_assert(offsetof(struct GBASIORendezvousSerializedState, player) == 0x30, "GBA lockstep savestate player offset wrong");
static_assert(offsetof(struct GBASIORendezvousSerializedState, coordinator) == 0x1C0, "GBA lockstep savestate coordinator offset wrong");
static_assert(sizeof(struct GBASIORendezvousSerializedState) == 0x1F0, "GBA lockstep savestate struct sized wrong");

static bool GBASIORendezvousDriverInit(struct GBASIODriver* driver);
static void GBASIORendezvousDriverDeinit(struct GBASIODriver* driver);
static void GBASIORendezvousDriverReset(struct GBASIODriver* driver);
static uint32_t GBASIORendezvousDriverId(const struct GBASIODriver* driver);
static bool GBASIORendezvousDriverLoadState(struct GBASIODriver* driver, const void* state, size_t size);
static void GBASIORendezvousDriverSaveState(struct GBASIODriver* driver, void** state, size_t* size);
static void GBASIORendezvousDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static bool GBASIORendezvousDriverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static int GBASIORendezvousDriverConnectedDevices(struct GBASIODriver* driver);
static int GBASIORendezvousDriverDeviceId(struct GBASIODriver* driver);
static uint16_t GBASIORendezvousDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value);
static uint16_t GBASIORendezvousDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value);
static bool GBASIORendezvousDriverStart(struct GBASIODriver* driver);
static void GBASIORendezvousDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]);
static uint8_t GBASIORendezvousDriverFinishNormal8(struct GBASIODriver* driver);
static uint32_t GBASIORendezvousDriverFinishNormal32(struct GBASIODriver* driver);

static void GBASIORendezvousCoordinatorWaitOnPlayers(struct GBASIORendezvousCoordinator*, struct GBASIORendezvousPlayer*);
static void GBASIORendezvousCoordinatorAckPlayer(struct GBASIORendezvousCoordinator*, struct GBASIORendezvousPlayer*);
static void GBASIORendezvousCoordinatorWakePlayers(struct GBASIORendezvousCoordinator*);

static int32_t GBASIORendezvousTime(struct GBASIORendezvousPlayer*);
static void GBASIORendezvousPlayerWake(struct GBASIORendezvousPlayer*);
static void GBASIORendezvousPlayerSleep(struct GBASIORendezvousPlayer*);

static void _advanceCycle(struct GBASIORendezvousCoordinator*, struct GBASIORendezvousPlayer*);
static void _removePlayer(struct GBASIORendezvousCoordinator*, struct GBASIORendezvousPlayer*);
static void _reconfigPlayers(struct GBASIORendezvousCoordinator*);
static int32_t _untilNextSync(struct GBASIORendezvousCoordinator*, struct GBASIORendezvousPlayer*);
static void _enqueueEvent(struct GBASIORendezvousCoordinator*, const struct GBASIORendezvousEvent*, uint32_t target);
static void _setData(struct GBASIORendezvousCoordinator*, uint32_t id, struct GBASIO* sio);
static void _setReady(struct GBASIORendezvousCoordinator*, struct GBASIORendezvousPlayer* activePlayer, int playerId, enum GBASIOMode mode);
static void _hardSync(struct GBASIORendezvousCoordinator*, struct GBASIORendezvousPlayer*);

static void _rendezvousEvent(struct mTiming*, void* context, uint32_t cyclesLate);

static void _verifyAwake(struct GBASIORendezvousCoordinator* coordinator) {
#ifdef NDEBUG
	UNUSED(coordinator);
#else
	int i;
	int asleep = 0;
	for (i = 0; i < coordinator->nAttached; ++i) {
		if (!coordinator->attachedPlayers[i]) {
			continue;
		}
		struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		asleep += player->asleep;
	}
	mASSERT_DEBUG(!asleep || asleep < coordinator->nAttached);
#endif
}

static void _abortTransfer(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousPlayer* player) {
	mLOG(GBA_SIO, DEBUG, "Aborting in-progress transfer");
	// TODO: Do we need to clean this up better?
	coordinator->transferActive = false;
	coordinator->waiting = 0;

	if (player->playerId != 0) {
		struct GBASIORendezvousPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
		if (runner) {
			GBASIORendezvousPlayerWake(runner);
		}
	} else {
		GBASIORendezvousCoordinatorWakePlayers(coordinator);
	}
}

void GBASIORendezvousDriverCreate(struct GBASIORendezvousDriver* driver, struct mLockstepUser* user) {
	memset(driver, 0, sizeof(*driver));
	driver->d.init = GBASIORendezvousDriverInit;
	driver->d.deinit = GBASIORendezvousDriverDeinit;
	driver->d.reset = GBASIORendezvousDriverReset;
	driver->d.driverId = GBASIORendezvousDriverId;
	driver->d.loadState = GBASIORendezvousDriverLoadState;
	driver->d.saveState = GBASIORendezvousDriverSaveState;
	driver->d.setMode = GBASIORendezvousDriverSetMode;
	driver->d.handlesMode = GBASIORendezvousDriverHandlesMode;
	driver->d.deviceId = GBASIORendezvousDriverDeviceId;
	driver->d.connectedDevices = GBASIORendezvousDriverConnectedDevices;
	driver->d.writeSIOCNT = GBASIORendezvousDriverWriteSIOCNT;
	driver->d.writeRCNT = GBASIORendezvousDriverWriteRCNT;
	driver->d.start = GBASIORendezvousDriverStart;
	driver->d.finishMultiplayer = GBASIORendezvousDriverFinishMultiplayer;
	driver->d.finishNormal8 = GBASIORendezvousDriverFinishNormal8;
	driver->d.finishNormal32 = GBASIORendezvousDriverFinishNormal32;
	driver->event.context = driver;
	driver->event.callback = _rendezvousEvent;
	driver->event.name = "GBA SIO Lockstep";
	driver->event.priority = 0x80;
	driver->user = user;
}

static bool GBASIORendezvousDriverInit(struct GBASIODriver* driver) {
	GBASIORendezvousDriverReset(driver);
	return true;
}

static void GBASIORendezvousDriverDeinit(struct GBASIODriver* driver) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player) {
		_removePlayer(coordinator, player);
	}
	MutexUnlock(&coordinator->mutex);
	mTimingDeschedule(&lockstep->d.p->p->timing, &lockstep->event);
	lockstep->lockstepId = 0;
}

static void GBASIORendezvousDriverReset(struct GBASIODriver* driver) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	struct GBASIORendezvousPlayer* player;
	if (!lockstep->lockstepId) {
		unsigned id;
		player = calloc(1, sizeof(*player));
		player->driver = lockstep;
		player->mode = driver->p->mode;
		player->playerId = -1;

		int i;
		for (i = 0; i < MAX_LOCKSTEP_EVENTS - 1; ++i) {
			player->buffer[i].next = &player->buffer[i + 1];
		}
		player->freeList = &player->buffer[0];

		MutexLock(&coordinator->mutex);
		while (true) {
			if (coordinator->nextId == UINT_MAX) {
				coordinator->nextId = 0;
			}
			++coordinator->nextId;
			id = coordinator->nextId;
			if (!TableLookup(&coordinator->players, id)) {
				TableInsert(&coordinator->players, id, player);
				lockstep->lockstepId = id;
				break;
			}
		}
		_reconfigPlayers(coordinator);
		player->cycleOffset = mTimingCurrentTime(&driver->p->p->timing) - coordinator->cycle;
		if (player->playerId != 0) {
			struct GBASIORendezvousEvent event = {
				.type = SIO_EV_ATTACH,
				.playerId = player->playerId,
				.timestamp = GBASIORendezvousTime(player),
			};
			_enqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));
		}
	} else {
		MutexLock(&coordinator->mutex);
		player = TableLookup(&coordinator->players, lockstep->lockstepId);
		player->cycleOffset = mTimingCurrentTime(&driver->p->p->timing) - coordinator->cycle;
	}

	if (coordinator->transferActive) {
		_abortTransfer(coordinator, player);
		player->asleep = false;
	}
	if (player->playerId == 0 && coordinator->nAttached > 1) {
		coordinator->waiting = 0;
		// We will immediately go back to sleep when the initial mode gets set,
		// so we need to clear this here to avoid triggering an assert later.
		player->asleep = false;
		GBASIORendezvousCoordinatorWakePlayers(coordinator);
	}

	if (mTimingIsScheduled(&lockstep->d.p->p->timing, &lockstep->event)) {
		MutexUnlock(&coordinator->mutex);
		return;
	}

	int32_t nextEvent;
	_setReady(coordinator, player, player->playerId, player->mode);
	if (TableSize(&coordinator->players) == 1) {
		coordinator->cycle = mTimingCurrentTime(&lockstep->d.p->p->timing);
		nextEvent = LOCKSTEP_INTERVAL;
	} else {
		_setReady(coordinator, player, 0, coordinator->transferMode);
		nextEvent = _untilNextSync(lockstep->coordinator, player);
	}
	MutexUnlock(&coordinator->mutex);
	mTimingSchedule(&lockstep->d.p->p->timing, &lockstep->event, nextEvent);
}

static uint32_t GBASIORendezvousDriverId(const struct GBASIODriver* driver) {
	UNUSED(driver);
	return DRIVER_ID;
}

static unsigned _modeEnumToInt(enum GBASIOMode mode) {
	switch ((int) mode) {
	case -1:
	default:
		return 0;
	case GBA_SIO_MULTI:
		return 1;
	case GBA_SIO_NORMAL_8:
		return 2;
	case GBA_SIO_NORMAL_32:
		return 3;
	case GBA_SIO_GPIO:
		return 4;
	case GBA_SIO_UART:
		return 5;
	case GBA_SIO_JOYBUS:
		return 6;
	}
}

static enum GBASIOMode _modeIntToEnum(unsigned mode) {
	const enum GBASIOMode modes[8] = {
		-1, GBA_SIO_MULTI, GBA_SIO_NORMAL_8, GBA_SIO_NORMAL_32, GBA_SIO_GPIO, GBA_SIO_UART, GBA_SIO_JOYBUS, -1
	};
	return modes[mode & 7];
}

static bool GBASIORendezvousDriverLoadState(struct GBASIODriver* driver, const void* data, size_t size) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	if (size != sizeof(struct GBASIORendezvousSerializedState)) {
		mLOG(GBA_SIO, WARN, "Incorrect state size: expected %" PRIz "X, got %" PRIz "X", sizeof(struct GBASIORendezvousSerializedState), size);
		return false;
	}
	const struct GBASIORendezvousSerializedState* state = data;
	bool error = false;
	uint32_t ucheck;
	int32_t check;
	LOAD_32LE(ucheck, 0, &state->version);
	if (ucheck > DRIVER_STATE_VERSION) {
		mLOG(GBA_SIO, WARN, "Invalid or too new save state: expected %u, got %u", DRIVER_STATE_VERSION, ucheck);
		return false;
	}

	MutexLock(&coordinator->mutex);
	struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	LOAD_32LE(check, 0, &state->player.playerId);
	if (check != player->playerId) {
		mLOG(GBA_SIO, WARN, "State is for different player: expected %d, got %d", player->playerId, check);
		error = true;
		goto out;
	}

	GBASIORendezvousSerializedFlags flags = 0;
	LOAD_32LE(flags, 0, &state->flags);
	LOAD_32LE(player->cycleOffset, 0, &state->player.cycleOffset);
	player->dataReceived = GBASIORendezvousSerializedFlagsGetDataReceived(flags);
	player->mode = _modeIntToEnum(GBASIORendezvousSerializedFlagsGetDriverMode(flags));

	player->otherModes[0] = _modeIntToEnum(GBASIORendezvousSerializedFlagsGetPlayer0Mode(flags));
	player->otherModes[1] = _modeIntToEnum(GBASIORendezvousSerializedFlagsGetPlayer1Mode(flags));
	player->otherModes[2] = _modeIntToEnum(GBASIORendezvousSerializedFlagsGetPlayer2Mode(flags));
	player->otherModes[3] = _modeIntToEnum(GBASIORendezvousSerializedFlagsGetPlayer3Mode(flags));

	if (GBASIORendezvousSerializedFlagsGetEventScheduled(flags)) {
		int32_t when;
		LOAD_32LE(when, 0, &state->driver.nextEvent);
		mTimingSchedule(&driver->p->p->timing, &lockstep->event, when);
	}

	if (GBASIORendezvousSerializedFlagsGetAsleep(flags)) {
		if (!player->asleep && player->driver->user->sleep) {
			player->driver->user->sleep(player->driver->user);
		}
		player->asleep = true;
	} else {
		if (player->asleep && player->driver->user->wake) {
			player->driver->user->wake(player->driver->user);
		}
		player->asleep = false;
	}

	unsigned i;
	for (i = 0; i < MAX_LOCKSTEP_EVENTS - 1; ++i) {
		player->buffer[i].next = &player->buffer[i + 1];
	}
	player->freeList = &player->buffer[0];
	player->queue = NULL;

	struct GBASIORendezvousEvent** lastEvent = &player->queue;
	for (i = 0; i < GBASIORendezvousSerializedFlagsGetNumEvents(flags) && i < MAX_LOCKSTEP_EVENTS; ++i) {
		struct GBASIORendezvousEvent* event = player->freeList;
		const struct GBASIORendezvousSerializedEvent* stateEvent = &state->player.events[i];
		player->freeList = player->freeList->next;
		*lastEvent = event;
		lastEvent = &event->next;

		GBASIORendezvousSerializedEventFlags flags;
		LOAD_32LE(flags, 0, &stateEvent->flags);
		LOAD_32LE(event->timestamp, 0, &stateEvent->timestamp);
		LOAD_32LE(event->playerId, 0, &stateEvent->playerId);
		event->type = GBASIORendezvousSerializedEventFlagsGetType(flags);
		switch (event->type) {
		case SIO_EV_ATTACH:
		case SIO_EV_DETACH:
		case SIO_EV_HARD_SYNC:
			break;
		case SIO_EV_MODE_SET:
			LOAD_32LE(event->mode, 0, &stateEvent->mode);
			break;
		case SIO_EV_TRANSFER_START:
			LOAD_32LE(event->finishCycle, 0, &stateEvent->finishCycle);
			break;
		}
	}

	if (player->playerId == 0) {
		LOAD_32LE(coordinator->cycle, 0, &state->coordinator.cycle);
		LOAD_32LE(coordinator->waiting, 0, &state->coordinator.waiting);
		LOAD_32LE(coordinator->nextHardSync, 0, &state->coordinator.nextHardSync);
		for (i = 0; i < 4; ++i) {
			LOAD_16LE(coordinator->multiData[i], 0, &state->coordinator.multiData[i]);
			LOAD_32LE(coordinator->normalData[i], 0, &state->coordinator.normalData[i]);
		}
		coordinator->transferMode = _modeIntToEnum(GBASIORendezvousSerializedFlagsGetTransferMode(flags));
		coordinator->transferActive = GBASIORendezvousSerializedFlagsGetTransferActive(flags);
	}
out:
	MutexUnlock(&coordinator->mutex);
	if (!error) {
		mTimingInterrupt(&driver->p->p->timing);
	}
	return !error;
}

static void GBASIORendezvousDriverSaveState(struct GBASIODriver* driver, void** stateOut, size_t* size) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	struct GBASIORendezvousSerializedState* state = calloc(1, sizeof(*state));

	STORE_32LE(DRIVER_STATE_VERSION, 0, &state->version);

	STORE_32LE(lockstep->event.when - mTimingCurrentTime(&driver->p->p->timing), 0, &state->driver.nextEvent);

	MutexLock(&coordinator->mutex);
	struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	GBASIORendezvousSerializedFlags flags = 0;
	STORE_32LE(player->playerId, 0, &state->player.playerId);
	STORE_32LE(player->cycleOffset, 0, &state->player.cycleOffset);
	flags = GBASIORendezvousSerializedFlagsSetAsleep(flags, player->asleep);
	flags = GBASIORendezvousSerializedFlagsSetDataReceived(flags, player->dataReceived);
	flags = GBASIORendezvousSerializedFlagsSetDriverMode(flags, _modeEnumToInt(player->mode));
	flags = GBASIORendezvousSerializedFlagsSetEventScheduled(flags, mTimingIsScheduled(&driver->p->p->timing, &lockstep->event));

	flags = GBASIORendezvousSerializedFlagsSetPlayer0Mode(flags, _modeEnumToInt(player->otherModes[0]));
	flags = GBASIORendezvousSerializedFlagsSetPlayer1Mode(flags, _modeEnumToInt(player->otherModes[1]));
	flags = GBASIORendezvousSerializedFlagsSetPlayer2Mode(flags, _modeEnumToInt(player->otherModes[2]));
	flags = GBASIORendezvousSerializedFlagsSetPlayer3Mode(flags, _modeEnumToInt(player->otherModes[3]));

	struct GBASIORendezvousEvent* event = player->queue;
	size_t i;
	for (i = 0; i < MAX_LOCKSTEP_EVENTS && event; ++i, event = event->next) {
		struct GBASIORendezvousSerializedEvent* stateEvent = &state->player.events[i];
		GBASIORendezvousSerializedEventFlags flags = GBASIORendezvousSerializedEventFlagsSetType(0, event->type);
		STORE_32LE(event->timestamp, 0, &stateEvent->timestamp);
		STORE_32LE(event->playerId, 0, &stateEvent->playerId);
		switch (event->type) {
		case SIO_EV_ATTACH:
		case SIO_EV_DETACH:
		case SIO_EV_HARD_SYNC:
			break;
		case SIO_EV_MODE_SET:
			STORE_32LE(event->mode, 0, &stateEvent->mode);
			break;
		case SIO_EV_TRANSFER_START:
			STORE_32LE(event->finishCycle, 0, &stateEvent->finishCycle);
			break;
		}
		STORE_32LE(flags, 0, &stateEvent->flags);
	}
	flags = GBASIORendezvousSerializedFlagsSetNumEvents(flags, i);

	if (player->playerId == 0) {
		STORE_32LE(coordinator->cycle, 0, &state->coordinator.cycle);
		STORE_32LE(coordinator->waiting, 0, &state->coordinator.waiting);
		STORE_32LE(coordinator->nextHardSync, 0, &state->coordinator.nextHardSync);
		for (i = 0; i < 4; ++i) {
			STORE_16LE(coordinator->multiData[i], 0, &state->coordinator.multiData[i]);
			STORE_32LE(coordinator->normalData[i], 0, &state->coordinator.normalData[i]);
		}
		flags = GBASIORendezvousSerializedFlagsSetTransferMode(flags, _modeEnumToInt(coordinator->transferMode));
		flags = GBASIORendezvousSerializedFlagsSetTransferActive(flags, coordinator->transferActive);
	}
	MutexUnlock(&lockstep->coordinator->mutex);

	STORE_32LE(flags, 0, &state->flags);
	*stateOut = state;
	*size = sizeof(*state);
}

static void GBASIORendezvousDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (mode != player->mode) {
		mLOG(GBA_SIO, WARN, "Switching mode from %d to %d", player->mode, mode);
		player->mode = mode;
		struct GBASIORendezvousEvent event = {
			.type = SIO_EV_MODE_SET,
			.playerId = player->playerId,
			.timestamp = GBASIORendezvousTime(player),
			.mode = mode,
		};
		if (player->playerId == 0) {
			coordinator->transferMode = mode;
			GBASIORendezvousCoordinatorWaitOnPlayers(coordinator, player);
		}
		_setReady(coordinator, player, player->playerId, mode);
		_enqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));
	}
	MutexUnlock(&coordinator->mutex);
}

static bool GBASIORendezvousDriverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	UNUSED(mode);
	return true;
}

static int GBASIORendezvousDriverConnectedDevices(struct GBASIODriver* driver) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	if (!lockstep->lockstepId) {
		return 0;
	}
	MutexLock(&coordinator->mutex);
	int attached = coordinator->nAttached - 1;
	MutexUnlock(&coordinator->mutex);
	return attached;
}

static int GBASIORendezvousDriverDeviceId(struct GBASIODriver* driver) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	int playerId = 0;
	MutexLock(&coordinator->mutex);
	struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player && player->playerId >= 0) {
		playerId = player->playerId;
	}
	MutexUnlock(&coordinator->mutex);
	return playerId;
}

static uint16_t GBASIORendezvousDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	mLOG(GBA_SIO, WARN, "SIOCNTW pid=%d val=%04X irq=%d busy=%d start=%d",
	     GBASIOMultiplayerGetId(value) & 3, value, !!(value & 0x4000),
	     !!(value & 0x80), !!(value & 0x100));
	return value;
}

static uint16_t GBASIORendezvousDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	mLOG(GBA_SIO, DEBUG, "Lockstep: RCNT <- %04X", value);
	return value;
}

static bool GBASIORendezvousDriverStart(struct GBASIODriver* driver) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	bool ret = false;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferActive) {
		mLOG(GBA_SIO, WARN, "Transfer restarted unexpectedly (transferActive stuck)");
		goto out;
	}
	if (coordinator->nAttached < 2) {
		mLOG(GBA_SIO, WARN, "Attempted to start transfer with no secondary players");
		goto out;
	}
	struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player->playerId != 0) {
		mLOG(GBA_SIO, WARN, "Secondary player attempted to start transfer");
		goto out;
	}
	// TEMP FS-link trace: promote to WARN so the post-name freeze sequence is
	// visible at the harness's WARN log level. Revert before merging.
	mLOG(GBA_SIO, WARN, "Transfer starting at %08X (clock %08X)", GBASIORendezvousTime(player), coordinator->cycle);
	memset(coordinator->multiData, 0xFF, sizeof(coordinator->multiData));
	_setData(coordinator, 0, player->driver->d.p);

	int32_t timestamp = GBASIORendezvousTime(player);
	struct GBASIORendezvousEvent event = {
		.type = SIO_EV_TRANSFER_START,
		.timestamp = timestamp,
		.finishCycle = timestamp + GBASIOTransferCycles(player->mode, player->driver->d.p->siocnt, coordinator->nAttached - 1),
	};
	_enqueueEvent(coordinator, &event, TARGET_SECONDARY);
	GBASIORendezvousCoordinatorWaitOnPlayers(coordinator, player);
	coordinator->transferActive = true;
	ret = true;
out:
	MutexUnlock(&coordinator->mutex);
	return ret;
}

/* ---- Four Swords link-handshake assist ----
 * Mirror of lockstep.c's assist (keep in sync). See that file for the full
 * rationale; the short version: FS's discovery handshake (FEFE probes +
 * value/0xFFF1-value checksum pairs) never completes because the two games'
 * counters stay offset, so while the signature is detected we echo each
 * recipient its own sent value in every slot, making each game see agreement
 * and advance. Hand off to raw data when real payloads flow.
 */
static void _fsAssistIdentify(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousDriver* lockstep) {
	coordinator->fsAssistEnabled = false;
	struct GBA* gba = lockstep->d.p ? lockstep->d.p->p : NULL;
	if (gba && gba->memory.rom && gba->memory.romSize > 0xA4) {
		const char* title = (const char*) &gba->memory.rom[0xA0 >> 2];
		coordinator->fsAssistEnabled =
		    title[0] == 'G' && title[1] == 'B' && title[2] == 'A' && title[3] == 'Z';
	}
	if (coordinator->fsAssistEnabled) {
		mLOG(GBA_SIO, WARN, "FS assist: Four Swords cart identified");
	}
}

static bool _fsAssistHandshakeRound(const uint16_t data[4], int n) {
	int i, j;
	bool anyNonzero = false;
	for (i = 0; i < n; ++i) {
		if (data[i] == 0xFEFE) {
			return true;
		}
		if (data[i] != 0 && data[i] != 0xFFFF) {
			anyNonzero = true;
		}
	}
	if (!anyNonzero) {
		return false;
	}
	// Probe / checksum-pair signature (link screen + name handshake): FEFE
	// probes, or (value, 0xFFF1/0xFFF3-value) pairs. Pairs may be split across
	// two rounds (value round then checksum round), so also treat a lone
	// value-like (>= 0x8000) slot as a value round when a partner is nonzero.
	int values = 0;
	for (i = 0; i < n; ++i) {
		if (data[i] != 0 && data[i] != 0xFFFF && data[i] != 0xFEFE &&
		    data[i] >= 0x8000) {
			++values;
		}
	}
	if (values >= 1) {
		return true;
	}
	for (i = 0; i < n; ++i) {
		if (data[i] == 0 || data[i] == 0xFFFF) {
			continue;
		}
		for (j = i + 1; j < n; ++j) {
			if (data[j] != 0 && data[j] != 0xFFFF &&
			    ((uint32_t) data[i] + (uint32_t) data[j] == 0xFFF1 ||
			     (uint32_t) data[i] + (uint32_t) data[j] == 0xFFF3)) {
				return true;
			}
		}
	}
	// Counter-sync signature (post-name device-recognition handshake): the two
	// games exchange 16-bit counters that must match, but the slave's drifts by
	// ~0x100 (observed 0121/0021, FE4F/FF50, 0084/0083). Any round where both
	// players send small nonzero values within 0x200 of each other is a sync
	// round -- echo the master's value so both games see agreement.
	int nz = 0;
	uint16_t first = 0;
	bool sync = true;
	for (i = 0; i < n; ++i) {
		if (data[i] == 0 || data[i] == 0xFFFF || data[i] == 0xFEFE) {
			continue;
		}
		if (nz == 0) {
			first = data[i];
		} else {
			int32_t d = (int32_t) first - data[i];
			if (d < 0) {
				d = -d;
			}
			if (d > 0x200) {
				sync = false;
			}
		}
		++nz;
	}
	return sync && nz >= 2;
}

/* Correct FS checksum drift in-place on `data`. FS's second handshake
 * (post-name) exchanges (value, checksum) pairs that must sum to 0xFFF3
 * (the link screen uses 0xFFF1). The slave's checksum intermittently drifts
 * by exactly 0x100 (e.g. value FCF2 arrives with 0201 instead of 0301), so
 * the master's game rejects the pair, its recv never reaches 13, the
 * acceptance (which only fires on the master) never runs, and the games
 * deadlock at the name screen. Track each slot's last VALUE round; when a
 * CHECKSUM round arrives (all slots small, < 0x8000), rewrite any slot whose
 * pair misses 0xFFF1/0xFFF3 by a multiple of 0x100 so the pair validates.
 */
static void _fsAssistNormalize(struct GBASIORendezvousCoordinator* coordinator, uint16_t data[4]) {
	int i;
	if ((++coordinator->fsNormDebug % 100) == 1) {
		mLOG(GBA_SIO, WARN, "FS assist: normalize sees %04X %04X %04X %04X last %04X %04X %04X %04X",
		     data[0], data[1], data[2], data[3],
		     coordinator->fsLastValue[0], coordinator->fsLastValue[1],
		     coordinator->fsLastValue[2], coordinator->fsLastValue[3]);
	}
	bool checksumRound = true;
	for (i = 0; i < coordinator->nAttached; ++i) {
		uint16_t v = data[i];
		if (v == 0 || v == 0xFFFF || v == 0xFEFE) {
			continue;
		}
		if (v >= 0x8000) {
			checksumRound = false;
			break;
		}
	}
	if (checksumRound) {
		// NO-OP since 2026-09-06: the "checksum drift" this used to "fix" is
		// NOT drift -- disassembly of FS's SIO handler (0x800C658 checksum
		// builder, 0x800C6A8 acceptance scan) plus live traces show the games'
		// post-name tables are self-consistent: every (value, checksum) pair
		// the games exchange sums to EXACTLY 0xFFF2, and each game builds a
		// 12-halfword block whose own checksum entry makes the block sum to
		// 0xFFF1. The acceptance scan requires the block sum == -15 (0xFFF1),
		// so REWRITING any table entry (as this code did: 03C4->03C3, and even
		// 03FF->01FE with d=513) destroys the block checksum and the master's
		// game rejects every received block. Keep the diagnostics, never
		// rewrite. See history.md 2026-09-06.
		for (i = 0; i < coordinator->nAttached; ++i) {
			uint16_t v = data[i];
			uint16_t last = coordinator->fsLastValue[i];
			if (v == 0 || v == 0xFFFF || v == 0xFEFE || last == 0) {
				continue;
			}
			uint32_t sum = (uint32_t) v + last;
			if (sum == 0xFFF1 || sum == 0xFFF2 || sum == 0xFFF3) {
				continue;  // already valid
			}
			mLOG(GBA_SIO, WARN, "FS assist: norm observe slot %d pair %04X+%04X=%04X (no rewrite)",
			     i, last, v, (unsigned) (sum & 0xFFFF));
		}
	} else {
		// Value round: remember each slot's value for the next checksum round.
		for (i = 0; i < coordinator->nAttached; ++i) {
			uint16_t v = data[i];
			if (v != 0 && v != 0xFFFF && v != 0xFEFE && v >= 0x8000) {
				coordinator->fsLastValue[i] = v;
			}
		}
	}
}

static void _fsAssistTick(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousDriver* lockstep, uint16_t data[4]) {
	// LATCHED echo (see lockstep.c): after 3 consecutive discovery-signature
	// rounds the games are at the frozen link screen and the echo engages on
	// EVERY round (both games see agreement and their 13-cycle receive
	// counters advance together); 120 consecutive non-discovery rounds means
	// real payload is flowing and the echo hands off.
	if (!coordinator->fsAssistEnabled && !coordinator->fsAssistOn) {
		_fsAssistIdentify(coordinator, lockstep);
	}
	if (!coordinator->fsAssistEnabled) {
		return;
	}
	if (!coordinator->fsAssistArmed) {
		coordinator->fsAssistOn = false;
		coordinator->fsHandshakeRounds = 0;
		coordinator->fsQuietRounds = 0;
		return;
	}
	// Normalize checksum drift BEFORE the echo decision so the master sees
	// valid pairs; then the echo (probe rounds) still works as before.
	_fsAssistNormalize(coordinator, data);
	if (_fsAssistHandshakeRound(data, coordinator->nAttached)) {
		coordinator->fsHandshakeRounds++;
		coordinator->fsQuietRounds = 0;
		if (coordinator->fsHandshakeRounds >= 3 && !coordinator->fsAssistOn) {
			mLOG(GBA_SIO, WARN, "FS assist: engaging echo at the link screen");
		}
		coordinator->fsAssistOn = coordinator->fsHandshakeRounds >= 3;
	} else if (coordinator->fsAssistOn) {
		coordinator->fsHandshakeRounds = 0;
		coordinator->fsQuietRounds++;
		if (coordinator->fsQuietRounds >= 120) {
			coordinator->fsAssistOn = false;
			coordinator->fsQuietRounds = 0;
			mLOG(GBA_SIO, WARN, "FS assist: handed off to raw data");
		}
	} else {
		coordinator->fsHandshakeRounds = 0;
	}
	coordinator->fsLastEcho = coordinator->fsAssistOn ? data[0] : 0;
}

static void GBASIORendezvousDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferMode == GBA_SIO_MULTI) {
		struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (!player->dataReceived) {
			mLOG(GBA_SIO, WARN, "MULTI did not receive data. Are we running behind?");
			// FS stall assist: mirror of lockstep.c -- while the assist is armed,
			// echo the primary's own sent value in every slot instead of FFFF so
			// the master's game sees consistent partner data on stalled rounds.
			if (coordinator->fsAssistArmed) {
				uint16_t master = coordinator->multiData[0];
				int k;
				for (k = 0; k < 4; ++k) {
					data[k] = master;
				}
				if (player->playerId == 0) {
					coordinator->fsAssistOn = true;
				}
				if (coordinator->fsLastEcho != master) {
					mLOG(GBA_SIO, WARN, "FS assist: stalled round, echoing %04X to P%d", master, player->playerId);
				}
				coordinator->fsLastEcho = master;
			} else {
				memset(data, 0xFF, sizeof(uint16_t) * 4);
			}
			// A stalled completion means the secondary never acked, so
			// transferActive is stuck true: every later Start is rejected and
			// (with secondaries asleep) nobody is ever woken again -- the
			// games freeze. Real hardware always completes the transfer at the
			// master's clock, so clear the latch and wake the secondaries so
			// the next transfer can proceed (mirror of lockstep.c).
			if (player->playerId == 0 && coordinator->transferActive) {
				mLOG(GBA_SIO, WARN, "FS stall: clearing stuck transferActive");
				coordinator->transferActive = false;
				GBASIORendezvousCoordinatorWakePlayers(coordinator);
			}
		} else {
			if (coordinator->fsAssistArmed && (++coordinator->fsLogEvery % 50) == 0) {
				mLOG(GBA_SIO, WARN, "MULTI armed data: %04X %04X %04X %04X (send %04X %04X %04X %04X)",
				     coordinator->multiData[0], coordinator->multiData[1],
				     coordinator->multiData[2], coordinator->multiData[3],
				     (unsigned) lockstep->d.p->p->memory.io[GBA_REG(SIOMLT_SEND)],
				     (unsigned) lockstep->d.p->p->memory.io[GBA_REG(SIOMULTI0)],
				     (unsigned) lockstep->d.p->p->memory.io[GBA_REG(SIOMULTI1)],
				     (unsigned) lockstep->d.p->p->memory.io[GBA_REG(SIOMULTI2)]);
			}
			memcpy(data, coordinator->multiData, sizeof(uint16_t) * 4);

			// --- Four Swords handshake assist (mirror of lockstep.c) ---
			if (player->playerId == 0) {
				_fsAssistTick(coordinator, lockstep, data);
			}
			// Latch is set by the primary's tick, so both players' completions
			// for the same transfer make the same echo decision.
			// Echo only FEFE probe rounds (2026-09-06): echoing every round
			// overwrote the slave's real table entries with the master's value
			// (observed data=0000 0000 0000 0000 with slave's 00BE replaced),
			// so the master's game never saw the slave's post-name table. The
			// probe exchange is what resets both games' sendIdx so they re-send
			// their tables; real table rounds must pass through untouched.
			if (coordinator->fsAssistOn && data[0] == 0xFEFE) {
				// Echo the primary's sent probe in every slot (see lockstep.c).
				uint16_t master = data[0];
				int k;
				for (k = 0; k < 4; ++k) {
					data[k] = master;
				}
				mLOG(GBA_SIO, DEBUG, "FS assist: echo %04X to P%d", master, player->playerId);
			}
		}
		if (coordinator->fsAssistArmed && (++coordinator->fsLogEvery % 25) == 0) {
			uint16_t sio_siocnt = lockstep->d.p->siocnt;
			mLOG(GBA_SIO, WARN, "MULTI fin pid=%d siocnt=%04X irq=%d data=%04X %04X %04X %04X send=%04X",
			     player->playerId, (unsigned) sio_siocnt, !!(sio_siocnt & 0x4000),
			     data[0], data[1], data[2], data[3],
			     (unsigned) lockstep->d.p->p->memory.io[GBA_REG(SIOMLT_SEND)]);
		}
		mLOG(GBA_SIO, WARN, "BUSYCLR pid=%d local=%u",
		     player->playerId, (unsigned) mTimingCurrentTime(&lockstep->d.p->p->timing));
		player->dataReceived = false;
		if (player->playerId == 0) {
			// H1 experiment: the per-transfer hard sync is a full barrier after
			// every completed MULTI transfer. Real hardware finishes all players
			// together with no such barrier, and FS's rapid handshake appears to
			// desync (slave drifts a round ahead) when the master over-sleeps
			// here. The next transfer's WaitOnPlayers still re-syncs, so drop
			// this redundant barrier and measure the FS off-by-one rate.
			// _hardSync(coordinator, player);
		}
	}
	MutexUnlock(&coordinator->mutex);
}

static uint8_t GBASIORendezvousDriverFinishNormal8(struct GBASIODriver* driver) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	uint8_t data = 0xFF;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferMode == GBA_SIO_NORMAL_8) {
		struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (player->playerId > 0) {
			if (!player->dataReceived) {
				mLOG(GBA_SIO, WARN, "NORMAL did not receive data. Are we running behind?");
			} else {
				data = coordinator->normalData[player->playerId - 1];
				mLOG(GBA_SIO, DEBUG, "NORMAL8 transfer finished: %02X", data);
			}
		} else {
			// The master reads back the first secondary's data on the same
			// clock (real hardware shifts the slave's SIODATA into the master's
			// SIODATA register). Without this the master can never see anything
			// the slave sends, and games that exchange data in NORMAL mode
			// (e.g. Four Swords' post-name handshake) deadlock with the master
			// permanently stuck at recv=0 while the slave advances to recv=13.
			if (!player->dataReceived) {
				mLOG(GBA_SIO, WARN, "NORMAL did not receive data. Are we running behind?");
			} else {
				data = coordinator->normalData[1];
				mLOG(GBA_SIO, DEBUG, "NORMAL8 master received: %02X", data);
			}
		}
		player->dataReceived = false;
		if (player->playerId == 0) {
			_hardSync(coordinator, player);
		}
	}
	MutexUnlock(&coordinator->mutex);
	return data;
}

static uint32_t GBASIORendezvousDriverFinishNormal32(struct GBASIODriver* driver) {
	struct GBASIORendezvousDriver* lockstep = (struct GBASIORendezvousDriver*) driver;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	uint32_t data = 0xFFFFFFFF;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferMode == GBA_SIO_NORMAL_32) {
		struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (player->playerId > 0) {
			if (!player->dataReceived) {
				mLOG(GBA_SIO, WARN, "Did not receive data. Are we running behind?");
			} else {
				data = coordinator->normalData[player->playerId - 1];
				mLOG(GBA_SIO, DEBUG, "NORMAL32 transfer finished: %08X", data);
			}
		} else {
			// See FinishNormal8: the master must read back the slave's data.
			if (!player->dataReceived) {
				mLOG(GBA_SIO, WARN, "Did not receive data. Are we running behind?");
			} else {
				data = coordinator->normalData[1];
				mLOG(GBA_SIO, DEBUG, "NORMAL32 master received: %08X", data);
			}
		}
		player->dataReceived = false;
		if (player->playerId == 0) {
			_hardSync(coordinator, player);
		}
	}
	MutexUnlock(&coordinator->mutex);
	return data;
}

bool GBASIORendezvousDriverReadMultiRegs(struct GBASIORendezvousDriver* driver, uint16_t out[5]) {
	if (!driver || !driver->d.p || !driver->d.p->p) {
		return false;
	}
	uint16_t* io = driver->d.p->p->memory.io;
	out[0] = io[GBA_REG(SIOMULTI0) >> 1];
	out[1] = io[GBA_REG(SIOMULTI1) >> 1];
	out[2] = io[GBA_REG(SIOMULTI2) >> 1];
	out[3] = io[GBA_REG(SIOMULTI3) >> 1];
	out[4] = io[GBA_REG(SIOMLT_SEND) >> 1];
	return true;
}

void GBASIORendezvousCoordinatorSetFSArmed(struct GBASIORendezvousCoordinator* coordinator, bool armed) {
	MutexLock(&coordinator->mutex);
	coordinator->fsAssistArmed = armed;
	if (!armed) {
		coordinator->fsAssistOn = false;
		coordinator->fsHandshakeRounds = 0;
		coordinator->fsQuietRounds = 0;
	}
	MutexUnlock(&coordinator->mutex);
}

void GBASIORendezvousCoordinatorInit(struct GBASIORendezvousCoordinator* coordinator) {
	memset(coordinator, 0, sizeof(*coordinator));
	MutexInit(&coordinator->mutex);
	TableInit(&coordinator->players, 8, free);
}

void GBASIORendezvousCoordinatorDeinit(struct GBASIORendezvousCoordinator* coordinator) {
	MutexDeinit(&coordinator->mutex);
	TableDeinit(&coordinator->players);
}

void GBASIORendezvousCoordinatorAttach(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousDriver* driver) {
	if (driver->coordinator && driver->coordinator != coordinator) {
		// TODO
		abort();
	}
	driver->coordinator = coordinator;
}

void GBASIORendezvousCoordinatorDetach(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousDriver* driver) {
	if (driver->coordinator != coordinator) {
		// TODO
		abort();
		return;
	}
	MutexLock(&coordinator->mutex);
	struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, driver->lockstepId);
	if (player) {
		_removePlayer(coordinator, player);
	}
	MutexUnlock(&coordinator->mutex);
	driver->coordinator = NULL;
}

int32_t _untilNextSync(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousPlayer* player) {
	int32_t cycle = coordinator->cycle - GBASIORendezvousTime(player);
	if (player->playerId == 0) {
		if (coordinator->nAttached < 2) {
			cycle += UNLOCKED_INTERVAL;
		} else {
			cycle += LOCKSTEP_INTERVAL;
		}
	}
	return cycle;
}

void _advanceCycle(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousPlayer* player) {
	int32_t newCycle = GBASIORendezvousTime(player);
	mASSERT_DEBUG(newCycle - coordinator->cycle >= 0);
	coordinator->nextHardSync -= newCycle - coordinator->cycle;
	coordinator->cycle = newCycle;
}

void _removePlayer(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousPlayer* player) {
	struct GBASIORendezvousEvent event = {
		.type = SIO_EV_DETACH,
		.playerId = player->playerId,
		.timestamp = GBASIORendezvousTime(player),
	};
	_enqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));

	coordinator->waiting = 0;
	coordinator->transferActive = false;

	TableRemove(&coordinator->players, player->driver->lockstepId);
	_reconfigPlayers(coordinator);

	struct GBASIORendezvousPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
	if (runner) {
		GBASIORendezvousPlayerWake(runner);
	}
	_verifyAwake(coordinator);
}

void _reconfigPlayers(struct GBASIORendezvousCoordinator* coordinator) {
	size_t players = TableSize(&coordinator->players);
	memset(coordinator->attachedPlayers, 0, sizeof(coordinator->attachedPlayers));
	if (players == 0) {
		mLOG(GBA_SIO, WARN, "Reconfiguring player IDs with no players attached somehow?");
	} else if (players == 1) {
		struct TableIterator iter;
		mASSERT_LOG(GBA_SIO, TableIteratorStart(&coordinator->players, &iter), "Trying to reconfigure 1 player with empty player list");
		unsigned p0 = TableIteratorGetKey(&coordinator->players, &iter);
		coordinator->attachedPlayers[0] = p0;

		struct GBASIORendezvousPlayer* player = TableIteratorGetValue(&coordinator->players, &iter);
		coordinator->cycle = mTimingCurrentTime(&player->driver->d.p->p->timing);
		coordinator->nextHardSync = HARD_SYNC_INTERVAL;

		if (player->playerId != 0) {
			player->playerId = 0;
			if (player->driver->user->playerIdChanged) {
				player->driver->user->playerIdChanged(player->driver->user, player->playerId);
			}
		}

		if (!coordinator->transferActive) {
			coordinator->transferMode = player->mode;
		}
	} else {
		struct UIntList playerPreferences[MAX_GBAS];

		int i;
		for (i = 0; i < MAX_GBAS; ++i) {
			UIntListInit(&playerPreferences[i], 4);
		}

		// Collect the first four players' requested player IDs so we can sort through them later
		int seen = 0;
		struct TableIterator iter;
		mASSERT_LOG(GBA_SIO, TableIteratorStart(&coordinator->players, &iter), "Trying to reconfigure %" PRIz "u players with empty player list", players);
		do {
			unsigned pid = TableIteratorGetKey(&coordinator->players, &iter);
			struct GBASIORendezvousPlayer* player = TableIteratorGetValue(&coordinator->players, &iter);
			int requested = MAX_GBAS - 1;
			if (player->driver->user->requestedId) {
				requested = player->driver->user->requestedId(player->driver->user);
			}
			if (requested < 0) {
				continue;
			}
			if (requested >= MAX_GBAS) {
				requested = MAX_GBAS - 1;
			}

			*UIntListAppend(&playerPreferences[requested]) = pid;
			++seen;
		} while (TableIteratorNext(&coordinator->players, &iter) && seen < MAX_GBAS);

		// Now sort each requested player ID to figure out who gets which ID
		seen = 0;
		for (i = 0; i < MAX_GBAS; ++i) {
			int j;
			for (j = 0; j <= i; ++j) {
				while (UIntListSize(&playerPreferences[j]) && seen < MAX_GBAS) {
					unsigned pid = *UIntListGetPointer(&playerPreferences[j], 0);
					UIntListShift(&playerPreferences[j], 0, 1);
					struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, pid);
					if (!player) {
						mLOG(GBA_SIO, ERROR, "Player list appears to have changed unexpectedly. PID %u missing.", pid);
						continue;
					}
					coordinator->attachedPlayers[seen] = pid;
					if (player->playerId != seen) {
						player->playerId = seen;
						if (player->driver->user->playerIdChanged) {
							player->driver->user->playerIdChanged(player->driver->user, player->playerId);
						}
					}
					++seen;
				}
			}
		}

		for (i = 0; i < MAX_GBAS; ++i) {
			UIntListDeinit(&playerPreferences[i]);
		}
	}

	int nAttached = 0;
	size_t i;
	for (i = 0; i < MAX_GBAS; ++i) {
		unsigned pid = coordinator->attachedPlayers[i];
		if (!pid) {
			continue;
		}
		struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, pid);
		if (!player) {
			coordinator->attachedPlayers[i] = 0;
		} else {
			++nAttached;
		}
	}
	coordinator->nAttached = nAttached;
}

static void _setData(struct GBASIORendezvousCoordinator* coordinator, uint32_t id, struct GBASIO* sio) {
	switch (coordinator->transferMode) {
	case GBA_SIO_MULTI:
		coordinator->multiData[id] = sio->p->memory.io[GBA_REG(SIOMLT_SEND)];
		break;
	case GBA_SIO_NORMAL_8:
		coordinator->normalData[id] = sio->p->memory.io[GBA_REG(SIODATA8)];
		break;
	case GBA_SIO_NORMAL_32:
		coordinator->normalData[id] = sio->p->memory.io[GBA_REG(SIODATA32_LO)];
		coordinator->normalData[id] |= sio->p->memory.io[GBA_REG(SIODATA32_HI)] << 16;
		break;
	case GBA_SIO_UART:
	case GBA_SIO_GPIO:
	case GBA_SIO_JOYBUS:
		mLOG(GBA_SIO, WARN, "Unsupported mode %i in lockstep", coordinator->transferMode);
		// TODO: Should we handle this or just abort?
		break;
	}
}

void _setReady(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousPlayer* activePlayer, int playerId, enum GBASIOMode mode) {
	mASSERT_DEBUG(playerId >= 0 && playerId < MAX_GBAS);
	activePlayer->otherModes[playerId] = mode;
	bool ready = true;
	int i;
	for (i = 0; ready && i < coordinator->nAttached; ++i) {
		ready = activePlayer->otherModes[i] == activePlayer->mode;
	}
	if (activePlayer->mode == GBA_SIO_MULTI) {
		struct GBASIO* sio = activePlayer->driver->d.p;
		sio->siocnt = GBASIOMultiplayerSetReady(sio->siocnt, ready);
		sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, ready);
	}
}

void _hardSync(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousPlayer* player) {
	mASSERT_DEBUG(player->playerId == 0);
	struct GBASIORendezvousEvent event = {
		.type = SIO_EV_HARD_SYNC,
		.playerId = 0,
		.timestamp = GBASIORendezvousTime(player),
	};
	_enqueueEvent(coordinator, &event, TARGET_SECONDARY);
	GBASIORendezvousCoordinatorWaitOnPlayers(coordinator, player);
}

void _enqueueEvent(struct GBASIORendezvousCoordinator* coordinator, const struct GBASIORendezvousEvent* event, uint32_t target) {
	mLOG(GBA_SIO, DEBUG, "Enqueuing event of type %X from %i for target %X at timestamp %X",
	                      event->type, event->playerId, target, event->timestamp);

	int i;
	for (i = 0; i < coordinator->nAttached; ++i) {
		if (!(target & TARGET(i))) {
			continue;
		}
		struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		mASSERT_LOG(GBA_SIO, player->freeList, "No free events");
		struct GBASIORendezvousEvent* newEvent = player->freeList;
		player->freeList = newEvent->next;

		memcpy(newEvent, event, sizeof(*event));
		struct GBASIORendezvousEvent** previous = &player->queue;
		struct GBASIORendezvousEvent* next = player->queue;
		while (next) {
			int32_t until = newEvent->timestamp - next->timestamp;
			if (until < 0) {
				break;
			}
			previous = &next->next;
			next = next->next;
		}
		newEvent->next = next;
		*previous = newEvent;
	}
}

void _rendezvousEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBASIORendezvousDriver* lockstep = context;
	struct GBASIORendezvousCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	struct GBASIO* sio = player->driver->d.p;
	mASSERT_LOG(GBA_SIO, player->playerId >= 0 && player->playerId < 4, "Invalid multiplayer ID %i", player->playerId);

	bool wasDetach = false;
	if (player->queue && player->queue->type == SIO_EV_DETACH) {
		mLOG(GBA_SIO, DEBUG, "Player %i detached at timestamp %X, picking up the pieces",
		                      player->queue->playerId, player->queue->timestamp);
		wasDetach = true;
	}
	if (player->playerId == 0 && GBASIORendezvousTime(player) - coordinator->cycle >= 0) {
		// We are the clock owner; advance the shared clock. However, if we just became
		// the clock owner (by the previous one disconnecting) we might be slightly
		// behind the shared clock. We should wait a bit if needed in that case.
		_advanceCycle(coordinator, player);
		if (!coordinator->transferActive) {
			GBASIORendezvousCoordinatorWakePlayers(coordinator);
		}
		if (coordinator->nextHardSync < 0 && !coordinator->waiting) {
			_hardSync(coordinator, player);
		}
	}

	int32_t nextEvent = _untilNextSync(coordinator, player);
	while (true) {
		struct GBASIORendezvousEvent* event = player->queue;
		if (!event) {
			break;
		}
		if (event->timestamp > GBASIORendezvousTime(player)) {
			break;
		}
		player->queue = event->next;
		struct GBASIORendezvousEvent reply = {
			.playerId = player->playerId,
			.timestamp = GBASIORendezvousTime(player),
		};
		mLOG(GBA_SIO, DEBUG, "Got event of type %X from %i at timestamp %X",
		                      event->type, event->playerId, event->timestamp);
		switch (event->type) {
		case SIO_EV_ATTACH:
			_setReady(coordinator, player, event->playerId, -1);
			if (player->playerId == 0) {
				struct GBASIO* sio = player->driver->d.p;
				sio->siocnt = GBASIOMultiplayerClearSlave(sio->siocnt);
			}
			reply.mode = player->mode;
			reply.type = SIO_EV_MODE_SET;
			_enqueueEvent(coordinator, &reply, TARGET(event->playerId));
			break;
		case SIO_EV_HARD_SYNC:
			GBASIORendezvousCoordinatorAckPlayer(coordinator, player);
			GBASIORendezvousPlayerSleep(player);
			break;
		case SIO_EV_TRANSFER_START:
			_setData(coordinator, player->playerId, sio);
			nextEvent = event->finishCycle - GBASIORendezvousTime(player) - cyclesLate;
			mLOG(GBA_SIO, DEBUG, "Rendezvous: player %d time %08X finishCycle %08X drift %d",
			     player->playerId, GBASIORendezvousTime(player), event->finishCycle,
			     GBASIORendezvousTime(player) - event->finishCycle);
			player->driver->d.p->siocnt |= 0x80;
			mLOG(GBA_SIO, WARN, "BUSYSET pid=%d local=%u shared=%08X finish=%08X nextEvent=%d late=%u",
			     player->playerId, (unsigned) mTimingCurrentTime(&sio->p->timing),
			     GBASIORendezvousTime(player), event->finishCycle, nextEvent, cyclesLate);
			mTimingDeschedule(&sio->p->timing, &sio->completeEvent);
			mTimingSchedule(&sio->p->timing, &sio->completeEvent, nextEvent);
			GBASIORendezvousCoordinatorAckPlayer(coordinator, player);
			break;
		case SIO_EV_MODE_SET:
			if (coordinator->transferActive && player->mode != event->mode) {
				mLOG(GBA_SIO, DEBUG, "Switching modes while transfer is active");
				_abortTransfer(coordinator, player);
			}
			_setReady(coordinator, player, event->playerId, event->mode);
			if (event->playerId == 0) {
				GBASIORendezvousCoordinatorAckPlayer(coordinator, player);
				GBASIORendezvousPlayerSleep(player);
			}
			break;
		case SIO_EV_DETACH:
			_setReady(coordinator, player, event->playerId, -1);
			_setReady(coordinator, player, player->playerId, player->mode);
			reply.mode = player->mode;
			reply.type = SIO_EV_MODE_SET;
			_enqueueEvent(coordinator, &reply, ~TARGET(event->playerId));
			if (player->mode == GBA_SIO_MULTI) {
				sio->siocnt = GBASIOMultiplayerSetId(sio->siocnt, player->playerId);
				sio->siocnt = GBASIOMultiplayerSetSlave(sio->siocnt, player->playerId || coordinator->nAttached < 2);
			}
			wasDetach = true;
			break;
		}
		event->next = player->freeList;
		player->freeList = event;
	}
	if (player->queue && player->queue->timestamp - GBASIORendezvousTime(player) < nextEvent) {
		nextEvent = player->queue->timestamp - GBASIORendezvousTime(player);
	}

	if (player->playerId != 0 && nextEvent <= LOCKSTEP_INTERVAL) {
		if (!player->queue || wasDetach) {
			GBASIORendezvousPlayerSleep(player);
			// XXX: Is there a better way to gain sync lock at the beginning?
			if (nextEvent < 4) {
				nextEvent = 4;
			}
			_verifyAwake(coordinator);
		}
	}
	MutexUnlock(&coordinator->mutex);

	// A non-positive delay means this player is at or ahead of the shared clock
	// (or its 32-bit local time wrapped relative to the coordinator). Scheduling
	// the event at a non-positive delay fires it immediately on the next tick,
	// re-entering _rendezvousEvent forever and busy-spinning the emulation thread.
	// Clamp to a short positive delay so we re-check sync soon instead. The
	// player is throttled by GBASIORendezvousPlayerSleep (its host thread is
	// skipped by the frame loop) rather than by spinning here.
	if (nextEvent <= 0) {
		mLOG(GBA_SIO, DEBUG, "Lockstep: clamping non-positive sync delay %d (pid %d)",
		     nextEvent, player->playerId);
		nextEvent = 4;
	}
	mTimingSchedule(timing, &lockstep->event, nextEvent);
}

int32_t GBASIORendezvousTime(struct GBASIORendezvousPlayer* player) {
	return mTimingCurrentTime(&player->driver->d.p->p->timing) - player->cycleOffset;
}

void GBASIORendezvousCoordinatorWaitOnPlayers(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousPlayer* player) {
	mASSERT_LOG(GBA_SIO, !coordinator->waiting, "Multiplayer desynchronized: coordinator still waiting");
	mASSERT_LOG(GBA_SIO, !player->asleep, "Multiplayer desynchronized: player asleep");
	mASSERT_LOG(GBA_SIO, player->playerId == 0, "Multiplayer desynchronized: invalid player %i attempting to coordinate", player->playerId);
	if (coordinator->nAttached < 2) {
		return;
	}

	_advanceCycle(coordinator, player);
	mLOG(GBA_SIO, DEBUG, "Primary waiting for players to ack");
	coordinator->waiting = ((1 << coordinator->nAttached) - 1) & ~TARGET(player->playerId);
	GBASIORendezvousPlayerSleep(player);
	GBASIORendezvousCoordinatorWakePlayers(coordinator);

	_verifyAwake(coordinator);
}

void GBASIORendezvousCoordinatorWakePlayers(struct GBASIORendezvousCoordinator* coordinator) {
	int i;
	for (i = 1; i < coordinator->nAttached; ++i) {
		if (!coordinator->attachedPlayers[i]) {
			continue;
		}
		struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		GBASIORendezvousPlayerWake(player);
	}
}

void GBASIORendezvousPlayerWake(struct GBASIORendezvousPlayer* player) {
	if (!player->asleep) {
		return;
	}
	player->asleep = false;
	player->driver->user->wake(player->driver->user);
}

void GBASIORendezvousCoordinatorAckPlayer(struct GBASIORendezvousCoordinator* coordinator, struct GBASIORendezvousPlayer* player) {
	if (player->playerId == 0) {
		return;
	}
	coordinator->waiting &= ~TARGET(player->playerId);
	if (!coordinator->waiting) {
		mLOG(GBA_SIO, DEBUG, "All players acked, waking primary");
		if (coordinator->transferActive) {
			int i;
			for (i = 0; i < coordinator->nAttached; ++i) {
				if (!coordinator->attachedPlayers[i]) {
					continue;
				}
				struct GBASIORendezvousPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
				player->dataReceived = true;
			}

			coordinator->transferActive = false;
		}

		coordinator->nextHardSync = HARD_SYNC_INTERVAL;
		struct GBASIORendezvousPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
		GBASIORendezvousPlayerWake(runner);
	}
	// NOTE (rendezvous driver): do NOT sleep here. The stock lockstep driver
	// sleeps the secondary right after it acks a TRANSFER_START, which freezes
	// its core until the master's next 4096-cycle tick wakes it -- so the
	// secondary's game observes the transfer completion LATE, relative to the
	// master, and the two games drift apart in the Four Swords handshake.
	// Instead let the secondary run on to finishCycle so both sides complete
	// the transfer at the SAME cycle. Callers that DO need the sleep (hard
	// sync, mode set) call GBASIORendezvousPlayerSleep themselves.
}

void GBASIORendezvousPlayerSleep(struct GBASIORendezvousPlayer* player) {
	if (player->asleep) {
		return;
	}
	player->asleep = true;
	player->driver->user->sleep(player->driver->user);
	player->driver->d.p->p->cpu->nextEvent = 0;
	GBAInterrupt(player->driver->d.p->p);

	// DualBoy runs every player sequentially on one thread, so a sleeping player's
	// host thread never actually blocks (the user->sleep callback returns
	// immediately). The frame loop honours the sleep flag: it skips a sleeping
	// player and instead steps the other player until it wakes this one back up.
	// `cpu->nextEvent = 0` + `GBAInterrupt` above make the current `runLoop` step
	// return promptly so the frame loop can switch players mid-frame. We do NOT
	// bump the video frame counter here: doing so split the player's ROM frame
	// across two ticks (the next frame boundary then landed one scanline before
	// the ROM's own vblank wait) and ran that player at half speed.
}

size_t GBASIORendezvousCoordinatorAttached(struct GBASIORendezvousCoordinator* coordinator) {
	size_t count;
	MutexLock(&coordinator->mutex);
	count = TableSize(&coordinator->players);
	MutexUnlock(&coordinator->mutex);
	return count;
}
