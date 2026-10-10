#pragma once

// XDK-neutral session readiness policy, exercised by native regression tests.
// The Xbox adapter protects updates and snapshots with its output spinlock.
struct XboxInputGipInputReadiness {
	bool startupComplete;

	void BeginStartup() {
		startupComplete = false;
	}

	// Called only after the required final startup OUT transfer succeeded.
	// A new ANNOUNCE/IDENTIFY exchange is not required for resumed INPUT.
	void CompleteStartup() {
		startupComplete = true;
	}

	bool CanRegisterFromInput(bool startupRequired, bool sessionStopping,
		bool outputStopped) const {
		return !sessionStopping && !outputStopped &&
			(!startupRequired || startupComplete);
	}
};
