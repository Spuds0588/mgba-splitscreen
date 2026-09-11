/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

#include <stdio.h>
#include <stdlib.h>

#define DRIVER_ID 0x6B636F4C
#define DRIVER_STATE_VERSION 1
#define LOCKSTEP_INTERVAL 4096
#define UNLOCKED_INTERVAL 8192
#define HARD_SYNC_INTERVAL 0x80000
#define TARGET(P) (1 << (P))
#define TARGET_ALL 0xF
#define TARGET_PRIMARY 0x1
#define TARGET_SECONDARY ((TARGET_ALL) & ~(TARGET_PRIMARY))

DECL_BITFIELD(GBASIOLockstepSerializedFlags, uint32_t);
DECL_BITS(GBASIOLockstepSerializedFlags, DriverMode, 0, 3);
DECL_BITS(GBASIOLockstepSerializedFlags, NumEvents, 3, 4);
DECL_BIT(GBASIOLockstepSerializedFlags, Asleep, 7);
DECL_BIT(GBASIOLockstepSerializedFlags, DataReceived, 8);
DECL_BIT(GBASIOLockstepSerializedFlags, EventScheduled, 9);
DECL_BITS(GBASIOLockstepSerializedFlags, Player0Mode, 10, 3);
DECL_BITS(GBASIOLockstepSerializedFlags, Player1Mode, 13, 3);
DECL_BITS(GBASIOLockstepSerializedFlags, Player2Mode, 16, 3);
DECL_BITS(GBASIOLockstepSerializedFlags, Player3Mode, 19, 3);
DECL_BITS(GBASIOLockstepSerializedFlags, TransferMode, 28, 3);
DECL_BIT(GBASIOLockstepSerializedFlags, TransferActive, 31);

DECL_BITFIELD(GBASIOLockstepSerializedEventFlags, uint32_t);
DECL_BITS(GBASIOLockstepSerializedEventFlags, Type, 0, 3);

struct GBASIOLockstepSerializedEvent {
	int32_t timestamp;
	int32_t playerId;
	GBASIOLockstepSerializedEventFlags flags;
	int32_t reserved[5];
	union {
		int32_t mode;
		int32_t finishCycle;
		int32_t padding[4];
	};
};
static_assert(sizeof(struct GBASIOLockstepSerializedEvent) == 0x30, "GBA lockstep event savestate struct sized wrong");

struct GBASIOLockstepSerializedState {
	uint32_t version;
	GBASIOLockstepSerializedFlags flags;
	uint32_t reserved[2];

	struct {
		int32_t nextEvent;
		uint32_t reservedDriver[7];
	} driver;

	struct {
		int32_t playerId;
		int32_t cycleOffset;
		uint32_t reservedPlayer[2];
		struct GBASIOLockstepSerializedEvent events[MAX_LOCKSTEP_EVENTS];
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
static_assert(offsetof(struct GBASIOLockstepSerializedState, driver) == 0x10, "GBA lockstep savestate driver offset wrong");
static_assert(offsetof(struct GBASIOLockstepSerializedState, player) == 0x30, "GBA lockstep savestate player offset wrong");
static_assert(offsetof(struct GBASIOLockstepSerializedState, coordinator) == 0x1C0, "GBA lockstep savestate coordinator offset wrong");
static_assert(sizeof(struct GBASIOLockstepSerializedState) == 0x1F0, "GBA lockstep savestate struct sized wrong");

static bool GBASIOLockstepDriverInit(struct GBASIODriver* driver);
static void GBASIOLockstepDriverDeinit(struct GBASIODriver* driver);
static void GBASIOLockstepDriverReset(struct GBASIODriver* driver);
static uint32_t GBASIOLockstepDriverId(const struct GBASIODriver* driver);
static bool GBASIOLockstepDriverLoadState(struct GBASIODriver* driver, const void* state, size_t size);
static void GBASIOLockstepDriverSaveState(struct GBASIODriver* driver, void** state, size_t* size);
static void GBASIOLockstepDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static bool GBASIOLockstepDriverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static int GBASIOLockstepDriverConnectedDevices(struct GBASIODriver* driver);
static int GBASIOLockstepDriverDeviceId(struct GBASIODriver* driver);
static uint16_t GBASIOLockstepDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value);
static uint16_t GBASIOLockstepDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value);
static bool GBASIOLockstepDriverStart(struct GBASIODriver* driver);
static void GBASIOLockstepDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]);
static uint8_t GBASIOLockstepDriverFinishNormal8(struct GBASIODriver* driver);
static uint32_t GBASIOLockstepDriverFinishNormal32(struct GBASIODriver* driver);

static void GBASIOLockstepCoordinatorWaitOnPlayers(struct GBASIOLockstepCoordinator*, struct GBASIOLockstepPlayer*);
static void GBASIOLockstepCoordinatorAckPlayer(struct GBASIOLockstepCoordinator*, struct GBASIOLockstepPlayer*);
static void GBASIOLockstepCoordinatorWakePlayers(struct GBASIOLockstepCoordinator*);

static int32_t GBASIOLockstepTime(struct GBASIOLockstepPlayer*);
static void GBASIOLockstepPlayerWake(struct GBASIOLockstepPlayer*);
static void GBASIOLockstepPlayerSleep(struct GBASIOLockstepPlayer*);

static void _advanceCycle(struct GBASIOLockstepCoordinator*, struct GBASIOLockstepPlayer*);
static void _removePlayer(struct GBASIOLockstepCoordinator*, struct GBASIOLockstepPlayer*);
static void _reconfigPlayers(struct GBASIOLockstepCoordinator*);
static int32_t _untilNextSync(struct GBASIOLockstepCoordinator*, struct GBASIOLockstepPlayer*);
static void _enqueueEvent(struct GBASIOLockstepCoordinator*, const struct GBASIOLockstepEvent*, uint32_t target);
static void _setData(struct GBASIOLockstepCoordinator*, uint32_t id, struct GBASIO* sio);
static void _setReady(struct GBASIOLockstepCoordinator*, struct GBASIOLockstepPlayer* activePlayer, int playerId, enum GBASIOMode mode);
static void _hardSync(struct GBASIOLockstepCoordinator*, struct GBASIOLockstepPlayer*);

static void _lockstepEvent(struct mTiming*, void* context, uint32_t cyclesLate);

static void _verifyAwake(struct GBASIOLockstepCoordinator* coordinator) {
#ifdef NDEBUG
	UNUSED(coordinator);
#else
	int i;
	int asleep = 0;
	for (i = 0; i < coordinator->nAttached; ++i) {
		if (!coordinator->attachedPlayers[i]) {
			continue;
		}
		struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		asleep += player->asleep;
	}
	mASSERT_DEBUG(!asleep || asleep < coordinator->nAttached);
#endif
}

static void _abortTransfer(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepPlayer* player) {
	mLOG(GBA_SIO, DEBUG, "Aborting in-progress transfer");
	// TODO: Do we need to clean this up better?
	coordinator->transferActive = false;
	coordinator->waiting = 0;

	if (player->playerId != 0) {
		struct GBASIOLockstepPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
		if (runner) {
			GBASIOLockstepPlayerWake(runner);
		}
	} else {
		GBASIOLockstepCoordinatorWakePlayers(coordinator);
	}
}

void GBASIOLockstepDriverCreate(struct GBASIOLockstepDriver* driver, struct mLockstepUser* user) {
	memset(driver, 0, sizeof(*driver));
	driver->d.init = GBASIOLockstepDriverInit;
	driver->d.deinit = GBASIOLockstepDriverDeinit;
	driver->d.reset = GBASIOLockstepDriverReset;
	driver->d.driverId = GBASIOLockstepDriverId;
	driver->d.loadState = GBASIOLockstepDriverLoadState;
	driver->d.saveState = GBASIOLockstepDriverSaveState;
	driver->d.setMode = GBASIOLockstepDriverSetMode;
	driver->d.handlesMode = GBASIOLockstepDriverHandlesMode;
	driver->d.deviceId = GBASIOLockstepDriverDeviceId;
	driver->d.connectedDevices = GBASIOLockstepDriverConnectedDevices;
	driver->d.writeSIOCNT = GBASIOLockstepDriverWriteSIOCNT;
	driver->d.writeRCNT = GBASIOLockstepDriverWriteRCNT;
	driver->d.start = GBASIOLockstepDriverStart;
	driver->d.finishMultiplayer = GBASIOLockstepDriverFinishMultiplayer;
	driver->d.finishNormal8 = GBASIOLockstepDriverFinishNormal8;
	driver->d.finishNormal32 = GBASIOLockstepDriverFinishNormal32;
	driver->event.context = driver;
	driver->event.callback = _lockstepEvent;
	driver->event.name = "GBA SIO Lockstep";
	driver->event.priority = 0x80;
	driver->user = user;
}

static bool GBASIOLockstepDriverInit(struct GBASIODriver* driver) {
	GBASIOLockstepDriverReset(driver);
	return true;
}

