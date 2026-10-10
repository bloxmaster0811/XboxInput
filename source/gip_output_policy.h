#pragma once

// Isolated Test26 diagnostic policy, not a production default.
// Previous modes and the DPC handoff are compiled by host transport tests.
#ifndef XBOXINPUT_USB_DPC_SUBMIT
#define XBOXINPUT_USB_DPC_SUBMIT 1
#endif
#if XBOXINPUT_USB_DPC_SUBMIT != 0 && XBOXINPUT_USB_DPC_SUBMIT != 1
#error Invalid USB submission context policy
#endif
enum { XBOXINPUT_USB_DPC_PROCESSOR = 2 };
#include "gip_output_queue.h"
#include "gip_output_trace.h"
#ifndef XBOXINPUT_DETAILED_OUTPUT_TRACE
#define XBOXINPUT_DETAILED_OUTPUT_TRACE 0
#endif
#if XBOXINPUT_DETAILED_OUTPUT_TRACE != 0 && XBOXINPUT_DETAILED_OUTPUT_TRACE != 1
#error Invalid output trace diagnostic policy
#endif
static inline bool XboxInputPublishOutputTrace(unsigned phase, unsigned a,
 unsigned b, unsigned c) {
#if !XBOXINPUT_DETAILED_OUTPUT_TRACE
 // Suppress the six successful per-transfer records before ring publication.
 // Keep errors/anomalies and independently sampled watchdog/request records.
 if (phase >= XBOXINPUT_TRACE_ENTER && phase <= XBOXINPUT_TRACE_FINISHED) {
  if (phase == XBOXINPUT_TRACE_CALLBACK) return a != 0 || b != 0;
  if (phase == XBOXINPUT_TRACE_CONSUMED) return a != 0 || b == 0;
  if (phase == XBOXINPUT_TRACE_FINISHED) return a != 0 || c != 0;
  return false;
 }
#else
 (void)phase; (void)a; (void)b; (void)c;
#endif
 return true;
}
#ifndef XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE
#define XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE 0
#endif
#ifndef XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE
#define XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE 0
#endif
#if XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE != 0 && XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE != 1
#error Invalid runtime rumble diagnostic policy
#endif
#if XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE != 0 && XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE != 1
#error Invalid wire rumble diagnostic policy
#endif
#if XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE && XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE
#error Diagnostic modes must be mutually exclusive
#endif
static inline bool XboxInputAllowRuntimeRumble(bool ready) {
 return ready && !XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE;
}
static inline void XboxInputApplyRuntimeOutputPolicy(XboxInputOutputPacket* packet) {
#if XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE
 // Dispatcher only, before USB submission: never alter an outstanding packet.
 // Keep the requested motors for scheduling/deduplication; only wire bytes
 // change. Ordered startup packets have rumble=false and must be untouched.
 if (packet->rumble && packet->length == 13) {
  packet->bytes[6] = packet->bytes[7] = 0;
  packet->bytes[8] = packet->bytes[9] = 0;
 }
#else
 (void)packet;
#endif
}
