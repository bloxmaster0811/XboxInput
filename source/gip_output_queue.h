#pragma once

#include <stdint.h>
#include <string.h>

// Portable queue policy. The Xbox transport holds its session spinlock around
// every operation. USB execution and user callbacks run outside that lock.
// Active storage is retained even after Stop() until its completion arrives.
struct XboxInputOutputPacket {
	uint8_t bytes[64];
	uint32_t length;
	uint32_t callback;
	uint16_t motors;
	bool rumble;
};

struct XboxInputOutputQueue {
	// The protocol drivers require a 10 ms recovery window, but 17559 can lock
	// after a title repeatedly restarts the controller's persistent GIP effect at
	// that minimum cadence. Keep the most recent state and submit it at most ten
	// times per second; a zero-motor stop is still delivered on the next slot.
	enum { CAPACITY = 32, RUMBLE_DELAY_MS = 100, MAX_RUMBLE_ERRORS = 3 };
	XboxInputOutputPacket commands[CAPACITY];
	XboxInputOutputPacket active;
	uint32_t head, count;
	uint8_t sequence;
	bool stopped, activeValid, completionPending;
	bool rumbleDesiredValid, rumbleCompletedValid, rumbleDeadlineValid;
	uint16_t rumbleDesired, rumbleCompleted;
	uint32_t rumbleDue, rumbleErrors;
	int32_t completionStatus;
	uint32_t enqueued, submitted, completed, failures, overflows;
	uint32_t unexpectedCompletions;

	void Initialize() {
		memset(this, 0, sizeof(*this));
		sequence = 1;
	}

	uint8_t NextSequence() {
		const uint8_t next = sequence++;
		if (!sequence) sequence = 1;
		return next;
	}

	bool Enqueue(const uint8_t* bytes, uint32_t length, uint32_t callback) {
		if (stopped || !bytes || length < 4 || length > 64) return false;
		if (count == CAPACITY) {
			++overflows;
			Stop(); // Do not silently drop an ACK or initialization command.
			return false;
		}
		XboxInputOutputPacket& packet = commands[(head + count) % CAPACITY];
		memset(&packet, 0, sizeof(packet));
		memcpy(packet.bytes, bytes, length);
		packet.length = length;
		packet.callback = callback;
		++count;
		++enqueued;
		return true;
	}

	void RequestRumble(uint8_t left, uint8_t right) {
		if (stopped || rumbleErrors >= MAX_RUMBLE_ERRORS) return;
		rumbleDesired = (uint16_t)(((uint16_t)(left > 100 ? 100 : left) << 8) |
			(right > 100 ? 100 : right));
		rumbleDesiredValid = true;
	}

	// Called after the serialized startup STOP/setup transfer succeeds.
	void EstablishStoppedRumble(uint32_t now) {
		rumbleCompleted = 0;
		rumbleCompletedValid = true;
		rumbleErrors = 0;
		rumbleDeadlineValid = true;
		rumbleDue = now + RUMBLE_DELAY_MS;
	}

	bool Prepare(uint32_t now, bool allowRumble) {
		if (stopped || activeValid) return false;
		if (count) {
			active = commands[head];
			head = (head + 1) % CAPACITY;
			--count;
		}
		else {
			if (!allowRumble || !rumbleDesiredValid ||
				rumbleErrors >= MAX_RUMBLE_ERRORS ||
				(rumbleCompletedValid && rumbleDesired == rumbleCompleted) ||
				(rumbleDeadlineValid && (int32_t)(now - rumbleDue) < 0))
				return false;
			memset(&active, 0, sizeof(active));
			active.rumble = true;
			active.motors = rumbleDesired;
			active.length = 13;
			const uint8_t bytes[13] = {
				0x09, 0x00, NextSequence(), 0x09,
				0x00, 0x0F, 0x00, 0x00,
				(uint8_t)(rumbleDesired >> 8), (uint8_t)rumbleDesired,
				0xFF, 0x00, 0xEB
			};
			memcpy(active.bytes, bytes, sizeof(bytes));
		}
		activeValid = true;
		completionPending = false;
		++submitted;
		return true;
	}

	bool RecordCompletion(int32_t status) {
		if (!activeValid || completionPending) {
			++unexpectedCompletions;
			return false;
		}
		completionStatus = status;
		completionPending = true;
		return true;
	}

	// Dispatcher only, strictly before the USB API was entered. This is not
	// a cancellation mechanism for a submitted/outstanding USB request.
	void AbortUnsubmitted() {
		if (activeValid && !completionPending) {
			activeValid = false;
			--submitted;
		}
	}

	// Caller copies callback/status before reusing active or the transport TRB.
	bool Finish(uint32_t now, uint32_t* callback, int32_t* status) {
		if (!activeValid || !completionPending) return false;
		*callback = stopped ? 0 : active.callback;
		*status = completionStatus;
		++completed;
		if (completionStatus) ++failures;
		if (active.rumble) {
			rumbleDue = now + RUMBLE_DELAY_MS;
			rumbleDeadlineValid = true;
			if (!completionStatus) {
				rumbleCompleted = active.motors;
				rumbleCompletedValid = true;
				rumbleErrors = 0;
			}
			else ++rumbleErrors;
		}
		else if (completionStatus) {
			Stop(); // An ordered protocol/init command failed: stop dispatching.
			*callback = 0;
		}
		activeValid = false;
		completionPending = false;
		return true;
	}

	void Stop() {
		stopped = true;
		count = 0;
		rumbleDesiredValid = false;
		// Do not clear or reuse an outstanding transfer's active storage.
	}
};