static void GBASIOLockstepDriverDeinit(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player) {
		_removePlayer(coordinator, player);
	}
	MutexUnlock(&coordinator->mutex);
	mTimingDeschedule(&lockstep->d.p->p->timing, &lockstep->event);
	lockstep->lockstepId = 0;
}

static void GBASIOLockstepDriverReset(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct GBASIOLockstepPlayer* player;
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
			struct GBASIOLockstepEvent event = {
				.type = SIO_EV_ATTACH,
				.playerId = player->playerId,
				.timestamp = GBASIOLockstepTime(player),
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
		GBASIOLockstepCoordinatorWakePlayers(coordinator);
	}

	// State-load reset (2026-09-09): after a core state is restored the
	// coordinator's per-player event queue comes back with it (HARD_SYNCs,
	// transfer acks in flight) and the asleep flags were restored too. If those
	// stale events are left in the queue, the first HARD_SYNC each player
	// processes immediately puts the secondaries back to sleep -- and since the
	// game itself is parked waiting for input (FS link screen), nothing ever
	// wakes them: every core freezes, zero frames complete. Drop the queue and
	// clear the sleep flags so the restored system is quiescent and the games
	// re-handshake on their own. (Observed via the --fs11 harness diag: P2/P3
	// asleep=1 with no frame progress after a browser-exported DUALSTATE load.)
	player->queue = NULL;
	player->asleep = false;
	player->dataReceived = false;
	int q;
	for (q = 0; q < MAX_LOCKSTEP_EVENTS - 1; ++q) {
		player->buffer[q].next = &player->buffer[q + 1];
	}
	player->freeList = &player->buffer[0];
	if (player->driver->user->wake) {
		player->driver->user->wake(player->driver->user);
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

static uint32_t GBASIOLockstepDriverId(const struct GBASIODriver* driver) {
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

static bool GBASIOLockstepDriverLoadState(struct GBASIODriver* driver, const void* data, size_t size) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	if (size != sizeof(struct GBASIOLockstepSerializedState)) {
		mLOG(GBA_SIO, WARN, "Incorrect state size: expected %" PRIz "X, got %" PRIz "X", sizeof(struct GBASIOLockstepSerializedState), size);
		return false;
	}
	const struct GBASIOLockstepSerializedState* state = data;
	bool error = false;
	uint32_t ucheck;
	int32_t check;
	LOAD_32LE(ucheck, 0, &state->version);
	if (ucheck > DRIVER_STATE_VERSION) {
		mLOG(GBA_SIO, WARN, "Invalid or too new save state: expected %u, got %u", DRIVER_STATE_VERSION, ucheck);
		return false;
	}

	MutexLock(&coordinator->mutex);
	struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	LOAD_32LE(check, 0, &state->player.playerId);
	if (check != player->playerId) {
		mLOG(GBA_SIO, WARN, "State is for different player: expected %d, got %d", player->playerId, check);
		error = true;
		goto out;
	}

	GBASIOLockstepSerializedFlags flags = 0;
	LOAD_32LE(flags, 0, &state->flags);
	LOAD_32LE(player->cycleOffset, 0, &state->player.cycleOffset);
	player->dataReceived = GBASIOLockstepSerializedFlagsGetDataReceived(flags);
	player->mode = _modeIntToEnum(GBASIOLockstepSerializedFlagsGetDriverMode(flags));

	player->otherModes[0] = _modeIntToEnum(GBASIOLockstepSerializedFlagsGetPlayer0Mode(flags));
	player->otherModes[1] = _modeIntToEnum(GBASIOLockstepSerializedFlagsGetPlayer1Mode(flags));
	player->otherModes[2] = _modeIntToEnum(GBASIOLockstepSerializedFlagsGetPlayer2Mode(flags));
	player->otherModes[3] = _modeIntToEnum(GBASIOLockstepSerializedFlagsGetPlayer3Mode(flags));

	if (GBASIOLockstepSerializedFlagsGetEventScheduled(flags)) {
		int32_t when;
		LOAD_32LE(when, 0, &state->driver.nextEvent);
		mTimingSchedule(&driver->p->p->timing, &lockstep->event, when);
	}

	if (GBASIOLockstepSerializedFlagsGetAsleep(flags)) {
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

	struct GBASIOLockstepEvent** lastEvent = &player->queue;
	for (i = 0; i < GBASIOLockstepSerializedFlagsGetNumEvents(flags) && i < MAX_LOCKSTEP_EVENTS; ++i) {
		struct GBASIOLockstepEvent* event = player->freeList;
		const struct GBASIOLockstepSerializedEvent* stateEvent = &state->player.events[i];
		player->freeList = player->freeList->next;
		*lastEvent = event;
		lastEvent = &event->next;

		GBASIOLockstepSerializedEventFlags flags;
		LOAD_32LE(flags, 0, &stateEvent->flags);
		LOAD_32LE(event->timestamp, 0, &stateEvent->timestamp);
		LOAD_32LE(event->playerId, 0, &stateEvent->playerId);
		event->type = GBASIOLockstepSerializedEventFlagsGetType(flags);
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
		coordinator->transferMode = _modeIntToEnum(GBASIOLockstepSerializedFlagsGetTransferMode(flags));
		coordinator->transferActive = GBASIOLockstepSerializedFlagsGetTransferActive(flags);
	}
out:
	MutexUnlock(&coordinator->mutex);
	if (!error) {
		mTimingInterrupt(&driver->p->p->timing);
	}
	return !error;
}

static void GBASIOLockstepDriverSaveState(struct GBASIODriver* driver, void** stateOut, size_t* size) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct GBASIOLockstepSerializedState* state = calloc(1, sizeof(*state));

	STORE_32LE(DRIVER_STATE_VERSION, 0, &state->version);

	STORE_32LE(lockstep->event.when - mTimingCurrentTime(&driver->p->p->timing), 0, &state->driver.nextEvent);

	MutexLock(&coordinator->mutex);
	struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	GBASIOLockstepSerializedFlags flags = 0;
	STORE_32LE(player->playerId, 0, &state->player.playerId);
	STORE_32LE(player->cycleOffset, 0, &state->player.cycleOffset);
	flags = GBASIOLockstepSerializedFlagsSetAsleep(flags, player->asleep);
	flags = GBASIOLockstepSerializedFlagsSetDataReceived(flags, player->dataReceived);
	flags = GBASIOLockstepSerializedFlagsSetDriverMode(flags, _modeEnumToInt(player->mode));
	flags = GBASIOLockstepSerializedFlagsSetEventScheduled(flags, mTimingIsScheduled(&driver->p->p->timing, &lockstep->event));

	flags = GBASIOLockstepSerializedFlagsSetPlayer0Mode(flags, _modeEnumToInt(player->otherModes[0]));
	flags = GBASIOLockstepSerializedFlagsSetPlayer1Mode(flags, _modeEnumToInt(player->otherModes[1]));
	flags = GBASIOLockstepSerializedFlagsSetPlayer2Mode(flags, _modeEnumToInt(player->otherModes[2]));
	flags = GBASIOLockstepSerializedFlagsSetPlayer3Mode(flags, _modeEnumToInt(player->otherModes[3]));

	struct GBASIOLockstepEvent* event = player->queue;
	size_t i;
	for (i = 0; i < MAX_LOCKSTEP_EVENTS && event; ++i, event = event->next) {
		struct GBASIOLockstepSerializedEvent* stateEvent = &state->player.events[i];
		GBASIOLockstepSerializedEventFlags flags = GBASIOLockstepSerializedEventFlagsSetType(0, event->type);
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
	flags = GBASIOLockstepSerializedFlagsSetNumEvents(flags, i);

	if (player->playerId == 0) {
		STORE_32LE(coordinator->cycle, 0, &state->coordinator.cycle);
		STORE_32LE(coordinator->waiting, 0, &state->coordinator.waiting);
		STORE_32LE(coordinator->nextHardSync, 0, &state->coordinator.nextHardSync);
		for (i = 0; i < 4; ++i) {
			STORE_16LE(coordinator->multiData[i], 0, &state->coordinator.multiData[i]);
			STORE_32LE(coordinator->normalData[i], 0, &state->coordinator.normalData[i]);
		}
		flags = GBASIOLockstepSerializedFlagsSetTransferMode(flags, _modeEnumToInt(coordinator->transferMode));
		flags = GBASIOLockstepSerializedFlagsSetTransferActive(flags, coordinator->transferActive);
	}
	MutexUnlock(&lockstep->coordinator->mutex);

	STORE_32LE(flags, 0, &state->flags);
	*stateOut = state;
	*size = sizeof(*state);
}

static void GBASIOLockstepDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (mode != player->mode) {
		mLOG(GBA_SIO, DEBUG, "Switching mode from %d to %d", player->mode, mode);
		player->mode = mode;
		struct GBASIOLockstepEvent event = {
			.type = SIO_EV_MODE_SET,
			.playerId = player->playerId,
			.timestamp = GBASIOLockstepTime(player),
			.mode = mode,
		};
		if (player->playerId == 0) {
			coordinator->transferMode = mode;
			GBASIOLockstepCoordinatorWaitOnPlayers(coordinator, player);
		}
		_setReady(coordinator, player, player->playerId, mode);
		_enqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));
	}
	MutexUnlock(&coordinator->mutex);
}

