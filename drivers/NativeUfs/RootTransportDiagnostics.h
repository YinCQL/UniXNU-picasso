// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included with the production host and ordered read32 adapter in scope.
// Command callers serialize transport access; report only the first failure
// even when detailed tracing is disabled. Never acknowledge/clear faults here.
static bool gRootTransportFailureReported = false;

static IOReturn recordTransport (IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  gRootDiagnostic.ocs = ocs;
  if (result == kIOReturnSuccess || (gRootTransportFailureReported && !gRootDiagnosticEnabled)) return result;

  gRootDiagnostic.faultSampleMask = 0;
  if (gHost.hc) {
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
    gRootDiagnostic.doorbell = read32 (gHost.hc, 0x58);
    gRootDiagnostic.interrupt = read32 (gHost.hc, 0x20);
    gRootDiagnostic.list = uint64_t (read32 (gHost.hc, 0x50)) |
                          (uint64_t (read32 (gHost.hc, 0x54)) << 32);
    gRootDiagnostic.runStop = read32 (gHost.hc, 0x60);
    gRootDiagnostic.ahit = read32 (gHost.hc, 0x18);
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32 (gHost.hc, 0x38 + 4 * i);
    gRootDiagnostic.faultSampleMask |= 1;
  }
  if (gHost.smmu) {
    gRootDiagnostic.gfsr = read32 (gHost.smmu, 0x48);
    gRootDiagnostic.gsyn = read32 (gHost.smmu, 0x54);
    gRootDiagnostic.faultSampleMask |= 2;
  }
  if (gHost.cb) {
    gRootDiagnostic.fsr = read32 (gHost.cb, 0x58);
    gRootDiagnostic.fsyn = read32 (gHost.cb, 0x68);
    gRootDiagnostic.far = uint64_t (read32 (gHost.cb, 0x60)) |
                          (uint64_t (read32 (gHost.cb, 0x64)) << 32);
    gRootDiagnostic.faultSampleMask |= 4;
  }
  if (!gRootTransportFailureReported) {
    gRootTransportFailureReported = true;
    char snapshot[2048];
    formatUfsRootDiagnostics (snapshot, sizeof(snapshot), gRootDiagnostic,
                             "first transport failure; mapped registers sampled read-only", "NATIVE_UFS_FAILURE");
    IOLog ("%s", snapshot);
  }
  return result;
}
