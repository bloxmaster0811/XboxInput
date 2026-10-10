// Execute the actual production transport against a deterministic fake kernel.
// This tests lifecycle/diagnostic behaviour, NOT Xbox USB hardware or PPC races.
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <setjmp.h>
#include "../source/gip_output_queue.h"
#include "../source/gip_output_trace.h"
#include "../source/gip_output_policy.h"
#include "../source/gip_input_readiness.h"

#pragma GCC diagnostic ignored "-Wunused-function" // Unrelated static startup wrappers.
typedef uint32_t DWORD; // This test is compiled as 32-bit, matching retail pointers.
typedef uint8_t BYTE;
typedef int32_t LONG;
typedef void* PVOID;
#define WINAPI
#define __sync() __sync_synchronize()
#define GIP_MAX_SESSIONS 32
enum { XBOXINPUT_LOG_OUTPUT = 13, XBOXINPUT_LOG_OUTPUT_TRACE = 14 };
struct deviceHandle { int unused; };
struct UsbTrb { BYTE* buffer; DWORD length, flags, callback, endpoint, savedEndpoint; };
enum { XBOXINPUT_QUIRK_RUMBLE_SETUP = 1 };
struct Profile { DWORD quirks; };
struct Runtime { BYTE playerIndex; bool stopping, ready; Profile* profile; };
struct Extension { deviceHandle* deviceHandle; };
struct GipSessionSlot {
 bool reserved; Runtime runtime; Extension ext; UsbTrb outTrb;
 bool outOpen; DWORD outputLock; XboxInputOutputQueue output;
 DWORD outputActiveTick, outputSummaryTick, outputLastCompleted;
 bool outputStallLogged;
 volatile LONG outputCompletionPending, outputCompletionStatus;
 volatile LONG outputTraceTransfer, outputTraceEntered, outputTraceReturned;
 volatile LONG outputTraceCallbacks, outputTraceConsumed, outputTraceInsideSubmit;
 volatile LONG outputTraceReturnTick, outputTraceToken, outputTraceWorkerTick;
 volatile LONG outputTraceCallbackTransfer, outputTraceCallbackTick;
 volatile LONG outputTraceCallbackStatus, outputTraceCallbackPreviousPending;
 DWORD outputTraceWatchdogTick; bool outputTraceWatchdogValid;
 DWORD outputTraceStopTick; bool outputTraceStopValid;
 DWORD runtimeRumbleRequests, runtimeRumbleNonzeroRequests;
 DWORD runtimeRumbleSampleTick, runtimeRumbleSampleRequests;
 bool runtimeRumbleSampleValid;
 XboxInputGipInputReadiness inputReadiness;
};
static GipSessionSlot g_gipSessions[GIP_MAX_SESSIONS];
static GipSessionSlot* g_gipPrimarySession = &g_gipSessions[0];
#define g_gipExt (g_gipPrimarySession->ext)
static deviceHandle g_handle;
static uint32_t g_tick;
static unsigned g_lockDepth, g_steps, g_limit, g_usbCalls;
static unsigned g_wireDigest;
static unsigned g_runtimeWireCalls, g_nonzeroRuntimeWireCalls;
static bool g_delayedPending, g_preemptPublish;
typedef void (*xboxinput_usb_dpc_routine_t)(PVOID, PVOID, PVOID, PVOID);
typedef int32_t (*xboxinput_wait_usb_dpc_t)(xboxinput_usb_dpc_routine_t, PVOID, DWORD, PVOID, PVOID);
static int32_t FakeCallAndWaitForDpcRoutine(xboxinput_usb_dpc_routine_t, PVOID, DWORD, PVOID, PVOID);
static xboxinput_wait_usb_dpc_t KeCallAndWaitForDpcRoutinePtr = FakeCallAndWaitForDpcRoutine;
static unsigned g_processor = 4, g_irql, g_handoffCalls;
static bool g_inUsbDpc, g_removeBeforeDpc, g_failAfterDpc;
static int32_t g_handoffFailure;
static jmp_buf g_workerReturn;
enum UsbMode { SYNC, DELAYED, LOST, BLOCKED, SYNC_THEN_BLOCK };
static UsbMode g_mode;
struct Event { DWORD type, tag, transfer, a, b, c; };
static Event g_events[32768];
static unsigned g_eventCount;

