// SPDX-License-Identifier: MIT
// CEF's renderer, GPU and utility processes (SPEC §2.3): a separate small executable, so
// subprocesses do not run the main binary's static initialisation (nmos-cpp, Boost).
#include "cef/runtime.hpp"

int main(int argc, char** argv)
{
    return mbs::cef::executeSubprocess(argc, argv);
}
