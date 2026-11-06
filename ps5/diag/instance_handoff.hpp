#pragma once

#include <string>

namespace archivefetch_diag {

enum class HandoffOutcome { PortFreed, NotOurs, StillHeld };

struct HandoffResult {
    HandoffOutcome outcome;
    char previous_version[48];  // empty when the holder did not report one
    // Unsaved S3 keys the previous copy held in memory; empty when there were none.
    std::string access_key;
    std::string secret_key;
};

// Asks an earlier ArchiveFetch on the port to stop and waits for the port; never kills a process.
HandoffResult handoff_previous_instance(int port);

const char* handoff_outcome_name(HandoffOutcome outcome);

}