static DWORD GetTickCount() { return g_tick; }
static BYTE KfAcquireSpinLock(DWORD*) { assert(g_lockDepth == 0); ++g_lockDepth; return 0; }
static void KfReleaseSpinLock(DWORD*, BYTE) { assert(g_lockDepth == 1); --g_lockDepth; }
static LONG InterlockedExchange(volatile LONG* p, LONG value) { LONG old = *p; *p = value; return old; }
static LONG InterlockedIncrement(volatile LONG* p) { return ++*p; }
static LONG InterlockedCompareExchange(volatile LONG* p, LONG value, LONG compare) {
 LONG old = *p;
 if (old == compare) *p = value;
 if (g_preemptPublish && p == &g_gipSessions[0].outputCompletionPending && value == 1) {
  // Simulate another CPU consuming the mailbox before the callback returns.
  g_preemptPublish = false;
  GipSessionSlot& s = g_gipSessions[0];
  s.outputCompletionPending = 0;
  assert(s.output.RecordCompletion(0));
  uint32_t callback = 0; int32_t status = 0;
  assert(s.output.Finish(0, &callback, &status));
  s.output.RequestRumble(10, 20);
  assert(s.output.Prepare(200, true));
  s.outputTraceTransfer = 99;
 }
 return old;
}
static bool XboxInputRuntimeIsReady(const Runtime* r) { return r->ready; }
static void XboxInputQueueLogEventEx(DWORD type, DWORD tag, DWORD transfer, DWORD a, DWORD b, DWORD c) {
 assert(g_lockDepth == 0); // Neither callback nor worker logs under its output lock.
 assert(g_eventCount < sizeof(g_events) / sizeof(g_events[0]));
 Event& e = g_events[g_eventCount++];
 e.type = type; e.tag = tag; e.transfer = transfer; e.a = a; e.b = b; e.c = c;
}
static void Sleep(DWORD);
static int32_t UsbdQueueAsyncTransfer(deviceHandle*, UsbTrb*);

#include "../source/gip_output_transport.inl"

static int32_t FakeCallAndWaitForDpcRoutine(xboxinput_usb_dpc_routine_t routine,
 PVOID context, DWORD processor, PVOID arg1, PVOID arg2) {
 assert(g_lockDepth == 0 && !g_inUsbDpc && g_processor == 4 && g_irql == 0);
 assert(processor == 2 && arg1 == NULL && arg2 == NULL);
 ++g_handoffCalls;
 if (g_handoffFailure < 0) return g_handoffFailure;
 g_inUsbDpc = true; g_processor = processor; g_irql = 2;
 if (g_removeBeforeDpc) {
  GipSessionSlot* s = (GipSessionSlot*)context;
  GipStopOutput(s); s->runtime.stopping = true; s->ext.deviceHandle = NULL;
  g_removeBeforeDpc = false;
 }
 routine(NULL, context, arg1, arg2);
 g_inUsbDpc = false; g_processor = 4; g_irql = 0;
 return g_failAfterDpc ? -123 : 0;
}

