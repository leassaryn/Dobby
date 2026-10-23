#include "dobby_internal.h"
#include <array>
#include <mutex>

namespace {
std::mutex event_mutex;
std::array<DobbyDiagnosticEvent, 256> events;
size_t head = 0, count = 0;
uint64_t sequence = 0, dropped = 0;
bool enabled = false;
} // namespace
void DobbyRecordEvent(DobbyEventKind kind, DobbyError error, void *address, size_t size, uint64_t detail,
                      int system_error) {
  std::lock_guard<std::mutex> lock(event_mutex);
  if (!enabled)
    return;
  if (count == events.size()) {
    head = (head + 1) % events.size();
    --count;
    ++dropped;
  }
  events[(head + count++) % events.size()] = {++sequence, kind, error, system_error, address, size, detail};
}
PUBLIC int DobbyEnableDiagnostics(int value) {
  std::lock_guard<std::mutex> lock(event_mutex);
  enabled = value != 0;
  head = count = 0;
  dropped = 0;
  return RS_SUCCESS;
}
PUBLIC int DobbyReadDiagnostics(DobbyDiagnosticEvent *output, size_t capacity, size_t *written, uint64_t *lost) {
  if (!written || (!output && capacity))
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  std::lock_guard<std::mutex> lock(event_mutex);
  if (lost)
    *lost = dropped;
  if (!output && !capacity) {
    *written = count;
    return RS_SUCCESS;
  }
  *written = 0;
  while (count && *written < capacity) {
    output[(*written)++] = events[head];
    head = (head + 1) % events.size();
    --count;
  }
  return RS_SUCCESS;
}
