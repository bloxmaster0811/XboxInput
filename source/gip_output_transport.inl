// Included after GipSessionSlot: all GIP OUT producers feed this single owner.
// Never hold outputLock across allocation, logging or a stage continuation.
static volatile LONG g_outputTraceUnmatchedCallbacks = 0;
static void GipOutputTraceLog(GipSessionSlot* session, DWORD phase, DWORD command,
 DWORD transfer, DWORD a, DWORD b, DWORD c) {
 // Fixed-size event publication only: no file I/O, formatting, allocation,
 // waits, USB calls, or spinlocks, including when called by USB completion.
 if (!XboxInputPublishOutputTrace(phase, a, b, c)) return;
 XboxInputQueueLogEventEx(XBOXINPUT_LOG_OUTPUT_TRACE,
  (phase << 24) | ((DWORD)(session - g_gipSessions) << 16) |
  ((DWORD)session->runtime.playerIndex << 8) | (command & 255), transfer, a, b, c);
}
static void GipStopOutput(GipSessionSlot* session) {
 BYTE irql = KfAcquireSpinLock(&session->outputLock);
 session->output.Stop();
 session->outOpen = false;
 KfReleaseSpinLock(&session->outputLock, irql);
}
static void GipOutputLog(GipSessionSlot* session, DWORD phase, DWORD command,
 DWORD value, DWORD submitted, DWORD completed, DWORD state) {
 XboxInputQueueLogEventEx(XBOXINPUT_LOG_OUTPUT,
  (phase << 24) | ((DWORD)(session - g_gipSessions) << 16) |
  ((DWORD)session->runtime.playerIndex << 8) | (command & 0xFF),
  value, submitted, completed, state);
}
static int GipEnqueueRaw(GipSessionSlot* session, const BYTE* bytes,
 DWORD length, DWORD callback) {
 if (!session || !bytes) return -1;
 BYTE irql = KfAcquireSpinLock(&session->outputLock);
 bool accepted = session->outOpen && session->ext.deviceHandle &&
  session->output.Enqueue(bytes, length, callback);
 DWORD submitted = session->output.submitted, completed = session->output.completed;
 KfReleaseSpinLock(&session->outputLock, irql);
 if (!accepted) GipOutputLog(session, 5, length ? bytes[0] : 0,
  length, submitted, completed, 0);
 return accepted ? 0 : -1;
}
static int GipEnqueueCommand(GipSessionSlot* session, BYTE cmd, BYTE options,
 const BYTE* payload, int payloadLen, DWORD callback, bool echoSequence = false,
 BYTE sequence = 0) {
 if (!session || payloadLen < 0 || payloadLen > 60 ||
  (payloadLen && !payload)) return -1;
 BYTE bytes[64];
 BYTE irql = KfAcquireSpinLock(&session->outputLock);
 bytes[0] = cmd; bytes[1] = options;
 bytes[2] = echoSequence ? sequence : session->output.NextSequence();
 bytes[3] = (BYTE)payloadLen;
 if (payloadLen) memcpy(bytes + 4, payload, payloadLen);
 bool accepted = session->outOpen && session->ext.deviceHandle &&
  session->output.Enqueue(bytes, payloadLen + 4, callback);
 DWORD submitted = session->output.submitted, completed = session->output.completed;
 KfReleaseSpinLock(&session->outputLock, irql);
 if (!accepted) GipOutputLog(session, 5, cmd, payloadLen, submitted, completed, 0);
 return accepted ? 0 : -1;
}
static void GipEstablishStoppedRumble(GipSessionSlot* session) {
 const DWORD now = GetTickCount();
 BYTE irql = KfAcquireSpinLock(&session->outputLock);
 session->output.EstablishStoppedRumble(now);
 // Both early and post-IDENTIFY setup complete the required startup commands.
 // Keep this independent of whether the controller sent IDENTIFY at all.
 session->inputReadiness.CompleteStartup();
 KfReleaseSpinLock(&session->outputLock, irql);
}
static void GipBeginStartup(GipSessionSlot* session) {
 BYTE irql = KfAcquireSpinLock(&session->outputLock);
 session->inputReadiness.BeginStartup();
 KfReleaseSpinLock(&session->outputLock, irql);
}
static bool GipSessionCanRegisterFromInput(GipSessionSlot* session) {
 if (!session || !session->reserved || !session->runtime.profile) return false;
 BYTE irql = KfAcquireSpinLock(&session->outputLock);
 bool ready = session->outOpen && session->ext.deviceHandle &&
  session->inputReadiness.CanRegisterFromInput(
   (session->runtime.profile->quirks & XBOXINPUT_QUIRK_RUMBLE_SETUP) != 0,
   session->runtime.stopping, session->output.stopped);
 KfReleaseSpinLock(&session->outputLock, irql);
 return ready;
}
static int GipSessionSend(GipSessionSlot* session, uint8_t cmd, uint8_t options,
 const BYTE* payload, int payloadLen) {
 return GipEnqueueCommand(session, cmd, options, payload, payloadLen, 0);
}
static int GipSessionSendSeq(GipSessionSlot* session, uint8_t cmd, uint8_t options,
 uint8_t seq, const BYTE* payload, int payloadLen) {
 return GipEnqueueCommand(session, cmd, options, payload, payloadLen, 0, true, seq);
}
static int GipSendSeq(deviceHandle* h, uint8_t cmd, uint8_t options, uint8_t seq,
 const BYTE* payload, int payloadLen) {
 if (!h || h != g_gipExt.deviceHandle) return -1;
 return GipSessionSendSeq(g_gipPrimarySession, cmd, options, seq, payload, payloadLen);
}
static int GipSend(deviceHandle* h, uint8_t cmd, uint8_t options,
 const BYTE* payload, int payloadLen) {
 if (!h || h != g_gipExt.deviceHandle) return -1;
 return GipSessionSend(g_gipPrimarySession, cmd, options, payload, payloadLen);
}
static int GipSessionSendRumble(GipSessionSlot* session, BYTE left, BYTE right) {
 if (!session) return -1;
 const DWORD now = (!left && !right) ? GetTickCount() : 0;
 BYTE irql = KfAcquireSpinLock(&session->outputLock);
 bool accepted = session->outOpen && session->ext.deviceHandle &&
  !session->output.stopped && !session->runtime.stopping &&
  XboxInputRuntimeIsReady(&session->runtime) &&
  session->output.rumbleErrors < XboxInputOutputQueue::MAX_RUMBLE_ERRORS;
 const bool stopTrace = accepted && !left && !right && session->output.rumbleDesired != 0 &&
  session->output.activeValid && (DWORD)(now - session->outputActiveTick) >= 1000 &&
  (!session->outputTraceStopValid || (DWORD)(now - session->outputTraceStopTick) >= 1000);
 const DWORD transfer = session->outputTraceTransfer;
 const DWORD activeMotors = session->output.active.motors;
 const DWORD age = now - session->outputActiveTick;
 if (accepted) {
  session->output.RequestRumble(left, right);
  // Keep Test25 request telemetry even when actual motor strengths are restored.
  ++session->runtimeRumbleRequests;
  if (left || right) ++session->runtimeRumbleNonzeroRequests;
 }
 if (stopTrace) { session->outputTraceStopTick = now; session->outputTraceStopValid = true; }
 KfReleaseSpinLock(&session->outputLock, irql);
 if (stopTrace) GipOutputTraceLog(session, XBOXINPUT_TRACE_STOP, 9, transfer, 0, activeMotors, age);
 return accepted ? 0 : -1;
}
// Completion publishes to the original mailbox and bounded diagnostic records
// only. No locks or disk I/O. The dispatcher still exclusively owns queue state.
static int32_t GipOutputComplete(DWORD trbAddr, int32_t status) {
 for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
  GipSessionSlot* session = &g_gipSessions[i];
  if ((DWORD)&session->outTrb != trbAddr) continue;
  // Capture identity before publishing completion: the worker may immediately
  // finish this packet and prepare the next one after the mailbox CAS.
  const DWORD transfer = session->outputTraceTransfer;
  const DWORD command = session->output.active.bytes[0];
  session->outputCompletionStatus = status;
  __sync();
  const LONG before = InterlockedCompareExchange(&session->outputCompletionPending, 1, 0);
  session->outputTraceCallbackTransfer = transfer;
  session->outputTraceCallbackTick = GetTickCount();
  session->outputTraceCallbackStatus = status;
  session->outputTraceCallbackPreviousPending = before;
  __sync();
  const LONG callbacks = InterlockedIncrement(&session->outputTraceCallbacks);
  // Command belongs to the permanent active packet. No packet/TRB mutation.
  GipOutputTraceLog(session, XBOXINPUT_TRACE_CALLBACK, command,
   transfer, (DWORD)status, (DWORD)before, (DWORD)callbacks);
  return status;
 }
 // Never dereference an unrecognized address or change USB state to diagnose it.
 XboxInputQueueLogEventEx(XBOXINPUT_LOG_OUTPUT_TRACE,
  (XBOXINPUT_TRACE_UNMATCHED << 24) | 0x00FFFF00, 0, trbAddr, (DWORD)status,
  (DWORD)InterlockedIncrement(&g_outputTraceUnmatchedCallbacks));
 return status;
}
// Only this routine enters native OUT submission. Test25 dispatches it through
// ordinal 886 on processor 2; the worker waits with NO output lock held.
// Queue state is revalidated in the destination context, after any earlier
// removal callback. No packet/handle captured across the handoff is trusted.
static void GipSubmitOutputOnUsbDpc(PVOID, PVOID context, PVOID, PVOID) {
 GipSessionSlot* session = (GipSessionSlot*)context;
 BYTE irql = KfAcquireSpinLock(&session->outputLock);
 bool canSubmit = session->outOpen && session->ext.deviceHandle &&
  !session->runtime.stopping && session->output.activeValid &&
  !session->output.completionPending && !session->output.stopped;
 if (!canSubmit) {
  session->output.AbortUnsubmitted();
  KfReleaseSpinLock(&session->outputLock, irql);
  return;
 }
 deviceHandle* handle = session->ext.deviceHandle;
 const DWORD command = session->output.active.bytes[0];
 const DWORD submitted = session->output.submitted;
 const DWORD completed = session->output.completed;
 const DWORD state = session->output.count | ((DWORD)session->output.activeValid << 8) |
  ((DWORD)session->output.stopped << 9) | (session->output.rumbleErrors << 16);
 const bool submitLog = submitted <= 16;
 XboxInputApplyRuntimeOutputPolicy(&session->output.active);
 session->outTrb.buffer = session->output.active.bytes;
 session->outTrb.length = session->output.active.length;
 session->outTrb.flags = 1;
 session->outTrb.callback = (DWORD)GipOutputComplete;
 session->outTrb.savedEndpoint = session->outTrb.endpoint;
 InterlockedExchange(&session->outputTraceTransfer, submitted);
 InterlockedIncrement(&session->outputTraceEntered);
 InterlockedExchange(&session->outputTraceInsideSubmit, 1);
 const DWORD enterTick = GetTickCount();
 const DWORD packetMeta = (session->outTrb.length << 24) |
  ((DWORD)session->output.active.bytes[2] << 16) |
  (command == 9 && session->outTrb.length == 13 ?
   ((DWORD)session->output.active.bytes[8] << 8) | session->output.active.bytes[9] : 0);
 const DWORD endpoint = session->outTrb.endpoint;
 const DWORD buffer = (DWORD)session->outTrb.buffer;
 const DWORD callbackAddress = session->outTrb.callback;
 const DWORD flags = session->outTrb.flags;
 KfReleaseSpinLock(&session->outputLock, irql);
 GipOutputTraceLog(session, XBOXINPUT_TRACE_ENTER, command, submitted,
  (DWORD)&session->outTrb, endpoint, packetMeta);
 GipOutputTraceLog(session, XBOXINPUT_TRACE_BUFFER, command, submitted,
  buffer, callbackAddress, flags);
 // No plugin spinlock held across USB entry. Do not add sleeps, allocations,
 // file I/O, stage continuations or a wait for USB completion to this routine.
 int32_t token = UsbdQueueAsyncTransfer(handle, &session->outTrb);
 const DWORD returnTick = GetTickCount();
 session->outputTraceToken = token;
 session->outputTraceReturnTick = returnTick;
 InterlockedIncrement(&session->outputTraceReturned);
 InterlockedExchange(&session->outputTraceInsideSubmit, 0);
 GipOutputTraceLog(session, XBOXINPUT_TRACE_RETURN, command, submitted,
  (DWORD)token, returnTick - enterTick, session->outputCompletionPending);
 // Opaque token, not completion status. Permanent storage remains owned until
 // the completion mailbox is consumed, including synchronous completion.
 if (submitLog) GipOutputLog(session, 6, command, token, submitted, completed, state);
}
static DWORD WINAPI GipOutputWorker(PVOID) {
 for (;;) {
  for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
   GipSessionSlot* session = &g_gipSessions[i];
   if (!session->reserved) continue;
   DWORD now = GetTickCount();
   session->outputTraceWorkerTick = now;
   uint32_t callback = 0;
   int32_t status = 0;
   BYTE irql = KfAcquireSpinLock(&session->outputLock);
   bool consumed = false, recorded = false;
   if (InterlockedCompareExchange(&session->outputCompletionPending, 0, 1) == 1) {
    status = session->outputCompletionStatus;
    recorded = session->output.RecordCompletion(status);
    consumed = true;
    InterlockedIncrement(&session->outputTraceConsumed);
   }
   DWORD command = session->output.active.bytes[0];
   bool finished = session->output.Finish(now, &callback, &status);
   DWORD submitted = session->output.submitted, completed = session->output.completed;
   const DWORD unexpected = session->output.unexpectedCompletions;
   DWORD state = session->output.count | ((DWORD)session->output.activeValid << 8) |
    ((DWORD)session->output.stopped << 9) | (session->output.rumbleErrors << 16);
   KfReleaseSpinLock(&session->outputLock, irql);
   if (consumed) GipOutputTraceLog(session, XBOXINPUT_TRACE_CONSUMED, command,
    session->outputTraceTransfer, (DWORD)status, recorded ? 1 : 0, session->outputTraceConsumed);
   if (finished) {
    GipOutputTraceLog(session, XBOXINPUT_TRACE_FINISHED, command, session->outputTraceTransfer,
     (DWORD)status, completed, unexpected);
    if (completed <= 16 || status) GipOutputLog(session, 2, command,
     (DWORD)status, submitted, completed, state);
    // Continuation can enqueue the next stage but cannot modify the TRB.
    if (callback) ((int32_t (*)(DWORD, int32_t))callback)(
     (DWORD)&session->outTrb, status);
   }
   irql = KfAcquireSpinLock(&session->outputLock);
   deviceHandle* handle = session->ext.deviceHandle;
   bool prepared = session->outOpen && handle && !session->runtime.stopping &&
    session->output.Prepare(now, XboxInputAllowRuntimeRumble(XboxInputRuntimeIsReady(&session->runtime)));
   if (prepared) {
    session->outputActiveTick = now;
    session->outputStallLogged = false;
   }
   command = session->output.active.bytes[0];
   submitted = session->output.submitted; completed = session->output.completed;
   DWORD motorSnapshot = ((DWORD)session->output.rumbleDesired << 16) |
    session->output.active.motors;
   state = session->output.count | ((DWORD)session->output.activeValid << 8) |
    ((DWORD)session->output.stopped << 9) | (session->output.rumbleErrors << 16);
   bool stalled = session->output.activeValid && !session->output.completionPending &&
    !session->outputStallLogged && (DWORD)(now - session->outputActiveTick) >= 1000;
   if (stalled) session->outputStallLogged = true;
   bool summary = completed != session->outputLastCompleted &&
    (DWORD)(now - session->outputSummaryTick) >= 1000;
   if (summary) {
    session->outputLastCompleted = completed;
    session->outputSummaryTick = now;
   }
   KfReleaseSpinLock(&session->outputLock, irql);
   if (stalled) GipOutputLog(session, 4, command, now - session->outputActiveTick,
    submitted, completed, state); // Never reuse an uncompleted transfer.
   if (summary) GipOutputLog(session, 3, command, motorSnapshot, submitted, completed, state);
   if (!prepared) continue;

   // Persist this breadcrumb before entering the USB stack. A hard lock in
   // UsbdQueueAsyncTransfer cannot be logged after it happens. It records a
   // dispatch attempt (rather than proof that the USB stack accepted it), which
   // is still the distinction needed when the final queue-return token is lost.
   const bool dispatchBreadcrumb = submitted <= 16;
   if (dispatchBreadcrumb) GipOutputLog(session, 1, command, motorSnapshot,
    submitted, completed, state);

#if XBOXINPUT_USB_DPC_SUBMIT
   const DWORD enteredBefore = session->outputTraceEntered;
   const int32_t handoffStatus = KeCallAndWaitForDpcRoutinePtr ?
    KeCallAndWaitForDpcRoutinePtr(GipSubmitOutputOnUsbDpc, session,
     XBOXINPUT_USB_DPC_PROCESSOR, NULL, NULL) : -1;
   if (handoffStatus < 0) {
    // Fail closed: never fall back to the suspect direct worker submission.
    // If the USB API was entered, preserve ownership of that active storage.
    irql = KfAcquireSpinLock(&session->outputLock);
    if ((DWORD)session->outputTraceEntered == enteredBefore)
     session->output.AbortUnsubmitted();
    session->output.Stop();
    session->outOpen = false;
    submitted = session->output.submitted; completed = session->output.completed;
    KfReleaseSpinLock(&session->outputLock, irql);
    GipOutputTraceLog(session, XBOXINPUT_TRACE_DPC_FAILURE, command, 0,
     (DWORD)handoffStatus, submitted, completed);
   }
#else
   // Host-only control mode: same routine without the kernel handoff.
   GipSubmitOutputOnUsbDpc(NULL, session, NULL, NULL);
#endif
  }
  Sleep(5);
 }
 return 0;
}