static int32_t UsbdQueueAsyncTransfer(deviceHandle* handle, UsbTrb* trb) {
 assert(g_lockDepth == 0 && handle == &g_handle);
#if XBOXINPUT_USB_DPC_SUBMIT
 assert(g_inUsbDpc && g_processor == 2 && g_irql == 2);
#else
 assert(!g_inUsbDpc && g_processor == 4 && g_irql == 0);
#endif
 GipSessionSlot* owner = NULL;
 for (unsigned i = 0; i < GIP_MAX_SESSIONS; ++i)
  if (trb == &g_gipSessions[i].outTrb) owner = &g_gipSessions[i];
 assert(owner && owner->output.activeValid && trb->buffer == owner->output.active.bytes);
 ++g_usbCalls;
 // Deterministic fixture fingerprint includes bytes and submission time.
 for (unsigned i = 0; i < trb->length; ++i)
  g_wireDigest = (g_wireDigest ^ trb->buffer[i]) * 16777619u;
 for (unsigned i = 0; i < 4; ++i)
  g_wireDigest = (g_wireDigest ^ ((g_tick >> (i * 8)) & 255)) * 16777619u;
 if (trb->buffer[0] == 9) {
  assert(trb->length == 13 && trb->buffer[1] == 0 && trb->buffer[3] == 9);
  assert(trb->buffer[4] == 0 && trb->buffer[5] == 15);
  assert(trb->buffer[6] == 0 && trb->buffer[7] == 0);
  assert(trb->buffer[10] == 255 && trb->buffer[11] == 0 && trb->buffer[12] == 0xEB);
  if (owner->output.active.rumble) {
   ++g_runtimeWireCalls;
   if (trb->buffer[6] || trb->buffer[7] || trb->buffer[8] || trb->buffer[9])
    ++g_nonzeroRuntimeWireCalls;
#if XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE
   assert(trb->buffer[8] == 0 && trb->buffer[9] == 0);
#else
   assert(trb->buffer[8] == owner->output.active.motors >> 8);
   assert(trb->buffer[9] == (owner->output.active.motors & 255));
#endif
  }
 }
 if (g_mode == SYNC || g_mode == SYNC_THEN_BLOCK)
  GipOutputComplete((DWORD)trb, 0);
 if (g_mode == BLOCKED || g_mode == SYNC_THEN_BLOCK) longjmp(g_workerReturn, 1);
 if (g_mode == DELAYED) g_delayedPending = true;
 return 0x123456; // Deliberately opaque, not a completion status.
}
static void Sleep(DWORD ms) {
 assert(g_lockDepth == 0 && !g_inUsbDpc && g_irql == 0);
 g_tick += (uint32_t)ms;
 if (g_delayedPending) {
  g_delayedPending = false;
  GipOutputComplete((DWORD)&g_gipSessions[0].outTrb, 0);
 }
 if (++g_steps >= g_limit) longjmp(g_workerReturn, 1);
}
static void RunWorker(unsigned steps) {
 g_steps = 0; g_limit = steps;
 if (setjmp(g_workerReturn) == 0) GipOutputWorker(NULL);
 assert(g_lockDepth == 0);
 // A blocked fake USB call escapes with longjmp rather than returning through
 // the fake handoff. Restore test execution context, NOT transfer ownership.
 g_inUsbDpc = false; g_processor = 4; g_irql = 0;
}
static GipSessionSlot& Reset(UsbMode mode) {
 memset(g_gipSessions, 0, sizeof(g_gipSessions));
 g_tick = 0; g_lockDepth = 0; g_usbCalls = 0; g_eventCount = 0;
 g_wireDigest = 2166136261u;
 g_runtimeWireCalls = 0; g_nonzeroRuntimeWireCalls = 0;
 g_delayedPending = false; g_preemptPublish = false; g_mode = mode;
 g_inUsbDpc = false; g_processor = 4; g_irql = 0; g_handoffCalls = 0;
 g_removeBeforeDpc = false; g_failAfterDpc = false; g_handoffFailure = 0;
 KeCallAndWaitForDpcRoutinePtr = FakeCallAndWaitForDpcRoutine;
 g_outputTraceUnmatchedCallbacks = 0;
 GipSessionSlot& s = g_gipSessions[0];
 s.reserved = true; s.runtime.ready = true; s.runtime.playerIndex = 0;
 s.ext.deviceHandle = &g_handle; s.outOpen = true; s.outTrb.endpoint = 2;
 s.output.Initialize();
 return s;
}
static const Event& Last(unsigned phase) {
 for (unsigned i = g_eventCount; i > 0; --i)
  if (g_events[i - 1].type == XBOXINPUT_LOG_OUTPUT_TRACE &&
      g_events[i - 1].tag >> 24 == phase) return g_events[i - 1];
 assert(false); return g_events[0];
}
static unsigned Count(unsigned phase) {
 unsigned n = 0;
 for (unsigned i = 0; i < g_eventCount; ++i)
  if (g_events[i].type == XBOXINPUT_LOG_OUTPUT_TRACE && g_events[i].tag >> 24 == phase) ++n;
 return n;
}
static void TestNormalCompletion(UsbMode mode) {
 GipSessionSlot& s = Reset(mode);
 assert(GipSessionSendRumble(&s, 100, 50) == 0);
 RunWorker(4);
 assert(g_usbCalls == 1 && s.output.completed == 1 && !s.output.activeValid);
 assert(s.outputTraceEntered == 1 && s.outputTraceReturned == 1 &&
        s.outputTraceCallbacks == 1 && s.outputTraceConsumed == 1);
 assert(Count(XBOXINPUT_TRACE_ENTER) == 1 && Count(XBOXINPUT_TRACE_RETURN) == 1);
 assert(Count(XBOXINPUT_TRACE_CALLBACK) == 1 && Count(XBOXINPUT_TRACE_CONSUMED) == 1);
 assert(Last(XBOXINPUT_TRACE_RETURN).a == 0x123456);
 assert(Last(XBOXINPUT_TRACE_CALLBACK).transfer == 1 && Last(XBOXINPUT_TRACE_CONSUMED).b == 1);
 assert(s.output.rumbleCompleted == 0x6432); // Logical request retained, not physical motor state.
#if XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE
 assert((Last(XBOXINPUT_TRACE_ENTER).c & 65535) == 0);
#endif
 if (mode == SYNC) assert(Last(XBOXINPUT_TRACE_RETURN).c == 1);
 if (mode == DELAYED) assert(Last(XBOXINPUT_TRACE_RETURN).c == 0);
 assert(GipSessionSendRumble(&s, 0, 0) == 0);
 RunWorker(30);
 assert(g_usbCalls == 2 && s.output.completed == 2 && s.output.rumbleCompleted == 0);
}
static void TestMissingCompletionAndStop() {
 GipSessionSlot& s = Reset(LOST);
 assert(GipSessionSendRumble(&s, 100, 50) == 0);
 RunWorker(260);
 XboxInputOutputPacket original = s.output.active;
 assert(s.output.submitted == 1 && s.output.completed == 0 && s.output.activeValid);
 XboxInputOutputQueue queueBeforeSample = s.output;
 GipLogOutputDiagnostics(g_tick);
 assert(memcmp(&queueBeforeSample, &s.output, sizeof(s.output)) == 0);
 assert(Last(XBOXINPUT_TRACE_COUNTS).a == 1 && Last(XBOXINPUT_TRACE_COUNTS).b == 1 &&
        Last(XBOXINPUT_TRACE_COUNTS).c == 0);
 assert(Last(XBOXINPUT_TRACE_WATCHDOG).a >= 1000);
 assert(GipSessionSendRumble(&s, 0, 0) == 0);
 assert(Count(XBOXINPUT_TRACE_STOP) == 1);
 RunWorker(300);
 assert(g_usbCalls == 1 && s.output.rumbleDesired == 0); // No unsafe retry/reuse.
 assert(memcmp(&original, &s.output.active, sizeof(original)) == 0);
 GipLogOutputDiagnostics(g_tick);
 assert(Count(XBOXINPUT_TRACE_WATCHDOG) == 2);
 assert(Last(XBOXINPUT_TRACE_MOTORS).a == 0 && Last(XBOXINPUT_TRACE_MOTORS).b == 0x6432);
 // A late callback completes the same owned transfer and permits the final stop.
 g_mode = SYNC; GipOutputComplete((DWORD)&s.outTrb, 0);
 RunWorker(40);
 assert(g_usbCalls == 2 && s.output.completed == 2 && s.output.rumbleCompleted == 0);
}
static void TestBlockedSubmission(UsbMode mode) {
 GipSessionSlot& s = Reset(mode);
 assert(GipSessionSendRumble(&s, 50, 50) == 0);
 RunWorker(2);
 g_tick = 2000; GipLogOutputDiagnostics(g_tick); // Logger independent of blocked worker.
 assert(Last(XBOXINPUT_TRACE_COUNTS).a == 1 && Last(XBOXINPUT_TRACE_COUNTS).b == 0);
 assert(Last(XBOXINPUT_TRACE_WATCHDOG).c & XBOXINPUT_TRACE_INSIDE_SUBMIT);
 assert(Count(XBOXINPUT_TRACE_RETURN) == 0);
 assert(s.output.activeValid);
 if (mode == SYNC_THEN_BLOCK) {
  assert(Last(XBOXINPUT_TRACE_MAILBOX).b == 1 && Last(XBOXINPUT_TRACE_MAILBOX).c == 0);
  assert(Last(XBOXINPUT_TRACE_COUNTS).c == 1);
 }
}
static void TestDuplicateAndUnmatched() {
 GipSessionSlot& s = Reset(LOST);
 assert(GipSessionSendRumble(&s, 50, 50) == 0); RunWorker(1);
 GipOutputComplete((DWORD)&s.outTrb, 0);
 GipOutputComplete((DWORD)&s.outTrb, 0);
 assert(Last(XBOXINPUT_TRACE_CALLBACK).b == 1); // Duplicate publication visible.
 RunWorker(2);
 assert(s.outputTraceCallbacks == 2 && s.outputTraceConsumed == 1 && s.output.completed == 1);
 GipOutputComplete(0xDEAD, -123);
 assert(Last(XBOXINPUT_TRACE_UNMATCHED).a == 0xDEAD &&
        Last(XBOXINPUT_TRACE_UNMATCHED).c == 1);
}
static void TestCallbackIdentityBeforePublish() {
 GipSessionSlot& s = Reset(LOST);
 const BYTE power[] = {5,32,1,1,0};
 assert(GipEnqueueRaw(&s, power, sizeof(power), 0) == 0); RunWorker(1);
 assert(s.output.active.bytes[0] == 5 && s.outputTraceTransfer == 1);
 g_preemptPublish = true;
 GipOutputComplete((DWORD)&s.outTrb, 0);
 assert(s.outputTraceTransfer == 99 && s.output.active.bytes[0] == 9);
 assert(Last(XBOXINPUT_TRACE_CALLBACK).transfer == 1 &&
        (Last(XBOXINPUT_TRACE_CALLBACK).tag & 255) == 5);
}
static void SetupAdditional(unsigned index) {
 GipSessionSlot& s = g_gipSessions[index];
 s.reserved = true; s.runtime.ready = true; s.runtime.playerIndex = (BYTE)index;
 s.ext.deviceHandle = &g_handle; s.outOpen = true; s.outTrb.endpoint = 2;
 s.output.Initialize();
}
static void TestRetirementAndIndependentSession() {
 GipSessionSlot& old = Reset(LOST);
 assert(GipSessionSendRumble(&old, 100, 50) == 0); RunWorker(1);
 XboxInputOutputPacket owned = old.output.active;
 GipStopOutput(&old);
 assert(!old.outOpen && old.output.stopped && old.output.activeValid);
 g_tick = 6000; GipLogOutputDiagnostics(g_tick);
 assert(Last(XBOXINPUT_TRACE_WATCHDOG).c & XBOXINPUT_TRACE_QUEUE_STOPPED);
 assert(!(Last(XBOXINPUT_TRACE_WATCHDOG).c & XBOXINPUT_TRACE_OUT_OPEN));
 SetupAdditional(1);
 GipSessionSlot& current = g_gipSessions[1];
 g_mode = SYNC; assert(GipSessionSendRumble(&current, 10, 20) == 0);
 RunWorker(4);
 assert(g_usbCalls == 2 && current.output.completed == 1 && old.output.completed == 0);
 assert(memcmp(&owned, &old.output.active, sizeof(owned)) == 0);
 assert(((Last(XBOXINPUT_TRACE_CALLBACK).tag >> 16) & 255) == 1);
 GipOutputComplete((DWORD)&old.outTrb, 0); RunWorker(4);
 assert(old.output.completed == 1 && !old.output.activeValid && g_usbCalls == 2);
 assert(current.output.completed == 1 && current.outputTraceCallbacks == 1);
 assert(GipSessionSendRumble(&old, 0, 0) == -1);
}
static void TestFourPlayers() {
 Reset(SYNC);
 for (unsigned i = 0; i < 4; ++i) {
  if (i) SetupAdditional(i);
  assert(GipSessionSendRumble(&g_gipSessions[i], (BYTE)(20 + i), (BYTE)(30 + i)) == 0);
 }
 RunWorker(4);
 assert(g_usbCalls == 4 && Count(XBOXINPUT_TRACE_CALLBACK) == 4);
 for (unsigned i = 0; i < 4; ++i) {
  const GipSessionSlot& s = g_gipSessions[i];
  assert(s.output.completed == 1 && s.outputTraceCallbacks == 1 && s.outputTraceConsumed == 1);
 }
}
static void TestWatchdogRollover() {
 GipSessionSlot& s = Reset(LOST);
 g_tick = 0xFFFFFFF0u;
 assert(GipSessionSendRumble(&s, 50, 50) == 0); RunWorker(1);
 GipLogOutputDiagnostics(g_tick);
 assert(Last(XBOXINPUT_TRACE_WATCHDOG).a == 5);
 g_tick = 0x400;
 GipLogOutputDiagnostics(g_tick);
 assert(Count(XBOXINPUT_TRACE_WATCHDOG) == 2 && Last(XBOXINPUT_TRACE_WATCHDOG).a == 0x400u + 16u);
}
static void TestSuppressedRuntime(unsigned players) {
 Reset(SYNC);
 for (unsigned i = 1; i < players; ++i) SetupAdditional(i);
 const BYTE power[] = {5,32,1,1,0};
 const BYTE setup[] = {9,0,2,9,0,15,0,0,0,0,255,0,0xEB};
 Profile profile = { XBOXINPUT_QUIRK_RUMBLE_SETUP };
 for (unsigned i = 0; i < players; ++i) {
  GipSessionSlot& s = g_gipSessions[i];
  s.runtime.profile = &profile;
  GipBeginStartup(&s);
  assert(!GipSessionCanRegisterFromInput(&s));
  assert(GipEnqueueRaw(&s, power, sizeof(power), 0) == 0);
  assert(GipEnqueueRaw(&s, setup, sizeof(setup), 0) == 0);
 }
 RunWorker(6);
 assert(g_usbCalls == players * 2); // Power and ordered startup 09 survive.
 for (unsigned i = 0; i < players; ++i) {
  GipEstablishStoppedRumble(&g_gipSessions[i]);
  assert(GipSessionCanRegisterFromInput(&g_gipSessions[i]));
 }
 for (unsigned n = 0; n < 10000; ++n) {
  for (unsigned i = 0; i < players; ++i)
   assert(GipSessionSendRumble(&g_gipSessions[i], (BYTE)(1 + n % 100), (BYTE)i) == 0);
  RunWorker(1);
  if (n % 100 == 0) GipLogOutputDiagnostics(g_tick);
 }
 for (unsigned i = 0; i < players; ++i)
  assert(GipSessionSendRumble(&g_gipSessions[i], 0, 0) == 0);
 RunWorker(1000);
 GipLogOutputDiagnostics(g_tick);
 assert(g_usbCalls == players * 2); // No runtime rumble, even final stop.
 assert(Last(XBOXINPUT_TRACE_SUPPRESSED).a == 10001);
 assert(Last(XBOXINPUT_TRACE_SUPPRESSED).b == 10000);
 assert(Last(XBOXINPUT_TRACE_SUPPRESSED).c == 0);
 unsigned samples = Count(XBOXINPUT_TRACE_SUPPRESSED);
 GipLogOutputDiagnostics(g_tick + 1);
 assert(Count(XBOXINPUT_TRACE_SUPPRESSED) == samples); // Rate-limited.
 for (unsigned i = 0; i < players; ++i) {
  GipSessionSlot& s = g_gipSessions[i];
  assert(s.output.submitted == 2 && s.output.completed == 2 && !s.output.activeValid);
  assert(s.output.rumbleDesiredValid && s.output.rumbleDesired == 0);
  assert(s.runtimeRumbleRequests == 10001 && s.runtimeRumbleNonzeroRequests == 10000);
  GipStopOutput(&s);
  assert(GipSessionSendRumble(&s, 10, 20) == -1);
  assert(s.runtimeRumbleRequests == 10001); // Rejected calls not counted.
 }
 SetupAdditional(players);
 assert(GipSessionSendRumble(&g_gipSessions[players], 90, 90) == 0);
 RunWorker(200);
 assert(g_usbCalls == players * 2); // New session uses same suppression policy.
}
static void TestSuppressedWithoutStartupOutput() {
 GipSessionSlot& s = Reset(LOST);
 g_tick = 0xFFFFFFF0u;
 assert(GipSessionSendRumble(&s, 40, 30) == 0);
 RunWorker(2); GipLogOutputDiagnostics(g_tick);
 assert(g_usbCalls == 0 && Count(XBOXINPUT_TRACE_SUPPRESSED) == 1);
 g_tick += 5000;
 GipLogOutputDiagnostics(g_tick);
 assert(Count(XBOXINPUT_TRACE_SUPPRESSED) == 2);
 assert(s.outputTraceEntered == 0 && s.output.completed == 0);
}
static void TestWirePolicyOnlyChangesStrengths() {
 XboxInputOutputQueue q; q.Initialize();
 q.RequestRumble(91, 47); assert(q.Prepare(0, true));
 XboxInputOutputPacket expected = q.active;
#if XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE
 expected.bytes[6] = expected.bytes[7] = expected.bytes[8] = expected.bytes[9] = 0;
#endif
 XboxInputApplyRuntimeOutputPolicy(&q.active);
 assert(memcmp(&expected, &q.active, sizeof(expected)) == 0);
 // Startup command 09 may deliberately contain nonzero motors. It is ordered,
 // not runtime, and must remain byte-for-byte unchanged.
 XboxInputOutputPacket startup = expected;
 startup.rumble = false; startup.bytes[8] = 18; startup.bytes[9] = 52;
 expected = startup; XboxInputApplyRuntimeOutputPolicy(&startup);
 assert(memcmp(&expected, &startup, sizeof(expected)) == 0);
 GipSessionSlot& s = Reset(SYNC);
 assert(GipEnqueueRaw(&s, startup.bytes, startup.length, 0) == 0);
 RunWorker(4);
 assert(g_usbCalls == 1 && s.output.completed == 1);
 assert(s.output.active.bytes[8] == 18 && s.output.active.bytes[9] == 52);
}
static void TestRuntimeScheduling() {
 GipSessionSlot& s = Reset(SYNC);
 assert(GipSessionSendRumble(&s, 70, 30) == 0); RunWorker(4);
 assert(g_usbCalls == 1 && s.output.rumbleCompleted == 0x461E);
 for (unsigned i = 0; i < 1000; ++i) {
  assert(GipSessionSendRumble(&s, 70, 30) == 0); RunWorker(1);
 }
 assert(g_usbCalls == 1); // Repeated requests do not destroy deduplication.
 assert(GipSessionSendRumble(&s, 71, 31) == 0); RunWorker(4);
 assert(g_usbCalls == 2 && s.output.rumbleCompleted == 0x471F);
 for (unsigned i = 0; i < 200; ++i) {
  assert(GipSessionSendRumble(&s, (BYTE)(i % 100), 40) == 0); RunWorker(1);
 }
 assert(g_usbCalls >= 10 && g_usbCalls <= 12); // Existing 100ms cadence, no zero-packet flood.
 const unsigned beforeStop = g_usbCalls;
 assert(GipSessionSendRumble(&s, 0, 0) == 0); RunWorker(30);
 assert(g_usbCalls == beforeStop + 1 && s.output.rumbleCompleted == 0);
 GipLogOutputDiagnostics(g_tick);
 assert(Last(XBOXINPUT_TRACE_SUPPRESSED).b > 1000);
 assert(Last(XBOXINPUT_TRACE_SUPPRESSED).c == 0);
}
static void TestRealWireMotorStrengths() {
#if !XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE && !XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE
 const BYTE cases[][2] = {{0,0}, {1,0}, {0,1}, {100,0}, {0,100},
  {100,100}, {255,255}, {91,47}};
 for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
  GipSessionSlot& s = Reset(SYNC);
  assert(GipSessionSendRumble(&s, cases[i][0], cases[i][1]) == 0);
  RunWorker(4);
  const BYTE left = cases[i][0] > 100 ? 100 : cases[i][0];
  const BYTE right = cases[i][1] > 100 ? 100 : cases[i][1];
  assert(g_usbCalls == 1 && s.output.completed == 1);
  assert(s.output.active.bytes[6] == 0 && s.output.active.bytes[7] == 0);
  assert(s.output.active.bytes[8] == left && s.output.active.bytes[9] == right);
  assert(s.output.rumbleCompleted == (left << 8 | right));
  assert(g_runtimeWireCalls == 1 && g_nonzeroRuntimeWireCalls == (unsigned)(left || right));
  assert(GipSessionSendRumble(&s, 0, 0) == 0); RunWorker(30);
  assert(s.output.rumbleCompleted == 0);
  assert(s.output.active.bytes[8] == 0 && s.output.active.bytes[9] == 0);
  assert(g_usbCalls == (unsigned)(left || right ? 2 : 1));
  assert(s.runtimeRumbleRequests == 2);
 }
 puts("PASS: actual left/right wire strengths, zero requests, 100-percent/clamped values, explicit stop and preserved request counters");