static bool GBASIOLockstepDriverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	UNUSED(mode);
	return true;
}

static int GBASIOLockstepDriverConnectedDevices(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	if (!lockstep->lockstepId) {
		return 0;
	}
	MutexLock(&coordinator->mutex);
	int attached = coordinator->nAttached - 1;
	MutexUnlock(&coordinator->mutex);
	return attached;
}

static int GBASIOLockstepDriverDeviceId(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	int playerId = 0;
	MutexLock(&coordinator->mutex);
	struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player && player->playerId >= 0) {
		playerId = player->playerId;
	}
	MutexUnlock(&coordinator->mutex);
	return playerId;
}

static uint16_t GBASIOLockstepDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	mLOG(GBA_SIO, DEBUG, "Lockstep: SIOCNT <- %04X", value);
	return value;
}

static uint16_t GBASIOLockstepDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	mLOG(GBA_SIO, DEBUG, "Lockstep: RCNT <- %04X", value);
	return value;
}

static bool GBASIOLockstepDriverStart(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	bool ret = false;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferActive) {
		mLOG(GBA_SIO, GAME_ERROR, "Transfer restarted unexpectedly");
		goto out;
	}
	if (coordinator->nAttached < 2) {
		mLOG(GBA_SIO, DEBUG, "Attempted to start transfer with no secondary players");
		goto out;
	}
	struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player->playerId != 0) {
		mLOG(GBA_SIO, DEBUG, "Secondary player attempted to start transfer");
		goto out;
	}
	mLOG(GBA_SIO, DEBUG, "Transfer starting at %08X", coordinator->cycle);
	memset(coordinator->multiData, 0xFF, sizeof(coordinator->multiData));
	_setData(coordinator, 0, player->driver->d.p);

	int32_t timestamp = GBASIOLockstepTime(player);
	struct GBASIOLockstepEvent event = {
		.type = SIO_EV_TRANSFER_START,
		.timestamp = timestamp,
		.finishCycle = timestamp + GBASIOTransferCycles(player->mode, player->driver->d.p->siocnt, coordinator->nAttached - 1),
	};
	_enqueueEvent(coordinator, &event, TARGET_SECONDARY);
	GBASIOLockstepCoordinatorWaitOnPlayers(coordinator, player);
	coordinator->transferActive = true;
	ret = true;
out:
	MutexUnlock(&coordinator->mutex);
	return ret;
}

/* ---- Four Swords link-handshake assist ----
 *
 * The FS linking screen never completes its discovery handshake under any
 * plumbing (verified: per-transfer barriers on/off, cycle-lockstep, threaded
 * execution — see PROJECT_LOG). The two games exchange FEFE probes and
 * (value, 0xFFF1-value) checksum pairs whose counters stay offset (~0x13,
 * the slave runs a round ahead), so neither game ever sees a consistent
 * partner and the discovery loops forever.
 *
 * This assist detects the discovery signature on the FS cart and, while it
 * is active, delivers each recipient ITS OWN sent value in every slot
 * (echo). Each game therefore sees all devices agreeing with it — which is
 * exactly what synchronized hardware looks like — and its discovery state
 * machine can advance. When real payload data flows (nonzero values that
 * neither probe nor sum to 0xFFF1, four rounds running), the assist hands
 * off to raw pass-through.
 */
static void _fsAssistIdentify(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepDriver* lockstep) {
	// Four Swords carts carry internal title "GBAZELDA" at ROM offset 0xA0.
	//
	// Probe exactly ONCE. The title cannot change while a ROM is loaded, but this
	// used to be called from every transfer completion and every kick attempt, so
	// on any non-FS game it logged tens of thousands of "identify failed" lines a
	// second (25k+ lines in a 20-second run) and burned work on every round of
	// every session. Cache the answer instead.
	if (coordinator->fsAssistChecked) {
		return;
	}
	if (coordinator->fsSuppressed) {
		// The host switched the whole assist off (see
		// GBASIOLockstepCoordinatorSetFSSuppressed). Latch so we never probe.
		coordinator->fsAssistChecked = true;
		coordinator->fsAssistEnabled = false;
		return;
	}
	struct GBA* gba = lockstep && lockstep->d.p ? lockstep->d.p->p : NULL;
	if (!gba) {
		// Core not wired up yet; retry on a later call rather than latching a
		// negative answer we cannot trust.
		return;
	}
	coordinator->fsAssistChecked = true;
	coordinator->fsAssistEnabled = false;
	// Kill switch for A/B runs: DUALBOY_FS_ASSIST=0 loads the game but leaves the
	// assist and the deadlock kick fully off, so FS's native behaviour can be
	// compared against the assisted path without a rebuild.
	const char* env = getenv("DUALBOY_FS_ASSIST");
	if (env && env[0] == '0') {
		mLOG(GBA_SIO, WARN, "FS assist: disabled by DUALBOY_FS_ASSIST=0");
		return;
	}
	if (gba->memory.rom && gba->memory.romSize > 0xA4) {
		const char* title = (const char*) &gba->memory.rom[0xA0 >> 2];
		coordinator->fsAssistEnabled =
		    title[0] == 'G' && title[1] == 'B' && title[2] == 'A' && title[3] == 'Z';
	}
	if (coordinator->fsAssistEnabled) {
		mLOG(GBA_SIO, WARN, "FS assist: Four Swords cart identified");
	} else {
		mLOG(GBA_SIO, WARN, "FS assist: not the Four Swords cart; assist off for this session");
	}
}

static bool _fsAssistHandshakeRound(const uint16_t data[4], int n) {
	// A discovery round: any player probing with FEFE, or a nonzero pair of
	// values summing to 0xFFF1 (FS's validity pair). All-zero idle rounds are
	// NOT discovery rounds — they recur inside the cycle and must not count
	// toward the hand-off.
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
	for (i = 0; i < n; ++i) {
		if (data[i] == 0 || data[i] == 0xFFFF) {
			continue;
		}
		for (j = i + 1; j < n; ++j) {
			if (data[j] != 0 && data[j] != 0xFFFF &&
			    (uint32_t) data[i] + (uint32_t) data[j] == 0xFFF1) {
				return true;
			}
		}
	}
	return false;
}

