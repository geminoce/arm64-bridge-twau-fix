// SPDX-License-Identifier: GPL-3.0-or-later
#include "dynarmic_host.h"
#include "thunks_android.h"
#include <jni.h>
#include <cstdio>

JavaVM* GetHostJavaVM() { return nullptr; }
int RunGraphicsContractTests(DynarmicHost& host);

int main() {
    DynarmicHost host;
    if (!host.Initialize()) { std::fprintf(stderr, "Host initialization failed\n"); return 2; }
    const int result = RunGraphicsContractTests(host);
    ResetAndroidThunks(host);
    return result ? result : host.HasFailed() ? 3 : 0;
}