#endif
}
static void TestComparableWireTraffic() {
 Reset(SYNC);
 const BYTE power[] = {5,32,1,1,0};
 for (unsigned i = 0; i < 4; ++i) {
  if (i) SetupAdditional(i);
  assert(GipEnqueueRaw(&g_gipSessions[i], power, sizeof(power), 0) == 0);
 }
 RunWorker(4);
 for (unsigned n = 0; n < 4000; ++n) {
  for (unsigned i = 0; i < 4; ++i)
   assert(GipSessionSendRumble(&g_gipSessions[i], (BYTE)((n * 7 + i) % 101),
    (BYTE)((n * 11 + i) % 101)) == 0);
  RunWorker(1);
  if (n % 100 == 0) GipLogOutputDiagnostics(g_tick);
 }
 for (unsigned i = 0; i < 4; ++i)
  assert(GipSessionSendRumble(&g_gipSessions[i], 0, 0) == 0);
 RunWorker(40);
 unsigned completed = 0;
 for (unsigned i = 0; i < 4; ++i) {
  const GipSessionSlot& s = g_gipSessions[i];
  assert(!s.output.activeValid);
  assert(s.output.completed == (DWORD)s.outputTraceEntered);
  assert(s.outputTraceEntered == s.outputTraceReturned);
  assert(s.outputTraceEntered == s.outputTraceCallbacks);
  assert(s.outputTraceEntered == s.outputTraceConsumed);
  completed += s.output.completed;
 }
 assert(completed == g_usbCalls);
#if XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE
 assert(g_runtimeWireCalls == 0 && g_nonzeroRuntimeWireCalls == 0);
#elif XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE
 assert(g_runtimeWireCalls > 0 && g_nonzeroRuntimeWireCalls == 0);
#else
 assert(g_runtimeWireCalls > 0 && g_nonzeroRuntimeWireCalls > 0);
 assert(g_nonzeroRuntimeWireCalls < g_runtimeWireCalls); // Final stops reach USB.
#endif
 printf("traffic_fixture calls=%u completed=%u hash=%08X endTick=%u\n",
  g_usbCalls, completed, g_wireDigest, g_tick);
#if !XBOXINPUT_DETAILED_OUTPUT_TRACE
 for (unsigned phase = XBOXINPUT_TRACE_ENTER; phase <= XBOXINPUT_TRACE_FINISHED; ++phase)
  assert(Count(phase) == 0);
 assert(Count(XBOXINPUT_TRACE_COUNTS) > 0);
 assert(Count(XBOXINPUT_TRACE_SUPPRESSED) > 0);
#endif
}
static void TestLowTraceCompletion(UsbMode mode) {
 GipSessionSlot& s = Reset(mode);
 assert(GipSessionSendRumble(&s, 100, 50) == 0); RunWorker(4);
 assert(g_usbCalls == 1 && s.output.completed == 1 && !s.output.activeValid);
 assert(s.outputTraceEntered == 1 && s.outputTraceReturned == 1 &&
  s.outputTraceCallbacks == 1 && s.outputTraceConsumed == 1);
 for (unsigned phase = XBOXINPUT_TRACE_ENTER; phase <= XBOXINPUT_TRACE_FINISHED; ++phase)
  assert(Count(phase) == 0);
 GipLogOutputDiagnostics(g_tick);
 assert(Last(XBOXINPUT_TRACE_COUNTS).a == 1 && Last(XBOXINPUT_TRACE_COUNTS).b == 1 &&
  Last(XBOXINPUT_TRACE_COUNTS).c == 1);
}
static void TestLowTraceLostAndBlocked() {
 GipSessionSlot& s = Reset(LOST);
 assert(GipSessionSendRumble(&s, 50, 50) == 0); RunWorker(260);
 XboxInputOutputPacket owned = s.output.active;
 GipLogOutputDiagnostics(g_tick);
 assert(s.output.activeValid && Last(XBOXINPUT_TRACE_WATCHDOG).a >= 1000);
 assert(Last(XBOXINPUT_TRACE_COUNTS).c == 0);
 assert(GipSessionSendRumble(&s, 0, 0) == 0); RunWorker(300);
 assert(g_usbCalls == 1 && s.output.completed == 0);
 assert(memcmp(&owned, &s.output.active, sizeof(owned)) == 0);
 assert(Count(XBOXINPUT_TRACE_STOP) == 1);
 GipStopOutput(&s); SetupAdditional(1); g_mode = SYNC;
 assert(GipSessionSendRumble(&g_gipSessions[1], 40, 30) == 0); RunWorker(4);
 assert(g_usbCalls == 2 && g_gipSessions[1].output.completed == 1);
 GipOutputComplete((DWORD)&s.outTrb, 0); RunWorker(4);
 assert(s.output.completed == 1 && !s.output.activeValid);
 for (unsigned mode = BLOCKED; mode <= SYNC_THEN_BLOCK; ++mode) {
  Reset((UsbMode)mode);
  assert(GipSessionSendRumble(&g_gipSessions[0], 30, 20) == 0); RunWorker(2);
  g_tick = 2000; GipLogOutputDiagnostics(g_tick);
  assert(Last(XBOXINPUT_TRACE_COUNTS).a == 1 && Last(XBOXINPUT_TRACE_COUNTS).b == 0);
  assert(Last(XBOXINPUT_TRACE_WATCHDOG).c & XBOXINPUT_TRACE_INSIDE_SUBMIT);
 }
}
static void TestLowTraceErrorsRetained() {
 GipSessionSlot& s = Reset(LOST);
 assert(GipSessionSendRumble(&s, 50, 50) == 0); RunWorker(1);
 GipOutputComplete((DWORD)&s.outTrb, -123); RunWorker(2);
 assert(Last(XBOXINPUT_TRACE_CALLBACK).a == (DWORD)-123);
 assert(Last(XBOXINPUT_TRACE_CONSUMED).a == (DWORD)-123);
 assert(Last(XBOXINPUT_TRACE_FINISHED).a == (DWORD)-123);
 Reset(LOST);
 assert(GipSessionSendRumble(&g_gipSessions[0], 50, 50) == 0); RunWorker(1);
 GipOutputComplete((DWORD)&g_gipSessions[0].outTrb, 0);
 assert(Count(XBOXINPUT_TRACE_CALLBACK) == 0);
 GipOutputComplete((DWORD)&g_gipSessions[0].outTrb, 0);
 assert(Last(XBOXINPUT_TRACE_CALLBACK).b == 1);
 GipOutputComplete(0xDEAD, -123);
 assert(Last(XBOXINPUT_TRACE_UNMATCHED).a == 0xDEAD);
}
static void TestDpcHandoffSafeguards() {
#if XBOXINPUT_USB_DPC_SUBMIT
 // No direct-worker fallback if a handoff cannot be made.
 for (unsigned missing = 0; missing < 2; ++missing) {
  GipSessionSlot& s = Reset(SYNC);
  if (missing) KeCallAndWaitForDpcRoutinePtr = NULL;
  else g_handoffFailure = -123;
  assert(GipSessionSendRumble(&s, 50, 50) == 0); RunWorker(4);
  assert(g_usbCalls == 0 && s.output.submitted == 0 && !s.output.activeValid);
  assert(s.output.stopped && !s.outOpen && s.runtime.ready);
  assert(Count(XBOXINPUT_TRACE_DPC_FAILURE) == 1);
  assert(Last(XBOXINPUT_TRACE_DPC_FAILURE).a == (DWORD)(missing ? -1 : -123));
  assert(GipSessionSendRumble(&s, 10, 20) == -1);
  RunWorker(20); assert(g_usbCalls == 0);
 }
 // Removal can run after worker preparation but before the DPC executes.
 GipSessionSlot& removed = Reset(SYNC);
 g_removeBeforeDpc = true;
 assert(GipSessionSendRumble(&removed, 50, 50) == 0); RunWorker(4);
 assert(g_handoffCalls == 1 && g_usbCalls == 0);
 assert(removed.output.submitted == 0 && !removed.output.activeValid);
 assert(removed.output.stopped && !removed.outOpen);
 // Defensive failure after USB entry must retain a kernel-owned buffer.
 GipSessionSlot& owned = Reset(LOST);
 g_failAfterDpc = true;
 assert(GipSessionSendRumble(&owned, 50, 50) == 0); RunWorker(4);
 assert(g_usbCalls == 1 && owned.output.submitted == 1 && owned.output.activeValid);
 XboxInputOutputPacket original = owned.output.active;
 assert(owned.output.stopped && !owned.outOpen && owned.runtime.ready);
 RunWorker(40);
 assert(g_usbCalls == 1 && memcmp(&original, &owned.output.active, sizeof(original)) == 0);
 GipOutputComplete((DWORD)&owned.outTrb, 0); RunWorker(4);
 assert(owned.output.completed == 1 && !owned.output.activeValid && g_usbCalls == 1);
 // Reconnected sessions remain independent; a failed session is not reused.
 g_failAfterDpc = false; g_mode = SYNC; SetupAdditional(1);
 assert(GipSessionSendRumble(&g_gipSessions[1], 20, 30) == 0); RunWorker(4);
 assert(g_usbCalls == 2 && g_gipSessions[1].output.completed == 1);
 puts("PASS: processor-2 DPC submit, unlocked passive handoff, no fallback, removal revalidation, late-completion ownership and independent reconnect");
#endif
}
int main() {
 TestComparableWireTraffic();
 TestWirePolicyOnlyChangesStrengths();
 TestRealWireMotorStrengths();
#if !XBOXINPUT_DETAILED_OUTPUT_TRACE
 TestLowTraceCompletion(SYNC); TestLowTraceCompletion(DELAYED);
 TestLowTraceLostAndBlocked(); TestLowTraceErrorsRetained();
 TestRuntimeScheduling();
 puts("PASS: low-trace actual transport, tested wire policy, bounded samples, retained errors, lost/blocked callbacks and retired/new sessions");
#else
#if XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE
 assert(!XboxInputAllowRuntimeRumble(true) && !XboxInputAllowRuntimeRumble(false));
 TestSuppressedRuntime(1); TestSuppressedRuntime(2); TestSuppressedRuntime(4);
 TestSuppressedWithoutStartupOutput();
 puts("PASS: runtime rumble suppressed under dense requests; ordered startup, readiness, reconnect, four players, bounded telemetry and rollover retained");
#else
 assert(XboxInputAllowRuntimeRumble(true) && !XboxInputAllowRuntimeRumble(false));
 TestNormalCompletion(SYNC); TestNormalCompletion(DELAYED);
 TestMissingCompletionAndStop(); TestBlockedSubmission(BLOCKED);
 TestBlockedSubmission(SYNC_THEN_BLOCK); TestDuplicateAndUnmatched();
 TestCallbackIdentityBeforePublish();
 TestRetirementAndIndependentSession(); TestFourPlayers(); TestWatchdogRollover();
 puts("PASS: actual transport with sync/delayed/lost/blocked/duplicate callbacks, pending stop, identity capture, watchdog, retirement, four players and rollover");
#if XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE
 TestRuntimeScheduling();
 puts("PASS: zero motor wire bytes, requested-state deduplication, unchanged cadence/startup payload and actual USB dispatch");
#endif
#endif
#endif
 TestDpcHandoffSafeguards();
 return 0;
}
