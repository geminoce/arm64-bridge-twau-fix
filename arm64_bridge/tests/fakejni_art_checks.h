// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <jni.h>
class DynarmicHost;

// Diagnostic-only: after group load/class-loader capture, before guest JNI_OnLoad.
// Caller treats false as bootstrap failure. No game entry point is invoked.
bool RunWolfFakeJniArtChecks(JNIEnv* env, DynarmicHost& host);
