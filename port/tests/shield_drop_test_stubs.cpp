// Hand-maintained no-op host stubs for the standalone shield-drop decision test.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ppc.h"
#include "host.h"
#include "hle.h"
#include "render_observer.h"
#include <functional>
namespace host {
Options options;
uint8_t* ram = nullptr;   // the test assigns its fake guest RAM
uint8_t* aram = nullptr;
ppc::Context* cpu = nullptr;
void log(const char*, ...) {}
void die(const char*, ...) { std::exit(1); }
const char* symbol_name(uint32_t) { return "test"; }
uint32_t retrace_count() { return 1; }
double now_seconds() { return 0; }
uint32_t mmio_read(uint32_t, int) { return 0; }
void mmio_write(uint32_t, uint32_t, int) {}
void post_completion(std::function<void()>) {}
void pump_completions() {}
uint8_t* ptr(uint32_t addr, uint32_t) { return ram + (addr - ppc::RAM_BASE); }
uint32_t rd32(uint32_t addr) { uint32_t v; std::memcpy(&v, ptr(addr, 4), 4); return v; }
void wr32(uint32_t addr, uint32_t v) { std::memcpy(ptr(addr, 4), &v, 4); }
uint8_t rd8(uint32_t addr) { return *ptr(addr, 1); }
void wr8(uint32_t addr, uint8_t v) { *ptr(addr, 1) = v; }
void audio_push(const unsigned char*, unsigned long) {}
void call_guest(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void sim_cost_add(int, double) {}
}

namespace ppc {
void loop_poll(Context&) {}
void interrupts_enabled(Context&) {}
}

namespace slippi {
void dma_read(uint32_t, uint32_t) {}
void dma_write(uint32_t, uint32_t) {}
uint32_t imm_read(uint32_t) { return 0; }
void imm_write(uint32_t, uint32_t) {}
}

namespace ax { void handle_mail(uint32_t) {} }

gx::RenderObserver::RenderObserver(ppc::Context& cpu, gx::Observe kind, unsigned char*) : cpu_(cpu), kind_(kind) {}
gx::RenderObserver::~RenderObserver() {}
namespace hle { void AIGetDSPSampleRate(ppc::Context&, uint8_t*) {} }
namespace hle { void AIGetStreamPlayState(ppc::Context&, uint8_t*) {} }
namespace hle { void AIGetStreamSampleRate(ppc::Context&, uint8_t*) {} }
namespace hle { void AIGetStreamVolLeft(ppc::Context&, uint8_t*) {} }
namespace hle { void AIGetStreamVolRight(ppc::Context&, uint8_t*) {} }
namespace hle { void AIInit(ppc::Context&, uint8_t*) {} }
namespace hle { void AIInitDMA(ppc::Context&, uint8_t*) {} }
namespace hle { void AIRegisterDMACallback(ppc::Context&, uint8_t*) {} }
namespace hle { void AISetDSPSampleRate(ppc::Context&, uint8_t*) {} }
namespace hle { void AISetStreamPlayState(ppc::Context&, uint8_t*) {} }
namespace hle { void AISetStreamVolLeft(ppc::Context&, uint8_t*) {} }
namespace hle { void AISetStreamVolRight(ppc::Context&, uint8_t*) {} }
namespace hle { void AIStartDMA(ppc::Context&, uint8_t*) {} }
namespace hle { void ARAlloc(ppc::Context&, uint8_t*) {} }
namespace hle { void ARFree(ppc::Context&, uint8_t*) {} }
namespace hle { void ARGetSize(ppc::Context&, uint8_t*) {} }
namespace hle { void ARInit(ppc::Context&, uint8_t*) {} }
namespace hle { void ARQInit(ppc::Context&, uint8_t*) {} }
namespace hle { void ARQPostRequest(ppc::Context&, uint8_t*) {} }
namespace hle { void ARRegisterDMACallback(ppc::Context&, uint8_t*) {} }
namespace hle { void ARStartDMA(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDCheckAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDClose(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDCreateAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDDeleteAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDFastOpen(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDFormatAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDFreeBlocks(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDGetStatus(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDGetXferredBytes(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDInit(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDMountAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDOpen(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDProbe(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDProbeEx(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDRead(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDReadAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDRenameAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDSetStatusAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDUnmount(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDWrite(ppc::Context&, uint8_t*) {} }
namespace hle { void CARDWriteAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void DSPAddTask(ppc::Context&, uint8_t*) {} }
namespace hle { void DSPAssertTask(ppc::Context&, uint8_t*) {} }
namespace hle { void DSPCheckInit(ppc::Context&, uint8_t*) {} }
namespace hle { void DSPCheckMailFromDSP(ppc::Context&, uint8_t*) {} }
namespace hle { void DSPCheckMailToDSP(ppc::Context&, uint8_t*) {} }
namespace hle { void DSPInit(ppc::Context&, uint8_t*) {} }
namespace hle { void DSPReadMailFromDSP(ppc::Context&, uint8_t*) {} }
namespace hle { void DSPSendMailToDSP(ppc::Context&, uint8_t*) {} }
namespace hle { void DVDCancel(ppc::Context&, uint8_t*) {} }
namespace hle { void DVDCancelAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void DVDCheckDisk(ppc::Context&, uint8_t*) {} }
namespace hle { void DVDGetCurrentDiskID(ppc::Context&, uint8_t*) {} }
namespace hle { void DVDGetDriveStatus(ppc::Context&, uint8_t*) {} }
namespace hle { void DVDInit(ppc::Context&, uint8_t*) {} }
namespace hle { void DVDReadAbsAsyncPrio(ppc::Context&, uint8_t*) {} }
namespace hle { void DVDReadAsyncPrio(ppc::Context&, uint8_t*) {} }
namespace hle { void DVDReset(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIAttach(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIClearInterrupts(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIDeselect(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIDetach(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIDma(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIGetID(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIGetState(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIImm(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIImmEx(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIInit(ppc::Context&, uint8_t*) {} }
namespace hle { void EXILock(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIProbe(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIProbeEx(ppc::Context&, uint8_t*) {} }
namespace hle { void EXISelect(ppc::Context&, uint8_t*) {} }
namespace hle { void EXISetExiCallback(ppc::Context&, uint8_t*) {} }
namespace hle { void EXISync(ppc::Context&, uint8_t*) {} }
namespace hle { void EXIUnlock(ppc::Context&, uint8_t*) {} }
namespace hle { void OSCreateThread(ppc::Context&, uint8_t*) {} }
namespace hle { void OSLoadContext(ppc::Context&, uint8_t*) {} }
namespace hle { void OSPanic(ppc::Context&, uint8_t*) {} }
namespace hle { void OSResetSystem(ppc::Context&, uint8_t*) {} }
namespace hle { void OSResumeThread(ppc::Context&, uint8_t*) {} }
namespace hle { void OSSleepThread(ppc::Context&, uint8_t*) {} }
namespace hle { void OSSuspendThread(ppc::Context&, uint8_t*) {} }
namespace hle { void OSWakeupThread(ppc::Context&, uint8_t*) {} }
namespace hle { void PADControlMotor(ppc::Context&, uint8_t*) {} }
namespace hle { void PADInit(ppc::Context&, uint8_t*) {} }
namespace hle { void PADRead(ppc::Context&, uint8_t*) {} }
namespace hle { void PADRecalibrate(ppc::Context&, uint8_t*) {} }
namespace hle { void PADReset(ppc::Context&, uint8_t*) {} }
namespace hle { void PADSetSamplingRate(ppc::Context&, uint8_t*) {} }
namespace hle { void PADSetSpec(ppc::Context&, uint8_t*) {} }
namespace hle { void SIBusy(ppc::Context&, uint8_t*) {} }
namespace hle { void SIDisablePolling(ppc::Context&, uint8_t*) {} }
namespace hle { void SIEnablePolling(ppc::Context&, uint8_t*) {} }
namespace hle { void SIGetResponse(ppc::Context&, uint8_t*) {} }
namespace hle { void SIGetStatus(ppc::Context&, uint8_t*) {} }
namespace hle { void SIGetType(ppc::Context&, uint8_t*) {} }
namespace hle { void SIGetTypeAsync(ppc::Context&, uint8_t*) {} }
namespace hle { void SIInit(ppc::Context&, uint8_t*) {} }
namespace hle { void SIIsChanBusy(ppc::Context&, uint8_t*) {} }
namespace hle { void SIRefreshSamplingRate(ppc::Context&, uint8_t*) {} }
namespace hle { void SIRegisterPollingHandler(ppc::Context&, uint8_t*) {} }
namespace hle { void SISetCommand(ppc::Context&, uint8_t*) {} }
namespace hle { void SISetXY(ppc::Context&, uint8_t*) {} }
namespace hle { void SITransfer(ppc::Context&, uint8_t*) {} }
namespace hle { void SIUnregisterPollingHandler(ppc::Context&, uint8_t*) {} }
namespace hle { void __OSInitAudioSystem(ppc::Context&, uint8_t*) {} }
namespace hle { void __OSReschedule(ppc::Context&, uint8_t*) {} }
namespace hle { void __OSStopAudioSystem(ppc::Context&, uint8_t*) {} }
namespace hle { void __OSUnhandledException(ppc::Context&, uint8_t*) {} }
namespace hle { void __read_console(ppc::Context&, uint8_t*) {} }
namespace hle { void __write_console(ppc::Context&, uint8_t*) {} }
namespace hle { void exit(ppc::Context&, uint8_t*) {} }
namespace hle { void longjmp(ppc::Context&, uint8_t*) {} }
