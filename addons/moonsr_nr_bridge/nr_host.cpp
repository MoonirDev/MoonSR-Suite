// SPDX-License-Identifier: GPL-3.0-only
// MoonSR_NrHost.exe: runs NVIDIA neural rendering for MoonSR on a GPU other than its main one. NGX creates features
// only on the adapter of the oldest D3D device of a process, so the bridge has to run where its device is the first
// one. The engine starts this process and drives it over a pipe; all of the logic is in src/engine/remote_stages.cpp.
// Threading: single-threaded.
#include "engine/remote_stages.h"

int wmain(int argc, wchar_t** argv) {
    return moonsr::RunNeuralHost(argc, argv);
}