static void _fsAssistTick(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepDriver* lockstep, const uint16_t data[4]) {
	// Called on the primary's transfer completion (once per round).
	// The echo is LATCHED, not per-round. A discovery-signature round (FEFE
	// probe or a 0xFFF1-validity pair) proves the games are exchanging the
	// link discovery cycle; after 3 consecutive such rounds the games are at
	// the frozen link screen and the echo engages, delivering the primary's
	// value to EVERY slot on EVERY round (so both games see agreement and
	// their 13-cycle receive counters advance together instead of stalling
	// one short when a round arrives raw). The echo hands off when real
	// payload data flows -- 120 consecutive non-discovery rounds means the
	// games left the link screen -- and can re-engage on the post-name
	// discovery cycle. Per-round decisions were racy (the secondary's
	// completion could fire before the primary latched the flag, or after the
	// next round cleared it), which is why the games stalled at 12/13.
	if (!coordinator->fsAssistEnabled && !coordinator->fsAssistOn) {
		_fsAssistIdentify(coordinator, lockstep);
	}
	if (!coordinator->fsAssistEnabled) {
		return;
	}
	// The host must arm the assist (games visibly frozen at the link screen):
	// the identical discovery cycle also runs during normal navigation, where
	// faking success breaks the flow. See SetFSArmed.
	if (!coordinator->fsAssistArmed) {
		coordinator->fsAssistOn = false;
		coordinator->fsHandshakeRounds = 0;
		coordinator->fsQuietRounds = 0;
		return;
	}
	// Round-content diagnostic (2026-09-10): when the games sit at the link
	// screen cycling rounds without the echo engaging, log what the rounds
	// actually contain (probes? idle tables? checksum pairs?) plus the
	// handshake state. Rate-limited to every 64th master completion.
	{
		static unsigned fsTickDiag = 0;
		if (++fsTickDiag % 64 == 0) {
			mLOG(GBA_SIO, WARN, "FS tick diag: rnd=%04X,%04X,%04X,%04X hs=%u on=%d quiet=%u nAtt=%u",
				data[0], data[1], data[2], data[3],
				(unsigned) coordinator->fsHandshakeRounds, coordinator->fsAssistOn ? 1 : 0,
				(unsigned) coordinator->fsQuietRounds, (unsigned) coordinator->nAttached);
		}
	}
	// Echo gating (2026-09-10): the echo was engaged IMMEDIATELY on arming,
	// which made every game receive the MASTER's value in every slot during
	// the discovery rounds -- so P2-P4 never saw their own checksums echoed
	// back and the per-slot checksum validation failed; the discovery looped
	// forever. The historical no-echo runs (fs8/fs8b, 0 injections) completed
	// the link on RAW data. Keep the echo OFF here; it only re-engages via the
	// quiet-round latch below (120 non-discovery rounds) if real data stops
	// flowing.
	if (coordinator->fsAssistOn) {
		// no-op: echo already engaged
	}
	if (_fsAssistHandshakeRound(data, coordinator->nAttached)) {
		coordinator->fsHandshakeRounds++;
		coordinator->fsQuietRounds = 0;
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

/* Declined-kick diagnostics (2026-09-09): the kick's self-gates silently
 * declined in the browser (console showed "FS kick: invoking" forever with no
 * reason), so debugging was blind. Log which gate failed plus each player's
 * gate values, rate-limited to once per distinct reason plus every 32nd
 * decline so a stuck session shows its state without flooding the console. */
static void _fsKickLogDecline(struct GBASIOLockstepCoordinator* coordinator, const char* why, struct GBASIOLockstepPlayer* const* players, int nPlayers) {
	static const char* lastWhy = NULL;
	static unsigned declines = 0;
	++declines;
	if (lastWhy == why && declines % 32 != 0) {
		return;
	}
	lastWhy = why;
	char buf[256] = "";
	int len = 0;
	int i;
	for (i = 0; i < nPlayers && len < 220; ++i) {
		struct GBA* gba = players[i]->driver->d.p->p;
		uint8_t* iw = (uint8_t*) gba->memory.iwram;
		uint8_t* st = (uint8_t*) gba->memory.wram + (0x02030790 & (GBA_SIZE_EWRAM - 1));
		uint32_t recv = 0;
		memcpy(&recv, st + 24, 4);
		len += snprintf(buf + len, sizeof(buf) - len,
			"P%d(mode=%u lstat=%u ssub=%u role=%u ph=%u recv=%u got12=%u) ",
			i + 1, iw ? iw[0x6D10] : 0, iw ? iw[0x0FC1] : 0, iw ? iw[0x0BFC] : 0,
			st[0], st[1], (unsigned) recv, st[5]);
	}
	mLOG(GBA_SIO, WARN, "FS kick declined (%s): %s", why, buf);
}

/* FS deadlock kick (2026-09-07, ported from the harness kick in
 * threaded_link.c which proved the full chain: link screen -> char select ->
 * PLAYABLE gameplay at mode 2:9).
 *
 * Why it exists: after the name entry, both games sit in the ROM phase
 * machine (0x800C54C) with phase>=1; the acceptance scan (0x800C6A8) needs a
 * received 12-halfword block summing to -15 in the +44 recv histories, and
 * got12 (state[5]) set. The games' own tables ARE valid (-15) but under the
 * emulator's timing the receive side never stores anything (histories all
 * zero), so neither acceptance scan can ever validate and the games deadlock
 * on the link screen forever. This kick writes a crafted -15 block into BOTH
 * recv buffers (state+40 AND state+44: the gate at EWRAM 0x02030950 swaps
 * them on every scan call), sets got12 + recvIdx=13 (master), and resets
 * sendIdx so the games' own handlers re-send their tables -- letting their
 * own state machines advance through char-select into gameplay.
 *
 * The crafted block is [0x00B0, 0xFF21, 0x0021, 0xFFFF, 0x8 x0] which sums
 * to 0xFFF1 == -15: the 0x0021 marker is the games' "link active" table
 * column (idle tables carry 0x0020), and the link-status machine's state-5
 * payload check (0x8037b5c) requires it so the [0x03000FC3]|=0x40 success
 * bit can set and the char-select gate ([0x03000FC8]==2) can open.
 *
 * Self-gating: only fires when the FS cart is identified AND both games are
 * in the phase machine (phase>=1) AND not already complete. Idempotent
 * (re-injects the current table + got12 each time the games consume it).
 */
static void _fsAssistKick(struct GBASIOLockstepCoordinator* coordinator) {
	// Self-gate on the FS cart: the kick pokes the FS ROM's private EWRAM
	// state machine, so it must NEVER run on another game. Identify lazily
	// (the cart ID is checked on the first transfer completion; if transfers
	// have stalled entirely before any completion, identify from here).
	if (!coordinator->fsAssistEnabled) {
		struct GBASIOLockstepPlayer* p0 = NULL;
		int i0;
		for (i0 = 0; i0 < coordinator->nAttached; ++i0) {
			p0 = TableLookup(&coordinator->players, coordinator->attachedPlayers[i0]);
			if (p0) {
				break;
			}
		}
		if (p0 && p0->driver) {
			_fsAssistIdentify(coordinator, p0->driver);
		}
	}
	if (!coordinator->fsAssistEnabled) {
		return;
	}
	mLOG(GBA_SIO, WARN, "FS kick: invoking (nAtt=%u)", (unsigned) coordinator->nAttached);
	struct GBASIOLockstepPlayer* players[MAX_GBAS] = {0};
	int nPlayers = 0;
	int i;
	for (i = 0; i < coordinator->nAttached; ++i) {
		struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		if (player && player->driver && player->driver->d.p && player->driver->d.p->p) {
			players[nPlayers++] = player;
		}
	}
	if (nPlayers < 2) {
		return;
	}
	// EWRAM base is gba->memory.wram (word pointer); state lives at 0x02030790.
	uint8_t* st[MAX_GBAS] = {0};
	bool inLink[MAX_GBAS] = {false};
	for (i = 0; i < nPlayers; ++i) {
		struct GBA* gba = players[i]->driver->d.p->p;
		st[i] = (uint8_t*) gba->memory.wram + (0x02030790 & (GBA_SIZE_EWRAM - 1));
		// The game's main-loop mode byte (IWRAM 0x03006D10): the link screen
		// and the post-name phase machine live in mode 9. During boot/menus
		// the mode is something else (P2's broken runs sat at 8/13), and the
		// EWRAM at 0x02030790 holds ordinary game data there -- so a
		// memory-signature check alone is NOT enough (the game initializes
		// the struct early and the boot data can look like the phase machine;
		// observed the kick firing from the first lockstep event). Require
		// mode == 9 on BOTH players as the primary gate, mirroring the
		// harness kick which is only invoked from the post-link watch loop.
		uint8_t* iw = (uint8_t*) gba->memory.iwram;
		if (iw) {
			inLink[i] = iw[0x6D10] == 9;
		}
	}
	// Self-arm the link-screen assist (2026-09-10): the discovery echo in
	// _fsAssistTick is what lets the FS linking screen's handshake complete
	// (the games exchange FEFE probes whose counters stay offset, so under
	// emulation neither ever sees a consistent partner and the discovery loops
	// until the retry counter (round=4) resets the role machine to
	// phase=0/role=0 -- observed in BOTH the browser WASM and the threaded
	// harness). The host was supposed to arm it (SetFSArmed: "the games are
	// visibly frozen at the link screen"), but only the test harness ever
	// called it -- the app and the web build never did, so the assist was dead
	// code everywhere except threaded_link.c and every link screen stalled.
	// Arm it here from the same signal the host would use: the FS cart
	// identified AND every attached game sitting in the link-screen mode
	// (IWRAM 0x6D10 == 9, which the title/menus are not -- they sit at 0/8/13).
	// Self-arming is idempotent and the assist still hands off to raw data once
	// real payload rounds flow, so a host override stays a no-op refinement.
	if (!coordinator->fsAssistArmed) {
		bool allLink = nPlayers >= 2;
		for (i = 0; i < nPlayers; ++i) {
			allLink = allLink && inLink[i];
		}
		if (allLink) {
			coordinator->fsAssistArmed = true;
			mLOG(GBA_SIO, WARN, "FS assist: armed at the link screen (all %d players in mode 9)", nPlayers);
		}
	}
	for (i = 0; i < nPlayers; ++i) {
		if (!inLink[i]) {
			_fsKickLogDecline(coordinator, "not-in-link-screen", players, nPlayers);
			return;
		}
	}
	// Title-screen discrimination (2026-09-07): the FS title screen runs the
	// SAME phase machine (link detection probe) with phase=2 role=8/0 a=3
	// b=3 got12=1 recv=12, so the mode-9 + phase-signature gates above pass
	// before START is even pressed and the kick corrupts the pre-link state
	// (observed firing from the first lockstep event, at the title; games
	// then stuck at 9:2 forever). The discriminator found via the harness
	// pre-START probe: at the title the link-status machine is DISENGAGED
	// -- IWRAM 0x03000FC0 (lstat struct) all zeros and sub-state 0x03000BFC
	// == 0 -- while the linking screen and the post-name phase machine keep
	// it engaged (lstat state byte != 0, ssub >= 1). Require engagement on
	// BOTH players.
	bool engaged[MAX_GBAS] = {false};
	for (i = 0; i < nPlayers; ++i) {
		struct GBA* gba = players[i]->driver->d.p->p;
		uint8_t* iw = (uint8_t*) gba->memory.iwram;
		if (iw) {
			// lstat struct at 0x03000FC0: +1 is the state byte.
			engaged[i] = iw[0x0FC1] != 0 || iw[0x0BFC] != 0;
		}
	}
	for (i = 0; i < nPlayers; ++i) {
		if (!engaged[i]) {
			_fsKickLogDecline(coordinator, "lstat-not-engaged", players, nPlayers);
			return;
		}
	}
	// Freeze signature: both games in the phase machine AND NOT both fully
	// received (recvIdx==13 + got12 both) -- that's the game actively
	// validating, not a stall. Secondary gate (belt-and-suspenders on top
	// of the mode-9 check): require a genuine phase-machine signature on
	// BOTH players -- role byte 0 or 8, phase byte 1 or 2, and the
	// send/recv pointers at st+28/+40/+44 all inside EWRAM
	// (0x02030000-0x02040000, 4-aligned).
	uint32_t recv[MAX_GBAS];
	bool stuck = true;
	for (i = 0; i < nPlayers; ++i) {
		uint32_t sendPtr, histPtr, altPtr;
		memcpy(&sendPtr, st[i] + 28, 4);
		memcpy(&histPtr, st[i] + 40, 4);
		memcpy(&altPtr, st[i] + 44, 4);
		bool sig = st[i][0] == 0 || st[i][0] == 8; /* role */
		sig = sig && (st[i][1] == 0 || st[i][1] == 1 || st[i][1] == 2); /* phase; 0 = post-retry reset */
		sig = sig && (sendPtr & 3) == 0 && sendPtr >= 0x02030000 && sendPtr < 0x02040000;
		sig = sig && (histPtr & 3) == 0 && histPtr >= 0x02030000 && histPtr < 0x02040000;
		sig = sig && (altPtr & 3) == 0 && altPtr >= 0x02030000 && altPtr < 0x02040000;
		if (!sig) {
			_fsKickLogDecline(coordinator, "phase-sig-mismatch", players, nPlayers);
			stuck = false;
			break;
		}
	}
	if (stuck) {
		for (i = 0; i < nPlayers; ++i) {
			memcpy(&recv[i], st[i] + 24, 4);
		}
		bool allComplete = true;
		for (i = 0; i < nPlayers; ++i) {
			if (!(recv[i] == 13 && st[i][5] == 1)) {
				allComplete = false;
				break;
			}
		}
		if (allComplete) {
			// recv==13 + got12==1 on EVERY player is the "13-round discovery
			// completed" state. On real hardware the game then moves on; under
			// emulation it can freeze HERE with zero transfer activity -- the
			// games restore from a mid-link state file into exactly this
			// signature (recv=13 got12=1) and park forever (observed in the
			// cooperative repro: kicks invoking every ~2s, no decline logged,
			// no injection -- this silent early-return was the gate). Only
			// treat "complete" as healthy while recv is actually MOVING
			// between kick attempts (~2s of emulated time); a player frozen at
			// 13 needs the crafted block to break the 12/13 stall just like a
			// player stuck at 11/12, and the inLink (mode==9) gate still
			// stops re-injection once the games leave the link screen.
			bool moved = false;
			for (i = 0; i < nPlayers; ++i) {
				if (recv[i] != coordinator->fsKickLastRecv[i]) {
					moved = true;
				}
				coordinator->fsKickLastRecv[i] = recv[i];
			}
			if (moved) {
				_fsKickLogDecline(coordinator, "already-complete", players, nPlayers);
				stuck = false;
			}
		}
		// ROUND-BOUNDARY ONLY gate (2026-09-08, fs7 vs fs7b): the harness kick
		// (host thread, every ~2s wall) fires when the games are PARKED at a
		// round boundary -- recv==0 (round timed out / not started), recv>=12
		// (a 12-transfer round is complete, got12 pending validation), or
		// recv==-1 (probe reset just happened) -- and reaches gameplay
		// (fs4/fs11/fs7b all hit mode 2:9). The driver-side inline kick fired
		// MID-ROUND (recv=4-9, fs7/fs12) and NEVER reached gameplay: injecting
		// got12/sendIdx=0 while the game is actively accumulating its own
		// 12-transfer round makes the acceptance scan validate a premature
		// half-round and desyncs the game's round accounting, so every natural
		// round times out (phase 1, round=4) and the games cycle forever.
		// Skip whenever ANY player is mid-round (recvIdx 1..11) AND advancing:
		// that game is progressing on its own and must not be disturbed.
		// 2026-09-10: "advancing" is recv actually MOVED since the previous
		// kick attempt (~2s of emulated time). A game stuck at recv=11 for
		// many attempts is the FS 12/13 discovery stall (one short of a full
		// round -- observed live in 4P: recv=12/11 for minutes with the kick
		// declining forever), NOT progress; the crafted block is exactly what
		// un-sticks that state, so allow the kick once recv stops moving.
		{
			bool midRound = false;
			bool progressed = false;
			for (i = 0; i < nPlayers; ++i) {
				if (recv[i] < 12 && recv[i] != 0 && recv[i] != (uint32_t) -1) {
					midRound = true;
				}
				if (recv[i] != coordinator->fsKickLastRecv[i]) {
					progressed = true;
				}
				coordinator->fsKickLastRecv[i] = recv[i];
			}
			if (midRound && progressed) {
				_fsKickLogDecline(coordinator, "mid-round-progress", players, nPlayers);
				stuck = false;
			}
		}
	}
	if (!stuck) {
		return;
	}

	// Crafted valid block (12 halfwords, sum == -15): counter, checksum,
	// 0x0021 "link active" marker, 0xFFFF, then 8 zero values. Slots 0-1 get
	// this block (both games build the identical table); slots 2-3 stay FFFF
	// (no 3rd/4th device -- their sum is rejected, which is correct).
	uint16_t blk[12] = {0x00B0, 0xFF21, 0x0021, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0};
	for (i = 0; i < nPlayers; ++i) {
		struct GBA* gba = players[i]->driver->d.p->p;
		uint32_t histPtr, altPtr;
		memcpy(&histPtr, st[i] + 44, 4);
		memcpy(&altPtr, st[i] + 40, 4);
		uint8_t* hist = (uint8_t*) gba->memory.wram + (histPtr & (GBA_SIZE_EWRAM - 1));
		uint8_t* alt = (uint8_t*) gba->memory.wram + (altPtr & (GBA_SIZE_EWRAM - 1));
		int which;
		for (which = 0; which < 2; ++which) {
			uint8_t* h = which ? hist : alt;
			int k, j;
			// Every ATTACHED slot gets the crafted active block. Slot k
			// receives player k's value each round and the acceptance scan
			// accepts each slot whose sum == -15 (only present players
			// count), so with 4 players all four slots must be valid -- the
			// old 2P assumption (slots 2-3 = FFFF, "no 3rd/4th device")
			// made P2-P4's scans reject every round and only P1 advanced
			// (2026-09-09, browser 4P). Slots beyond nAttached stay FFFF.
			for (k = 0; k < coordinator->nAttached; ++k) {
				memcpy(h + 28 * k, blk, 24);
			}
			for (k = coordinator->nAttached; k < 4; ++k) {
				for (j = 0; j < 12; ++j) {
					uint16_t ff = 0xFFFF;
					memcpy(h + 28 * k + 2 * j, &ff, 2);
				}
			}
		}
		// got12 = 1; master (playerId 0) recvIdx = 13; sendIdx = 0 for both so
		// their handlers re-send the tables on the next transfers.
		st[i][5] = 1;
		uint32_t thirteen = 13;
		uint32_t zero = 0;
		if (players[i]->playerId == 0) {
			memcpy(st[i] + 24, &thirteen, 4);
		}
		memcpy(st[i] + 20, &zero, 4);
		// Re-enter a reset phase machine (2026-09-10): in 4P the secondaries'
		// discovery exhausts its 4 retries and the machine halts at phase=0
		// (post-retry reset) -- observed live: the master's machine stays at
		// phase=2 and its sub-state advances 1->4 under A-taps while P2-P4
		// sit frozen at phase=0/ssub=4 with the crafted block + got12 present
		// (the acceptance scan never runs at phase 0). Lift the machine back
		// into phase 1 so the scan can validate the block we just wrote; the
		// game's own handlers then re-send their tables and advance.
		if (st[i][1] == 0) {
			st[i][1] = 1;
		}
		// Link-status success latch (2026-09-10): the games' own rounds
		// overwrite the crafted +44 histories before the state-5 payload
		// check (0x8037b5c) can run, so [0x03000FC3]|=0x40 never sets and
		// sub-state 4's A-confirm gate stays closed -- every player parks at
		// ssub=4 with the crafted block + got12=1 present (verified via
		// EWRAM/IWRAM dumps: block in hist+alt, got12=1 recv=13, ssub=4,
		// fC3=00). The sub-state machine only waits on that bit (the A-check
		// is gated on the link-status bit + the 120-frame counter), so set
		// the latch and open the char-select gate directly. The game clears
		// these on its own once it advances; re-injecting is idempotent and
		// gated on mode==9 + the phase signature, so a game that already
		// left the link screen is never touched.
		if (gba->memory.iwram) {
			uint8_t* iw = (uint8_t*) gba->memory.iwram;
			if ((iw[0x0FC3] & 0x40) == 0) {
				mLOG(GBA_SIO, WARN, "FS assist: setting link-status success bit for P%d (fC3=%02X -> %02X)",
					i + 1, (unsigned) iw[0x0FC3], (unsigned) (iw[0x0FC3] | 0x40));
			}
			iw[0x0FC3] |= 0x40;
			iw[0x0FC8] = 2;
		}
	}
	if (++coordinator->fsKicks % 8 == 0) {
		// Per-player values for ALL attached players (2P run: two entries;
		// 4P run: four) so a stuck session shows who is behind.
		// 2026-09-10: include the link-status machine bytes -- ssub
		// (0x03000BFC), the lstat state byte (0x03000FC1), the success bit
		// latch [0x03000FC3]|=0x40 and the char-select gate [0x03000FC8]
		// that the state-5 payload check drives -- so a run shows whether the
		// crafted block ever completes the success path (it sits in the +44
		// histories with got12=1, yet the machine resets to phase=0; this
		// makes the sub-state visible at kick time).
		char kbuf[256] = "";
		int klen = 0;
		int ki;
		for (ki = 0; ki < nPlayers && klen < 224; ++ki) {
			struct GBA* gba = players[ki]->driver->d.p->p;
			uint8_t* iw = (uint8_t*) gba->memory.iwram;
			klen += snprintf(kbuf + klen, sizeof(kbuf) - klen,
				"P%d phase=%u recv=%u got12=%u ssub=%u lstat=%u fC3=%02X fC8=%u%s",
				ki + 1, (unsigned) st[ki][1], (unsigned) recv[ki], (unsigned) st[ki][5],
				iw ? (unsigned) iw[0x0BFC] : 0, iw ? (unsigned) iw[0x0FC1] : 0,
				iw ? (unsigned) iw[0x0FC3] : 0, iw ? (unsigned) iw[0x0FC8] : 0,
				ki + 1 < nPlayers ? ", " : "");
		}
		mLOG(GBA_SIO, WARN, "FS assist: kicked deadlock (%s)", kbuf);
	}
}

static void GBASIOLockstepDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferMode == GBA_SIO_MULTI) {
		struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (!player->dataReceived) {
			mLOG(GBA_SIO, WARN, "MULTI did not receive data. Are we running behind?");
			// FS stall assist: a transfer that completes without the secondary
			// acking delivers FFFF, which reads as "no device present" and
			// makes the master's game retry the same round forever (Four Swords
			// discovery/post-name handshake). Real hardware holds the line at
			// each device's last-driven level instead, so while the assist is
			// armed we deliver each device's OWN last-driven value in its slot
			// (captured at transfer start) -- each game then sees its partners'
			// real table values and its link state machine can advance.
			// Real-data rounds (dataReceived=true) are untouched, and the
			// assist is FS-only and host-armed, so no other game is affected.
			if (coordinator->fsAssistArmed) {
				uint16_t master = coordinator->multiData[0];
				int k;
				// Per-slot delivery, NOT echo-the-master-to-every-slot: the
				// 2026-09-06 experiment that echoed the master's value into all
				// four slots hid the slaves' real table entries (slave values
				// 00BE/00BF replaced by the master's), so the master's game
				// never saw the slaves' tables. slot k holds player k's own
				// sent value, which is exactly what synchronized hardware
				// presents. FEFE probe rounds still echo the master to every
				// slot (that's what a probe looks like on real hardware).
				if (_fsAssistHandshakeRound(coordinator->multiData, coordinator->nAttached)) {
					for (k = 0; k < 4; ++k) {
						data[k] = master;
					}
				} else {
					memcpy(data, coordinator->multiData, sizeof(uint16_t) * 4);
				}
				// Stall diagnostics (2026-09-10): when rounds keep stalling
				// (MULTI flood) log WHO failed to ack -- the finisher, the
				// wait mask, the transfer latch, and each player's
				// asleep/dataReceived/sent-value. Rate-limited to every 128th
				// stall so a stuck session shows its shape without flooding.
				{
					static unsigned fsStallDiag = 0;
					if (++fsStallDiag % 128 == 0) {
						char sbuf[192] = "";
						int slen = 0;
						int si;
						for (si = 0; si < coordinator->nAttached && slen < 168; ++si) {
							struct GBASIOLockstepPlayer* sp = TableLookup(&coordinator->players, coordinator->attachedPlayers[si]);
							if (!sp) {
								continue;
							}
							slen += snprintf(sbuf + slen, sizeof(sbuf) - slen,
								"P%d(a=%d dr=%d s=%04X) ",
								si + 1, sp->asleep ? 1 : 0, sp->dataReceived ? 1 : 0,
								(unsigned) coordinator->multiData[si]);
						}
						mLOG(GBA_SIO, WARN, "FS stall diag: finisher=pid%d nAtt=%d waiting=%X active=%d | %s",
							player->playerId, coordinator->nAttached,
							(unsigned) coordinator->waiting, coordinator->transferActive ? 1 : 0, sbuf);
					}
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
			// the next transfer can proceed (see lockstep.c mirror).
			if (player->playerId == 0 && coordinator->transferActive) {
				mLOG(GBA_SIO, DEBUG, "FS stall: clearing stuck transferActive");
				coordinator->transferActive = false;
				GBASIOLockstepCoordinatorWakePlayers(coordinator);
			}
		} else {
			mLOG(GBA_SIO, DEBUG, "MULTI transfer finished: %04X %04X %04X %04X",
			     coordinator->multiData[0],
			     coordinator->multiData[1],
			     coordinator->multiData[2],
			     coordinator->multiData[3]);
			memcpy(data, coordinator->multiData, sizeof(uint16_t) * 4);

			// --- Four Swords handshake assist ---
			if (player->playerId == 0) {
				_fsAssistTick(coordinator, lockstep, data);
			}
			// The echo flag is latched by the primary's tick, so both players'
			// completions for the SAME transfer make the same decision -- no
			// per-round recompute needed (and no thread-ordering race).
			// Echo DISCOVERY rounds (FEFE probes and the games' value-checksum
			// pairs) so every game sees its partners agreeing with it, which is
			// what synchronized hardware presents; real table rounds pass
			// through raw (2026-09-06: echoing those overwrote real entries).
			if (coordinator->fsAssistOn && _fsAssistHandshakeRound(data, coordinator->nAttached)) {
				uint16_t master = data[0];
				int k;
				for (k = 0; k < 4; ++k) {
					data[k] = master;
				}
				mLOG(GBA_SIO, DEBUG, "FS assist: echo %04X to P%d", master, player->playerId);
			}
		}
		player->dataReceived = false;
		if (player->playerId == 0) {
			// Per-transfer hard sync. This was dropped as the "H1 experiment"
			// (a9ca9b441) to test whether the barrier was desyncing FS's rapid
			// handshake; the experiment measured the opposite of what it hoped --
			// with the barrier gone FS goes right back to cycling discovery on
			// the linking screen, which is the state the loosen-timing patch had
			// just fixed. Restore it: PROJECT_LOG's 2026-08-17 entry (FS 2P passes
			// the linking screen into character-select) was recorded with this
			// call ACTIVE, on top of the loosened timing. H1's own commit message
			// reports "the handshake still cycles" as its result, so the barrier
			// is not the handshake's problem -- dropping it only loses the
			// end-of-round realignment that keeps the two games on the same round.
			_hardSync(coordinator, player);
		}
	}
	MutexUnlock(&coordinator->mutex);
}

static uint8_t GBASIOLockstepDriverFinishNormal8(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	uint8_t data = 0xFF;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferMode == GBA_SIO_NORMAL_8) {
		struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
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

static uint32_t GBASIOLockstepDriverFinishNormal32(struct GBASIODriver* driver) {
	struct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	uint32_t data = 0xFFFFFFFF;
	MutexLock(&coordinator->mutex);
	if (coordinator->transferMode == GBA_SIO_NORMAL_32) {
		struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
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

void GBASIOLockstepCoordinatorSetFSSuppressed(struct GBASIOLockstepCoordinator* coordinator, bool suppressed) {
	// Master kill switch for the Four Swords link assist and its deadlock kick.
	//
	// Why it exists: the assist pokes the game's private IWRAM/EWRAM link state
	// (the +40/+44 recv histories, got12/recvIdx, the phase byte, and the
	// 0x03000FC3 success latch) every ~2 s for as long as the games sit in
	// mode 9. Across every model measured it has never moved the games off the
	// linking screen, and the sessions that run with it show the emulated game
	// fetching from unmapped memory (Out of bounds ROM 0x0D000000, Bad BIOS
	// 0x00000000, Bad memory 0xffffeXXX) -- the signature of a game executing a
	// corrupted state machine. It also does nothing measurable when disabled: a
	// 4P A/B with the ack barrier in place logged zero stalled transfers BOTH
	// ways. Default it off; keep it available for A/B runs.
	MutexLock(&coordinator->mutex);
	coordinator->fsSuppressed = suppressed;
	if (suppressed) {
		coordinator->fsAssistEnabled = false;
		coordinator->fsAssistChecked = true;
		coordinator->fsAssistArmed = false;
		coordinator->fsAssistOn = false;
		coordinator->fsHandshakeRounds = 0;
		coordinator->fsQuietRounds = 0;
		coordinator->fsKickEnabled = false;
		coordinator->fsKickCountdown = 0;
		mLOG(GBA_SIO, WARN, "FS assist: suppressed by host");
	} else {
		// Re-probe on the next opportunity and re-enable the kick.
		coordinator->fsAssistChecked = false;
		coordinator->fsKickEnabled = true;
		mLOG(GBA_SIO, WARN, "FS assist: re-enabled by host");
	}
	MutexUnlock(&coordinator->mutex);
}

void GBASIOLockstepCoordinatorSetFSKickEnabled(struct GBASIOLockstepCoordinator* coordinator, bool enabled) {
	MutexLock(&coordinator->mutex);
	coordinator->fsKickEnabled = enabled;
	MutexUnlock(&coordinator->mutex);
}

void GBASIOLockstepCoordinatorSetFSArmed(struct GBASIOLockstepCoordinator* coordinator, bool armed) {
	// Host-level gate: the app (which can see the video) arms the assist only
	// when the games are actually at the frozen FS link screen. The SIO stream
	// alone cannot distinguish the (identical) discovery cycle that runs during
	// normal menu navigation, where faking success would break the game flow.
	MutexLock(&coordinator->mutex);
	coordinator->fsAssistArmed = armed;
	if (!armed) {
		coordinator->fsAssistOn = false;
		coordinator->fsHandshakeRounds = 0;
		coordinator->fsQuietRounds = 0;
	}
	MutexUnlock(&coordinator->mutex);
}

void GBASIOLockstepCoordinatorInit(struct GBASIOLockstepCoordinator* coordinator) {
	memset(coordinator, 0, sizeof(*coordinator));
	coordinator->fsKickEnabled = true;
	MutexInit(&coordinator->mutex);
	TableInit(&coordinator->players, 8, free);
}

void GBASIOLockstepCoordinatorDeinit(struct GBASIOLockstepCoordinator* coordinator) {
	MutexDeinit(&coordinator->mutex);
	TableDeinit(&coordinator->players);
}

void GBASIOLockstepCoordinatorAttach(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepDriver* driver) {
	if (driver->coordinator && driver->coordinator != coordinator) {
		// TODO
		abort();
	}
	driver->coordinator = coordinator;
}

void GBASIOLockstepCoordinatorDetach(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepDriver* driver) {
	if (driver->coordinator != coordinator) {
		// TODO
		abort();
		return;
	}
	MutexLock(&coordinator->mutex);
	struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, driver->lockstepId);
	if (player) {
		_removePlayer(coordinator, player);
	}
	MutexUnlock(&coordinator->mutex);
	driver->coordinator = NULL;
}

int32_t _untilNextSync(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepPlayer* player) {
	int32_t cycle = coordinator->cycle - GBASIOLockstepTime(player);
	if (player->playerId == 0) {
		if (coordinator->nAttached < 2) {
			cycle += UNLOCKED_INTERVAL;
		} else {
			cycle += LOCKSTEP_INTERVAL;
		}
	}
	return cycle;
}

void _advanceCycle(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepPlayer* player) {
	int32_t newCycle = GBASIOLockstepTime(player);
	mASSERT_DEBUG(newCycle - coordinator->cycle >= 0);
	coordinator->nextHardSync -= newCycle - coordinator->cycle;
	coordinator->cycle = newCycle;
}

void _removePlayer(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepPlayer* player) {
	struct GBASIOLockstepEvent event = {
		.type = SIO_EV_DETACH,
		.playerId = player->playerId,
		.timestamp = GBASIOLockstepTime(player),
	};
	_enqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));

	coordinator->waiting = 0;
	coordinator->transferActive = false;

	TableRemove(&coordinator->players, player->driver->lockstepId);
	_reconfigPlayers(coordinator);

	struct GBASIOLockstepPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
	if (runner) {
		GBASIOLockstepPlayerWake(runner);
	}
	_verifyAwake(coordinator);
}

void _reconfigPlayers(struct GBASIOLockstepCoordinator* coordinator) {
	size_t players = TableSize(&coordinator->players);
	memset(coordinator->attachedPlayers, 0, sizeof(coordinator->attachedPlayers));
	if (players == 0) {
		mLOG(GBA_SIO, WARN, "Reconfiguring player IDs with no players attached somehow?");
	} else if (players == 1) {
		struct TableIterator iter;
		mASSERT_LOG(GBA_SIO, TableIteratorStart(&coordinator->players, &iter), "Trying to reconfigure 1 player with empty player list");
		unsigned p0 = TableIteratorGetKey(&coordinator->players, &iter);
		coordinator->attachedPlayers[0] = p0;

		struct GBASIOLockstepPlayer* player = TableIteratorGetValue(&coordinator->players, &iter);
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
			struct GBASIOLockstepPlayer* player = TableIteratorGetValue(&coordinator->players, &iter);
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
					struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, pid);
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
		struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, pid);
		if (!player) {
			coordinator->attachedPlayers[i] = 0;
		} else {
			++nAttached;
		}
	}
	coordinator->nAttached = nAttached;
}

