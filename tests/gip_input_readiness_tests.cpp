#include "../source/gip_input_readiness.h"
#include "../source/gip_output_queue.h"
#include "../source/gip_gamepad_parser.h"
#include <assert.h>
#include <stdio.h>

#ifdef NDEBUG
#error Readiness regression tests require assertions enabled
#endif

static bool ParseAndMayRegister(const XboxInputGipInputReadiness& readiness,
	const XboxInputOutputQueue& output, const uint8_t* payload, int length,
	bool startupRequired = true, bool stopping = false) {
	XboxInputGipGamepadState state = {};
	return XboxInputParseGipGamepadPayload(payload, length, &state) &&
		readiness.CanRegisterFromInput(startupRequired, stopping, output.stopped);
}

static void TestLoggedInputOnlyConnections() {
	// Replay the relevant Test 15 log sequence on three fresh sessions:
	// POWER -> LED -> AUTH -> RUMBLE setup succeeds, then valid INPUT.
	// No ANNOUNCE or IDENTIFY is required in this supported resume path.
	const uint8_t commands[] = {0x05, 0x0A, 0x06, 0x09};
	const uint8_t input[14] = {0x10}; // A button, otherwise neutral.
	for (unsigned connection = 0; connection < 3; ++connection) {
		XboxInputOutputQueue output;
		output.Initialize();
		XboxInputGipInputReadiness readiness = {};
		readiness.BeginStartup();
		assert(!ParseAndMayRegister(readiness, output, input, sizeof(input)));
		for (unsigned stage = 0; stage < sizeof(commands); ++stage) {
			const uint8_t packet[4] = {commands[stage], 0, 1, 0};
			assert(output.Enqueue(packet, sizeof(packet), stage + 1));
			assert(output.Prepare(stage * 6, false));
			assert(!ParseAndMayRegister(readiness, output, input, sizeof(input)));
			assert(output.RecordCompletion(0));
			uint32_t callback = 0;
			int32_t status = -1;
			assert(output.Finish(stage * 6 + 6, &callback, &status));
			assert(status == 0 && callback == stage + 1);
			if (commands[stage] == 0x09) {
				// Same state updates as GipEstablishStoppedRumble in production.
				output.EstablishStoppedRumble(stage * 6 + 6);
				readiness.CompleteStartup();
			}
			else assert(!ParseAndMayRegister(readiness, output, input, sizeof(input)));
		}
		assert(output.submitted == 4 && output.completed == 4);
		assert(ParseAndMayRegister(readiness, output, input, sizeof(input)));
		assert(!ParseAndMayRegister(readiness, output, input, sizeof(input) - 1));
		// Registration does not remove any runtime rumble ownership/timing rules.
		output.RequestRumble(100, 50);
		assert(!output.Prepare(123, true));
		assert(output.Prepare(124, true));
		assert(output.active.rumble && output.active.motors == 0x6432);
	}
}

static void TestRestartFailureAndRetirement() {
	const uint8_t input[14] = {0x10};
	XboxInputOutputQueue output;
	output.Initialize();
	XboxInputGipInputReadiness readiness = {};
	readiness.CompleteStartup();
	assert(ParseAndMayRegister(readiness, output, input, sizeof(input)));
	// A later post-IDENTIFY startup must finish again before new registration.
	readiness.BeginStartup();
	assert(!ParseAndMayRegister(readiness, output, input, sizeof(input)));
	readiness.CompleteStartup();
	assert(ParseAndMayRegister(readiness, output, input, sizeof(input)));
	assert(!ParseAndMayRegister(readiness, output, input, sizeof(input), true, true));
	output.Stop();
	assert(!ParseAndMayRegister(readiness, output, input, sizeof(input)));
	readiness.CompleteStartup(); // A late signal cannot revive retired output.
	assert(!ParseAndMayRegister(readiness, output, input, sizeof(input)));

	output.Initialize();
	readiness.BeginStartup();
	const uint8_t setup[4] = {9, 0, 1, 0};
	assert(output.Enqueue(setup, sizeof(setup), 1));
	assert(output.Prepare(0, false) && output.RecordCompletion(-1));
	uint32_t callback = 99;
	int32_t status = 0;
	assert(output.Finish(1, &callback, &status));
	assert(status == -1 && callback == 0 && output.stopped);
	assert(!readiness.startupComplete);
	assert(!ParseAndMayRegister(readiness, output, input, sizeof(input)));
}

static void TestStandardProfilesAndIndependentPlayers() {
	const uint8_t input[14] = {0x10};
	XboxInputOutputQueue output;
	output.Initialize();
	XboxInputGipInputReadiness standard = {};
	assert(ParseAndMayRegister(standard, output, input, sizeof(input), false));
	assert(!ParseAndMayRegister(standard, output, input, sizeof(input), false, true));
	output.Stop();
	assert(!ParseAndMayRegister(standard, output, input, sizeof(input), false));

	XboxInputGipInputReadiness players[4] = {};
	for (unsigned p = 0; p < 4; ++p) {
		players[p].CompleteStartup();
		for (unsigned other = 0; other < 4; ++other)
			assert(players[other].CanRegisterFromInput(true, false, false) == (other <= p));
	}
	players[0].BeginStartup();
	assert(!players[0].CanRegisterFromInput(true, false, false));
	for (unsigned p = 1; p < 4; ++p)
		assert(players[p].CanRegisterFromInput(true, false, false));
}

int main() {
	TestLoggedInputOnlyConnections();
	TestRestartFailureAndRetirement();
	TestStandardProfilesAndIndependentPlayers();
	puts("PASS: input-only startup/registration policy, incomplete/failed/restarted startup, retirement, malformed input, standard profiles, independent players");
	return 0;
}
