#pragma once

// Called under the voice request mutex: the terminal write and cancellation
// have exactly one winner. Once committed, even a failed write cannot promise cancellation.
struct VoiceUploadGate {
  bool committed = false;
  bool interrupted = false;
  bool interrupt() {
    if (committed) return false;
    interrupted = true;
    return true;
  }
  bool commit(bool due) {
    if (due) interrupt();
    if (interrupted) return false;
    committed = true;
    return true;
  }
};
