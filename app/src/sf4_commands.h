// sf4 - the game's command stream (docs/d3d-map.md): the opcodes, a walker over a recorded buffer, guest-memory
// reads that cannot fault, and the hook on the player sub_82287DF0 that lets diagnostics look at each buffer before
// it is played.

#pragma once

#include <cstdint>

namespace sf4::commands {

constexpr uint32_t kOpcodes = 61;

// Names of the opcodes (docs/d3d-map.md, "The opcodes"); "unused" where the player has the empty handler.
const char* OpName(uint32_t op);

// Guest memory, read only after checking that every page is mapped readable.
bool Readable(uint32_t address, uint32_t bytes);
uint32_t Word(uint32_t address);  // big-endian word; call only when Readable
float Float(uint32_t address);

// Walks a buffer the way sub_82287DF0 does: segments of commands, header = opcode << 16 | length in words (header
// included), 0x00010000 ends a segment. Calls visit(op, address of the header, length) for each command and returns
// false if the buffer could not be read or looked malformed (the commands before that were visited).
template <typename Visit>
bool ForEachCommand(uint32_t buffer, Visit&& visit) {
  if (!Readable(buffer, 24)) return false;
  const uint32_t words = Word(buffer + 8);
  uint32_t segment = Word(buffer + 16);
  const uint32_t segments_end = Word(buffer + 20);
  for (int s = 0; segment != segments_end && s < 4096; ++s, segment += 8) {
    if (!Readable(segment, 8)) return false;
    uint32_t command = words + 4 * Word(segment + 4);
    for (int n = 0; n < 200000; ++n) {
      if (!Readable(command, 4)) return false;
      const uint32_t header = Word(command);
      if (header == 0x00010000) break;
      const uint32_t op = header >> 16, length = header & 0xFFFF;
      if (op >= kOpcodes || length == 0 || length > 4096) return false;
      visit(op, command, length);
      command += 4 * length;
    }
  }
  return true;
}

// Called with the buffer argument of sub_82287DF0 before the original plays it, on the thread that plays it.
// Observers are registered at start-up (they are not removed); at most 4.
using PlayerObserver = void (*)(uint32_t buffer);
void AddPlayerObserver(PlayerObserver observer);
// The same, after the original has played the buffer.
void AddAfterPlayObserver(PlayerObserver observer);

// The Direct3D draw functions the draw opcodes call (docs/d3d-map.md), as the handlers call them while a buffer is
// being played (calls from anywhere else are not reported):
//   kDrawVertices          sub_8228B258(device, type, start vertex, vertex count)                op 28
//   kDrawIndexedVertices   sub_8228B648(device, type, base vertex, start index, index count)     op 29
//   kBeginVertices         sub_8228A8D0(device, type, vertex count, stride)                      op 30
//   kBeginIndexedVertices  sub_8228AD68(device, type, ..., index count in r7)                    op 31
enum DrawFunction : uint32_t { kDrawVertices, kDrawIndexedVertices, kBeginVertices, kBeginIndexedVertices };
struct DrawCall {
  DrawFunction function;
  uint32_t type;
  uint32_t first;   // start vertex, base vertex, or 0
  uint32_t second;  // start index (indexed), stride (BeginVertices), or 0
  uint32_t count;   // vertices or indices
};
using DrawObserver = void (*)(const DrawCall& call);
void AddDrawObserver(DrawObserver observer);

}  // namespace sf4::commands