static void GipLogOutputDiagnostics(DWORD now) {
 // Called only by the file-logger thread. Samples may overlap a USB callback;
 // compare successive records instead of treating one sample as a transaction.
 for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
  GipSessionSlot* session = &g_gipSessions[i];
  if (!session->reserved) continue;
  // Independent logger samples counters; never emit an event per XInput call.
  BYTE sampleIrql = KfAcquireSpinLock(&session->outputLock);
  const bool sampleDue = session->runtimeRumbleRequests &&
   (!session->runtimeRumbleSampleValid || (DWORD)(now - session->runtimeRumbleSampleTick) >= 5000) &&
   (session->outOpen || session->runtimeRumbleRequests != session->runtimeRumbleSampleRequests);
  const DWORD requests = session->runtimeRumbleRequests;
  const DWORD nonzero = session->runtimeRumbleNonzeroRequests;
  const DWORD sampleDesired = session->output.rumbleDesired;
  if (sampleDue) {
   session->runtimeRumbleSampleTick = now;
   session->runtimeRumbleSampleValid = true;
   session->runtimeRumbleSampleRequests = requests;
  }
  KfReleaseSpinLock(&session->outputLock, sampleIrql);
  if (sampleDue) GipOutputTraceLog(session, XBOXINPUT_TRACE_SUPPRESSED, 9, 0, requests, nonzero, sampleDesired);
  if (!session->outputTraceEntered) continue;
  BYTE irql = KfAcquireSpinLock(&session->outputLock);
  const bool stalled = session->output.activeValid && (DWORD)(now - session->outputActiveTick) >= 1000;
  const DWORD interval = stalled && session->outOpen ? 1000 : 5000;
  const bool due = session->outputTraceEntered &&
   (!session->outputTraceWatchdogValid || (DWORD)(now - session->outputTraceWatchdogTick) >= interval) &&
   (session->outOpen || session->output.activeValid);
  if (!due) { KfReleaseSpinLock(&session->outputLock, irql); continue; }
  session->outputTraceWatchdogTick = now; session->outputTraceWatchdogValid = true;
  const DWORD transfer = session->outputTraceTransfer;
  const DWORD command = session->output.active.bytes[0];
  const DWORD submitted = session->output.submitted;
  const DWORD age = session->output.activeValid ? now - session->outputActiveTick : 0;
  const DWORD desired = session->output.rumbleDesired;
  const DWORD active = session->output.active.motors;
  const DWORD completed = session->output.rumbleCompleted;
  const DWORD mailbox = session->outputCompletionPending;
  const DWORD state = (session->output.activeValid ? 1u : 0u) | (mailbox ? 2u : 0u) |
   (session->output.completionPending ? 4u : 0u) | (session->outputTraceInsideSubmit ? 8u : 0u) |
   (session->outOpen ? 16u : 0u) | (session->runtime.stopping ? 32u : 0u) |
   (session->output.stopped ? 64u : 0u) | (session->output.rumbleDesiredValid ? 128u : 0u) |
   (session->output.rumbleCompletedValid ? 256u : 0u) | (session->output.count << 16);
  KfReleaseSpinLock(&session->outputLock, irql);
  GipOutputTraceLog(session, XBOXINPUT_TRACE_WATCHDOG, command, transfer, age, submitted, state);
  GipOutputTraceLog(session, XBOXINPUT_TRACE_COUNTS, command, transfer,
   session->outputTraceEntered, session->outputTraceReturned, session->outputTraceCallbacks);
  GipOutputTraceLog(session, XBOXINPUT_TRACE_MAILBOX, command, transfer,
   session->outputTraceCallbackTransfer, mailbox, session->outputTraceConsumed);
  GipOutputTraceLog(session, XBOXINPUT_TRACE_LAST_CALLBACK, command, transfer,
   session->outputTraceCallbackTick, session->outputTraceCallbackStatus, session->outputTraceCallbackPreviousPending);
  GipOutputTraceLog(session, XBOXINPUT_TRACE_WORKER, command, transfer,
   session->outputTraceWorkerTick, session->outputTraceToken, session->outputTraceReturnTick);
  GipOutputTraceLog(session, XBOXINPUT_TRACE_MOTORS, command, transfer, desired, active, completed);
 }
}