static void _setData(struct GBASIOLockstepCoordinator* coordinator, uint32_t id, struct GBASIO* sio) {
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

void _setReady(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepPlayer* activePlayer, int playerId, enum GBASIOMode mode) {
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

void _hardSync(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepPlayer* player) {
	mASSERT_DEBUG(player->playerId == 0);
	struct GBASIOLockstepEvent event = {
		.type = SIO_EV_HARD_SYNC,
		.playerId = 0,
		.timestamp = GBASIOLockstepTime(player),
	};
	_enqueueEvent(coordinator, &event, TARGET_SECONDARY);
	GBASIOLockstepCoordinatorWaitOnPlayers(coordinator, player);
}

void _enqueueEvent(struct GBASIOLockstepCoordinator* coordinator, const struct GBASIOLockstepEvent* event, uint32_t target) {
	mLOG(GBA_SIO, DEBUG, "Enqueuing event of type %X from %i for target %X at timestamp %X",
	                      event->type, event->playerId, target, event->timestamp);

	int i;
	for (i = 0; i < coordinator->nAttached; ++i) {
		if (!(target & TARGET(i))) {
			continue;
		}
		struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		mASSERT_LOG(GBA_SIO, player->freeList, "No free events");
		struct GBASIOLockstepEvent* newEvent = player->freeList;
		player->freeList = newEvent->next;

		memcpy(newEvent, event, sizeof(*event));
		struct GBASIOLockstepEvent** previous = &player->queue;
		struct GBASIOLockstepEvent* next = player->queue;
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

void _lockstepEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBASIOLockstepDriver* lockstep = context;
	struct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	MutexLock(&coordinator->mutex);
	struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	struct GBASIO* sio = player->driver->d.p;
	mASSERT_LOG(GBA_SIO, player->playerId >= 0 && player->playerId < 4, "Invalid multiplayer ID %i", player->playerId);

	bool wasDetach = false;
	if (player->queue && player->queue->type == SIO_EV_DETACH) {
		mLOG(GBA_SIO, DEBUG, "Player %i detached at timestamp %X, picking up the pieces",
		                      player->queue->playerId, player->queue->timestamp);
		wasDetach = true;
	}
	if (player->playerId == 0 && GBASIOLockstepTime(player) - coordinator->cycle >= 0) {
		// We are the clock owner; advance the shared clock. However, if we just became
		// the clock owner (by the previous one disconnecting) we might be slightly
		// behind the shared clock. We should wait a bit if needed in that case.
		_advanceCycle(coordinator, player);
		if (!coordinator->transferActive) {
			GBASIOLockstepCoordinatorWakePlayers(coordinator);
		}
		if (coordinator->nextHardSync < 0 && !coordinator->waiting) {
			_hardSync(coordinator, player);
		}
		// FS deadlock kick (2026-09-07): while the games are stuck in the
		// post-name phase machine, re-inject the valid blocks + got12/recvIdx
		// (the game consumes got12 on each acceptance-scan attempt). Cheap and
		// idempotent; self-gated on the FS cart + freeze signature inside
		// _fsAssistKick.
		//
		// Throttle: the lockstep event fires every LOCKSTEP_INTERVAL (4096
		// cycles ~ 244us), so every-128-events = ~31ms between attempts. That
		// is FAR too aggressive: the successful harness kick fires once per
		// ~2s watch-loop iteration, and a 31ms re-injection window never lets
		// the games' own state machine consume the block and advance (they
		// just cycle phase 2->1->0 forever; observed in fs6f). Match the
		// harness cadence instead: only attempt once per ~2s of emulated time
		// (2s * 16777216 Hz / 4096-cycle event = ~8192 events).
		// Read-only link diagnostic (2026-09-11). The FS assist is off by default
		// now, which also removed the only always-on view of the link state, so a
		// session logged nothing at all about SIO and we could not tell whether
		// the cores were even talking.
		//
		// This reads (never writes) each player's SIO mode and SIOCNT once per ~2 s
		// of emulated time. It answers the one question that would make FS wait
		// forever with NO stall warnings: is every attached core actually in MULTI
		// mode? `_setReady` only sets the SD "all units ready" bit (SIOCNT bit 3)
		// once all attached players report the same mode, and a game in MULTI mode
		// blocks on that bit before it starts any transfer. One core stuck in
		// NORMAL/GPIO therefore parks every other core silently.
		{
			static unsigned sioDiagCountdown = 0;
			if (++sioDiagCountdown >= 8192) {
				sioDiagCountdown = 0;
				char dbuf[256] = "";
				int dlen = 0;
				int di;
				for (di = 0; di < coordinator->nAttached && dlen < 200; ++di) {
					struct GBASIOLockstepPlayer* dp = TableLookup(&coordinator->players, coordinator->attachedPlayers[di]);
					if (!dp || !dp->driver || !dp->driver->d.p) {
						continue;
					}
					dlen += snprintf(dbuf + dlen, sizeof(dbuf) - dlen,
						"P%d(mode=%d sio=%04X msg=%04X) ", di + 1, (int) dp->mode,
						(unsigned) dp->driver->d.p->siocnt,
						(unsigned) dp->driver->d.p->p->memory.io[GBA_REG(SIOMLT_SEND)]);
				}
				mLOG(GBA_SIO, WARN, "SIO diag: nAtt=%d xferMode=0x%X active=%d | %s",
					 coordinator->nAttached, (unsigned) coordinator->transferMode,
					 coordinator->transferActive ? 1 : 0, dbuf);
			}
		}
		if (coordinator->fsKickEnabled && ++coordinator->fsKickCountdown >= 8192) {
			coordinator->fsKickCountdown = 0;
			// The "invoking" log lives inside _fsAssistKick, past the FS-cart
			// gate, so a non-FS game does not emit it twice a second forever.
			_fsAssistKick(coordinator);
		}
	}

	int32_t nextEvent = _untilNextSync(coordinator, player);
	while (true) {
		struct GBASIOLockstepEvent* event = player->queue;
		if (!event) {
			break;
		}
		if (event->timestamp > GBASIOLockstepTime(player)) {
			break;
		}
		player->queue = event->next;
		struct GBASIOLockstepEvent reply = {
			.playerId = player->playerId,
			.timestamp = GBASIOLockstepTime(player),
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
			GBASIOLockstepCoordinatorAckPlayer(coordinator, player);
			GBASIOLockstepPlayerSleep(player);
			break;
		case SIO_EV_TRANSFER_START:
			_setData(coordinator, player->playerId, sio);
			nextEvent = event->finishCycle - GBASIOLockstepTime(player) - cyclesLate;
			player->driver->d.p->siocnt |= 0x80;
			mTimingDeschedule(&sio->p->timing, &sio->completeEvent);
			mTimingSchedule(&sio->p->timing, &sio->completeEvent, nextEvent);
			GBASIOLockstepCoordinatorAckPlayer(coordinator, player);
			break;
		case SIO_EV_MODE_SET:
			if (coordinator->transferActive && player->mode != event->mode) {
				mLOG(GBA_SIO, DEBUG, "Switching modes while transfer is active");
				_abortTransfer(coordinator, player);
			}
			_setReady(coordinator, player, event->playerId, event->mode);
			if (event->playerId == 0) {
				GBASIOLockstepCoordinatorAckPlayer(coordinator, player);
				GBASIOLockstepPlayerSleep(player);
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
	if (player->queue && player->queue->timestamp - GBASIOLockstepTime(player) < nextEvent) {
		nextEvent = player->queue->timestamp - GBASIOLockstepTime(player);
	}

	if (player->playerId != 0 && nextEvent <= LOCKSTEP_INTERVAL) {
		if (!player->queue || wasDetach) {
			GBASIOLockstepPlayerSleep(player);
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
	// re-entering _lockstepEvent forever and busy-spinning the emulation thread.
	// Clamp to a short positive delay so we re-check sync soon instead. The
	// player is throttled by GBASIOLockstepPlayerSleep (its host thread is
	// skipped by the frame loop) rather than by spinning here.
	if (nextEvent <= 0) {
		mLOG(GBA_SIO, DEBUG, "Lockstep: clamping non-positive sync delay %d (pid %d)",
		     nextEvent, player->playerId);
		nextEvent = 4;
	}
	mTimingSchedule(timing, &lockstep->event, nextEvent);
}

int32_t GBASIOLockstepTime(struct GBASIOLockstepPlayer* player) {
	return mTimingCurrentTime(&player->driver->d.p->p->timing) - player->cycleOffset;
}

void GBASIOLockstepCoordinatorWaitOnPlayers(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepPlayer* player) {
	mASSERT_LOG(GBA_SIO, !coordinator->waiting, "Multiplayer desynchronized: coordinator still waiting");
	mASSERT_LOG(GBA_SIO, !player->asleep, "Multiplayer desynchronized: player asleep");
	mASSERT_LOG(GBA_SIO, player->playerId == 0, "Multiplayer desynchronized: invalid player %i attempting to coordinate", player->playerId);
	if (coordinator->nAttached < 2) {
		return;
	}

	_advanceCycle(coordinator, player);
	mLOG(GBA_SIO, DEBUG, "Primary waiting for players to ack");
	coordinator->waiting = ((1 << coordinator->nAttached) - 1) & ~TARGET(player->playerId);
	GBASIOLockstepPlayerSleep(player);
	GBASIOLockstepCoordinatorWakePlayers(coordinator);

	_verifyAwake(coordinator);
}

void GBASIOLockstepCoordinatorWakePlayers(struct GBASIOLockstepCoordinator* coordinator) {
	int i;
	for (i = 1; i < coordinator->nAttached; ++i) {
		if (!coordinator->attachedPlayers[i]) {
			continue;
		}
		struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		GBASIOLockstepPlayerWake(player);
	}
}

void GBASIOLockstepPlayerWake(struct GBASIOLockstepPlayer* player) {
	if (!player->asleep) {
		return;
	}
	player->asleep = false;
	player->driver->user->wake(player->driver->user);
}

void GBASIOLockstepCoordinatorAckPlayer(struct GBASIOLockstepCoordinator* coordinator, struct GBASIOLockstepPlayer* player) {
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
				struct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
				player->dataReceived = true;
			}

			coordinator->transferActive = false;
		}

		coordinator->nextHardSync = HARD_SYNC_INTERVAL;
		struct GBASIOLockstepPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
		GBASIOLockstepPlayerWake(runner);
		// 4-player barrier (2026-09-10): with more than one secondary, the
		// non-final ackers must NOT run on to their transfer completion yet.
		// dataReceived is only set when the LAST ack lands, so under the
		// sequential cooperative model an earlier acker reaching its
		// completeEvent first delivers garbage ("MULTI did not receive data"
		// flood, FS 4P stalls forever; 2P never showed it because its single
		// secondary is always the final acker). Wake the slept secondaries so
		// every player completes the same round with dataReceived=true.
		GBASIOLockstepCoordinatorWakePlayers(coordinator);
	} else if (coordinator->transferActive) {
		// A secondary that acked TRANSFER_START but is not the final acker:
		// hold it here (sleep = end its step early, frame loop skips it)
		// until the last ack wakes everyone. Without this it reaches its
		// completeEvent before dataReceived is set and its game reads FFFF.
		GBASIOLockstepPlayerSleep(player);
	}
	// NOTE: do NOT sleep the secondary here. Sleeping right after it acks a
	// TRANSFER_START freezes its core until the master's next 4096-cycle tick
	// wakes it, so the secondary's game observes the transfer completion LATE
	// relative to the master. Four Swords' handshake validates 12 halfwords of
	// table data per round and the two games drift out of sync when completions
	// land on different cycles, so P2's acceptance scan never sees a complete
	// table (b stays 0 and the round times out forever). Let the secondary run
	// on to finishCycle so both sides complete the transfer at the SAME cycle.
	// Callers that DO need the sleep (hard sync, mode set) call
	// GBASIOLockstepPlayerSleep themselves. Matches the rendezvous driver.
}

void GBASIOLockstepPlayerSleep(struct GBASIOLockstepPlayer* player) {
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

size_t GBASIOLockstepCoordinatorAttached(struct GBASIOLockstepCoordinator* coordinator) {
	size_t count;
	MutexLock(&coordinator->mutex);
	count = TableSize(&coordinator->players);
	MutexUnlock(&coordinator->mutex);
	return count;
}
