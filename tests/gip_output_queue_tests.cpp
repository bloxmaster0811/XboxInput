#include "../source/gip_output_queue.h"
#ifdef NDEBUG
#error Queue regression tests require assertions enabled
#endif
#include <assert.h>
#include <stdio.h>

static void Complete(XboxInputOutputQueue& q, uint32_t now, int32_t error = 0,
 uint32_t expectedCallback = 0) {
 uint32_t callback = 0xFFFFFFFF;
 int32_t status = 0;
 assert(q.RecordCompletion(error));
 assert(q.Finish(now, &callback, &status));
 assert(callback == expectedCallback && status == error);
 assert(!q.activeValid && !q.completionPending);
}

static void TestCopyFifoAndOwnership() {
 XboxInputOutputQueue q;
 q.Initialize();
 uint8_t power[] = {5,32,1,1,0};
 uint8_t ack[] = {1,32,7,0};
 assert(q.Enqueue(power, sizeof(power), 11));
 power[4] = 99; // Producer storage must no longer matter.
 assert(q.Prepare(0, false));
 assert(q.active.bytes[4] == 0);
 XboxInputOutputPacket owned = q.active;
 assert(q.Enqueue(ack, sizeof(ack), 22));
 q.RequestRumble(100, 80);
 for (uint32_t tick = 1; tick < 10000; ++tick) {
  assert(!q.Prepare(tick, true));
  assert(memcmp(&owned, &q.active, sizeof(owned)) == 0);
 }
 Complete(q, 10000, 0, 11);
 assert(q.Prepare(10000, true));
 assert(q.active.bytes[0] == 1); // ACK precedes rumble.
 Complete(q, 10000, 0, 22);
 assert(q.Prepare(10000, true));
 assert(q.active.rumble && q.active.motors == 0x6450);
 assert(q.active.length == 13 && q.active.bytes[0] == 9 &&
  q.active.bytes[5] == 15 && q.active.bytes[10] == 255 &&
  q.active.bytes[11] == 0 && q.active.bytes[12] == 0xEB);
 Complete(q, 10001);
 assert(q.submitted == 3 && q.completed == 3 && q.enqueued == 2);
}

static void TestFinalStopAndDelay() {
 XboxInputOutputQueue q;
	q.Initialize();
	q.EstablishStoppedRumble(100);
	q.RequestRumble(100, 100);
	assert(!q.Prepare(199, true));
	assert(q.Prepare(200, true));
	q.RequestRumble(40, 40);
	q.RequestRumble(0, 0); // Last XAM call, no future producer needed.
	assert(q.active.motors == 0x6464);
	Complete(q, 210);
	assert(!q.Prepare(309, true));
	assert(q.Prepare(310, true));
	assert(q.active.rumble && q.active.motors == 0);
	Complete(q, 311);
 assert(!q.Prepare(100000, true)); // Redundant zero suppressed.
 q.RequestRumble(255, 200);
 assert(q.Prepare(100000, true) && q.active.motors == 0x6464);
}

static void TestTickWrapAndSequence() {
 XboxInputOutputQueue q;
	q.Initialize();
	q.EstablishStoppedRumble(0xFFFFFFFC);
	q.RequestRumble(1, 2);
	assert(!q.Prepare(0xFFFFFFFF, true));
	assert(!q.Prepare(95, true));
	assert(q.Prepare(96, true));
 assert(q.active.bytes[2] == 1);
 Complete(q, 7);
 q.sequence = 255;
 assert(q.NextSequence() == 255 && q.NextSequence() == 1);
}

static void TestFailurePolicy() {
	XboxInputOutputQueue q;
	q.Initialize();
	q.RequestRumble(10, 20);
	for (uint32_t i = 0; i < 3; ++i) {
		assert(q.Prepare(i * 100, true));
		Complete(q, i * 100, -1);
		assert(!q.rumbleCompletedValid && q.rumbleErrors == i + 1);
	}
	assert(!q.Prepare(300, true));
	q.RequestRumble(50, 50);
	assert(q.rumbleDesired == 0x0A14 && q.failures == 3);
	// Protocol traffic remains possible when only runtime motors failed.
	const uint8_t ack[4] = {1,32,3,0};
	assert(q.Enqueue(ack, 4, 4) && q.Prepare(300, false));
	Complete(q, 300, -2); // Failed protocol command halts the queue.
 assert(q.stopped && !q.Prepare(1000, true));
}

