#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The checksum D3D12 verifies before it accepts DXBC bytecode. It is MD5 over
// everything after the checksum field. DXBC pads the last block its own way.
void dxbc_checksum(const uint8_t *data, size_t size, uint32_t out[4]);

// Inserts instruction tokens into the code chunk at byte offset `at`. It
// must be the start of an instruction or the end of the stream. The stream
// length, chunk size, container size and later chunk offsets are updated.
// DXBC control flow is structured. No jump targets need fixing. The caller
// redoes the checksum (dxbc_checksum). False if the code or offset is wrong.
bool dxbc_insert(std::vector<uint8_t> &code, size_t at, const uint32_t *tokens, size_t count);
// Raises dcl_temps to at least `temps` for inserted code that needs more
// registers. False if there is no dcl_temps.
bool dxbc_set_temps(std::vector<uint8_t> &code, uint32_t temps);