static void TestRetirementOverflowAndCompletions() {
 XboxInputOutputQueue oldSession, newSession;
 oldSession.Initialize(); newSession.Initialize();
 const uint8_t command[4] = {5,32,1,0};
 assert(oldSession.Enqueue(command, 4, 100));
 assert(oldSession.Prepare(0, false));
 XboxInputOutputPacket owned = oldSession.active;
 oldSession.Stop();
 assert(memcmp(&owned, &oldSession.active, sizeof(owned)) == 0);
 assert(!oldSession.Enqueue(command, 4, 0) && !oldSession.Prepare(1, true));
 newSession.RequestRumble(100, 0);
 assert(newSession.Prepare(1, true));
 Complete(oldSession, 1); // Retired continuation must not execute.
 assert(newSession.activeValid); // Late old completion cannot release new owner.
 assert(!oldSession.RecordCompletion(0) && oldSession.unexpectedCompletions == 1);
 Complete(newSession, 2);
 oldSession.Initialize();
 assert(oldSession.Enqueue(command, 4, 100) && oldSession.Prepare(0, false));
 owned = oldSession.active;
 for (unsigned i = 0; i < XboxInputOutputQueue::CAPACITY; ++i)
  assert(oldSession.Enqueue(command, 4, i));
 assert(!oldSession.Enqueue(command, 4, 0));
 assert(oldSession.overflows == 1 && oldSession.stopped);
 assert(memcmp(&owned, &oldSession.active, sizeof(owned)) == 0);
 Complete(oldSession, 3);
 // A synchronous completion can publish before USB submit returns.
 newSession.Initialize(); newSession.RequestRumble(1, 1);
 assert(newSession.Prepare(0, true) && newSession.RecordCompletion(0));
 assert(!newSession.RecordCompletion(0));
 assert(!newSession.Prepare(0, true));
 uint32_t callback; int32_t status;
 assert(newSession.Finish(0, &callback, &status));
 assert(newSession.unexpectedCompletions == 1 && newSession.completed == 1);
 newSession.Initialize(); newSession.RequestRumble(10, 10);
 assert(newSession.Prepare(0, true));
 newSession.Stop();
 newSession.AbortUnsubmitted();
 assert(!newSession.activeValid && newSession.submitted == 0);
}

static void TestStartupAndMultiControllerStress() {
 XboxInputOutputQueue players[4];
 for (unsigned p = 0; p < 4; ++p) players[p].Initialize();
 // Exercise startup priority even when XAM produces motors early.
 uint8_t startup[] = {5,32,1,1,0};
 for (unsigned p = 0; p < 4; ++p) {
  players[p].RequestRumble(100, 100);
  assert(players[p].Enqueue(startup, sizeof(startup), 1));
  assert(players[p].Prepare(0, false) && !players[p].active.rumble);
  Complete(players[p], 0, 0, 1);
  const uint8_t ack[4] = {1,32,2,0};
  assert(players[p].Enqueue(ack, sizeof(ack), 0));
  assert(players[p].Prepare(1, false));
  Complete(players[p], 1);
  assert(!players[p].Prepare(2, false));
  players[p].EstablishStoppedRumble(2);
 }
 // Independent producer bursts and delayed, out-of-order player completions.
 for (uint32_t tick = 3; tick < 200003; ++tick) {
  for (unsigned p = 0; p < 4; ++p) {
   XboxInputOutputQueue& q = players[p];
   if (tick % (3 + p) == 0)
    q.RequestRumble((uint8_t)(tick % 101), (uint8_t)((tick / 7 + p) % 101));
   if (q.activeValid) {
    XboxInputOutputPacket owned = q.active;
    assert(!q.Prepare(tick, true));
    assert(memcmp(&owned, &q.active, sizeof(owned)) == 0);
    if (tick % (7 + p) == 0) Complete(q, tick);
   }
   else q.Prepare(tick, true);
   assert(q.submitted == q.completed + (q.activeValid ? 1u : 0u));
   assert(q.count <= XboxInputOutputQueue::CAPACITY);
  }
 }
 for (unsigned p = 0; p < 4; ++p) {
  XboxInputOutputQueue& q = players[p];
	q.RequestRumble(0,0);
	if (q.activeValid) Complete(q, 200003);
	if (!q.rumbleCompletedValid || q.rumbleCompleted != 0) {
		assert(q.Prepare(200103, true) && q.active.motors == 0);
		Complete(q, 200104);
	}
	assert(q.rumbleCompleted == 0 && q.submitted == q.completed &&
		!q.Prepare(200130, true));
 }
}

int main() {
 TestCopyFifoAndOwnership();
 TestFinalStopAndDelay();
 TestTickWrapAndSequence();
 TestFailurePolicy();
 TestRetirementOverflowAndCompletions();
 TestStartupAndMultiControllerStress();
 puts("PASS: output ownership, FIFO, final-stop, timing/wrap, failures, retirement, overflow, synchronous/duplicate completions, four-player stress");
 return 0;
}
