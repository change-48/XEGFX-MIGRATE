### Machine

MacBookPro16,2 (Core Ultra 7 255H laptop, 32 GB, SMBIOS spoof, board Mac-5F9802EFE386AA28)

### CPU, GPU, Wi-Fi / Bluetooth card

Intel Core Ultra 7 255H · Intel Arrow Lake Xe-LPG+ `8086:7D51` · Wi-Fi `8086:0094` · Bluetooth BCM_4350C2

### macOS version and build, OpenCore version

26.5.2 (25F84), OpenCore DBG-108-2026-09-27

### Which project is this about?

WTLIntelGfx / XeGfx 0.0.46 (kernel driver, accelerator and plugin built from `xe-2.6.2`);
`XeGfxIntelFB` 0.8, `XeGfxAGDC` 0.1.1, `XeGfxAccel` 0.1, `XeGfxDisplay` 0.5, `IGPUConsoleRange` 0.4.

### What happened

Bring-up on a **3120x2080 @ 120 Hz** eDP panel. It works: WindowServer composites on the iGPU through
Metal -> NVMTL -> ANV, Liquid Glass renders, `Resolution 3120 x 2080 / UI Looks like 1560 x 1040 @ 120 Hz /
Online: Yes`, `boot_steps = 123`.

Two facts from this machine may be worth having on file, both written up in
https://github.com/watchthelight-labs/WTLIntelGfx/issues/14 :

1. **The panel width breaks the "surface pitch == scanout pitch" assumption.** 3120 x 4 = 12480 and
   12480 / 256 = 48.75, so CoreAnimation pads its IOSurface rows to 12544 while the firmware console stays
   at `PLANE_STRIDE_1_A = 0xc3` (195 x 64 = 12480). On 2560-wide panels 2560 x 4 = 10240 = 256 x 40, so the
   two are equal by construction. Here that means `iop_xe_flips` and `iop_xe_defer_queued` stay 0 and every
   frame is copied by the CPU (about 4 ms at this size).

2. **WindowServer blocks every frame.** A `sample` taken during a window drag shows 666 of 669 samples under
   `CompositorMetal::CompositeLayersToDestination -> MetalContext::Flush -> commitAndWaitUntilSubmitted ->
   waitUntilCompleted -> __psynch_cvwait`. It waits because the pixels must be finished before the copy.

Also worth knowing for other testers: after two boots that ended in a hard freeze, the next boot came up in
"CPU rendering, no HiDPI" and looked like a dead kext. It was `XeGfxIntelFB` own guard
(`boot guard tripped (two starts without WindowServer): declining`); `nvram xeintelfb-starts=%00` clears it
and the next boot is normal again.

One more thing that may be relevant to #14: a panic report from this machine (2026-10-31 22:51, kept below)
has this backtrace, i.e. a fault inside the deferred flip path itself:

```
org.watchthelight.XeGfxAccel : __ZL19nvAccelResidentCallPKcPvS1_S1_S1_ + 0x7e
org.watchthelight.XeGfxAccel : __ZL16nvAccelXeFlipArmP9IOSurfacejjjb + 0x2ae
org.watchthelight.XeGfxAccel : __ZL21nvAccelXeFlipDeferRunPvS_ + 0xc4
panic: Kernel trap, type = 14=page fault, CR2: 0x5dfe22000
```

We are not claiming this is the same failure as the frozen frames reported in #14 (different build, different
day); it is here only because it is the one record we have of `nvAccelXeFlipArm()` actually being entered, and
the fault lands in the `nvAccelResidentCall()` it makes.

No request in this issue, just the machine on file. Plane registers, the framebuffer node, the loaded kexts,
the boot sequence and the accelerator counters are below.

### Logs

**Plane registers (userspace, after the desktop came up)**

```text
magic            0x5845
switched         0
gtt_mapped       1
gtt_entries      1048576
live_pte         0x800003
plan_offset      0x1000000
SURF             0xfd000000
SURFLIVE         0xfd000000
PS_CTRL          0x00000200
PIPESRC          0x0c2f081f
PLANE_SIZE       0x081f0c2f
PLANE_STRIDE     195
PLANE_CTL        0x84000008
SURF_before      0xfe600000
phys_base        0x71800000
FRMCOUNT         1749304
```

**Accelerator counters**

```text
iop = 1
iop_ok = 978
iop_fail = 0
iop_xe_flips = 0
iop_copy_sum_us = 4071120
iop_copy_max_us = 24729
```

**XeGfxIntelFramebuffer node**

```text
+-o .Display_boot  <class XeGfxIntelFramebuffer, id 0x100000500, registered, matched, active, busy 0 (24 ms), retain 25>
  | {
  |   "IOPMStrictTreeOrder" = Yes
  |   "IOFBMemorySize" = 25958400
  |   "boot-gamma-restored" = <00000000>
  |   "IOFBDependentIndex" = 0
  |   "IOFBUIScale" = <02000000>
  |   "IOFBGammaHeaderSize" = 12
  |   "IOFBGammaCount" = 256
  |   "IOFBCurrentPixelCount" = 7507200
  |   "IOFramebufferOpenGLIndex" = 0
  |   "IONameMatched" = "display"
  |   "XeGfxL0Ready" = Yes
  |   "IODisplayParameters" = "IOFramebufferParameterHandler is not serializable"
  |   "startup-timing" = <0000000000000080000000000000000000000000000000000000000000000000300c000020080000000000000000000080cdaf350000000080cdaf350000000080cdaf3500000000300c000018010000d40000000400000020080000800000007a0000000100000000000000000000000000000000000000000000000000000000000000000000000100000000000000010001000100010000000000000000000000000000000000>
  |   "IOAccelTypes" = "IOService:/AppleACPIPlatformExpert/PC00/AppleACPIPCI/IGPU@2/NMXAccel"
  |   "IOPersonalityPublisher" = "org.watchthelight.XeGfxIntelFB"
  |   "IOPowerManagement" = {"ChildrenPowerState"=2,"MaxPowerState"=2,"CurrentPowerState"=2,"CapabilityFlags"=32784,"ChildProxyPowerState"=2,"DriverPowerState"=1}
  |   "IOFBCurrentPixelClock" = 900721363
  |   "IOFBGammaWidth" = 8
  |   "IOFBDependentID" = 6360568414703976449
  |   "graphic-options" = 0
  |   "IOFBConfig" = {"IOFBModes"=({"ID"=100,"DM"=<300c00002008000022fb770000000000030080003601d200000000000000000000000000>,"DF"=8388611,"TM"=<64000000000000000000000000000000000000000000000000000000000000000000000000000000d3eaaf3500000000d3eaaf3500000000d3eaaf3500000000300c000018010000d40000000400000020080000800000007a0000000100000000000000000000000000000000000000000000000000000000000000000000000100000000000000010001000100010000000000000000000000000000000000>}),"dims"=<ffffffffffffffff0002000007000000>}
  |   "IOFBWaitCursorFrames" = 29
  |   "IOAccelIndex" = 0
  |   "IOFBIntegrated" = Yes
  |   "IOFBProbeOptions" = 1
  |   "IOFBNeedsRefresh" = Yes
  |   "IOFBWaitCursorPeriod" = 33333333
  |   "IOFBTransform" = 0
  |   "CFBundleIdentifier" = "org.watchthelight.XeGfxIntelFB"
  |   "IOFBDetailedTimings" = (<000000000000000000000000000000000000000000000000300c0000200800000000000000000000d3eaaf3500000000d3eaaf3500000000d3eaaf3500000000300c000018010000d40000000400000020080000800000007a0000000100000000000000000000000000000000000000000000000000000000000000000000000100000000000000010001000100010000000000000000000000000000000000>)
  |   "IOFBCursorInfo" = ()
  |   "IOProviderClass" = "IOPCIDevice"
  |   "IOCFPlugInTypes" = {"UUID-8"="IOAccelerator2D.plugin"}
  |   "IOAccelRevision" = 2
  |   "IONameMatch" = "display"
  |   "IOClass" = "XeGfxIntelFramebuffer"
  |   "CFBundleIdentifierKernel" = "org.watchthelight.XeGfxIntelFB"
  |   "IOFBCurrentPixelCountReal" = 7507200
  |   "IOGeneralInterest" = "IOCommand is not serializable"
  |   "IOPCIMatch" = "0x7d518086 0x7d678086 0x7d418086 0x7dd18086"
  |   "IOMatchCategory" = "IOFramebuffer"
  |   "IOProbeScore" = 20001
  |   "IONDRVFramebufferGeneration" = <0100000001000000>
  | }
  | 
  +-o AGPM  <class AGPM, id 0x10000055e, registered, matched, active, busy 0 (7 ms), retain 9>
  | | {
  | |   "max-value" = 15
  | |   "min-value" = 0
  | |   "IOReportLegendPublic" = Yes
  | |   "GPUInternalSingleSlice" = {"max-value"=15,"min-value"=0}
  | |   "IOPowerManagement" = {"CapabilityFlags"=0,"CurrentPowerState"=0}
  | |   "Heuristic-ID" = <02000000>
  | |   "IOReportLegend" = ({"IOReportChannels"=((5795122739160566084,6442450945,"PlimitNS"),(6008203707511294276,6442450945,"PlimitSampleTimeNS")),"IOReportGroupName"="AGPMStatistics","IOReportChannelInfo"={"IOReportChannelUnit"=0}})
  | | }
  | | 
  | +-o AGPMClient  <class AGPMClient, id 0x1000005f2, !registered, !matched, active, busy 0, retain 5>
  |     {
  |       "IOUserClientCreator" = "pid 111, powerd"
  |       "IOUserClientDefaultLocking" = Yes
  |     }
  |     
  +-o AppleMCCSControlModule  <class AppleMCCSControlModule, id 0x100000560, registered, matched, active, busy 0 (1 ms), retain 7>
  | | {
  | |   "IOProbeScore" = 0
  | |   "CFBundleIdentifier" = "com.apple.driver.AppleMCCSControl"
  | |   "IOMatchCategory" = "AppleMCCSControlFamily"
  | |   "IOClass" = "AppleMCCSControlModule"
  | |   "IOPersonalityPublisher" = "com.apple.driver.AppleMCCSControl"
  | |   "IOKitDebug" = 0
  | |   "IOProviderClass" = "IOFramebuffer"
  | |   "CFBundleIdentifierKernel" = "com.apple.driver.AppleMCCSControl"
  | |   "IOUserClientClass" = "AppleMCCSUserClient"
  | |   "IOResourceMatch" = "IOKit"
  | | }
  | | 
  | +-o AppleMCCSParameterHandler  <class AppleMCCSParameterHandler, id 0x100000562, registered, matched, active, busy 0 (0 ms), retain 5>
  |     {
  |       "IOProbeScore" = 0
  |       "CFBundleIdentifier" = "com.apple.driver.AppleMCCSControl"
  |       "IOMatchCategory" = "IODisplayParameters"
  |       "IOClass" = "AppleMCCSParameterHandler"
  |       "IOPersonalityPublisher" = "com.apple.driver.AppleMCCSControl"
  |       "IOKitDebug" = 0
  |       "AppleMCCSFeatures" = 1
  |       "IOProviderClass" = "AppleMCCSControlFamily"
  |       "CFBundleIdentifierKernel" = "com.apple.driver.AppleMCCSControl"
  |       "IOResourceMatch" = "IOKit"
  |     }
  |     
  +-o AppleUpstreamUserClientDriver  <class AppleUpstreamUserClientDriver, id 0x100000561, registered, matched, active, busy 0 (1 ms), retain 5>
  |   {
  |     "IOProbeScore" = 0
  |     "CFBundleIdentifier" = "com.apple.driver.AppleUpstreamUserClient"
  |     "IOMatchCategory" = "AppleUpstreamUserClientDriver"
  |     "IOClass" = "AppleUpstreamUserClientDriver"
  |     "IOPersonalityPublisher" = "com.apple.driver.AppleUpstreamUserClient"
  |     "IOProviderClass" = "IOFramebuffer"
  |     "CFBundleIdentifierKernel" = "com.apple.driver.AppleUpstreamUserClient"
  |     "IOUserClientClass" = "AppleUpstreamUserClient"
  |     "IOResourceMatch" = "IOKit"
  |   }
  |   
  +-o display0  <class IODisplayConnect, id 0x1000005c1, registered, matched, active, busy 0 (2 ms), retain 6>
  | | {
  | | }
  | | 
  | +-o AppleDisplay  <class AppleDisplay, id 0x1000005c3, registered, matched, active, busy 0 (1 ms), retain 8>
  |     {
  |       "IOClass" = "AppleDisplay"
  |       "CFBundleIdentifier" = "com.apple.iokit.IOGraphicsFamily"
  |       "IOProviderClass" = "IODisplayConnect"
  |       "DisplayProductID" = 16761
  |       "IODisplayEDID" = <00ffffffffffff00148f79410000000003220104b51f15780008b5af513eba230a505400000001010101010101010101010101010101000000100000000000000000000000000000000000fe0051515151515151515151515151000000fc0045444f31342e35350a2020202000000010000000000000000000000000000001f1702079020020000c940bd502450000000001180021001d030cfd07300c200880f1ea50e713ba39c20a0235547861406600402378260009070000000000400000220028ffbe0d802f0c1701d30003001f087f0079000000ffbe0d002f0c1701d30003001f081f091909000081000be3058000e6060501a07a070000000000aa90>
  |       "IODisplayPrefsKeyOld" = "IOService:/AppleACPIPlatformExpert/PC00/AppleACPIPCI/IGPU@2/.Display_boot/display0/AppleDisplay-148f-4179"
  |       "IODisplayPrefsKey" = "IOService:/AppleACPIPlatformExpert/PC00/AppleACPIPCI/IGPU@2-148f-4179-df95d6daaa7c17af"
  |       "IOProbeScore" = 2000
  |       "IOPowerManagement" = {"CapabilityFlags"=49152,"MaxPowerState"=3,"CurrentPowerState"=3}
  |       "IODisplayParameters" = {"bgsc"={"min"=0,"max"=65536,"value"=65536},"rgsc"={"min"=0,"max"=65536,"value"=65536},"vblm"={"min"=0,"max"=196608,"value"=65536},"ownr"=4294968576,"ggsc"={"min"=0,"max"=65536,"value"=65536}}
  |       "IOMatchCategory" = "IODefaultMatchCategory"
  |       "IOPersonalityPublisher" = "com.apple.iokit.IOGraphicsFamily"
  |       "IODisplayConnectFlags" = <00000000>
  |       "CFBundleIdentifierKernel" = "com.apple.iokit.IOGraphicsFamily"
  |       "IODisplayAttributes" = {"IODisplayAttributes"=<676174760000000073676c6600000000726c6f630100000063706220040000006364670080808000676c666400000000>}
  |       "DisplayVendorID" = 5263
  |       "DisplaySerialNumber" = SERIAL-1
  |     }
  |     
  +-o IOFramebufferSharedUserClient  <class IOFramebufferSharedUserClient, id 0x100000687, !registered, !matched, active, busy 0, retain 4>
  |   {
  |     "IOUserClientDefaultLocking" = Yes
  |     "IOUserClientSharedInstance" = Yes
  |     "IOUserClientCreator" = "pid 1956, nsattributedstri"
  |   }
  |   
  +-o IOFramebufferUserClient  <class IOFramebufferUserClient, id 0x1000008fa, !registered, !matched, active, busy 0, retain 5>
      {
        "IOUserClientCreator" = "pid 1425, WindowServer"
        "IOUserClientDefaultLocking" = Yes
      }
      
```

**Loaded kexts**

```text
  142    0 0xffffff7f968f9000 0x1e62     0x1e62     org.watchthelight.IGPUConsoleRange (0.4) UUID-1 <19 17 9 7 6 3 1>
  144    0 0xffffff7f96f1c000 0x2d05     0x2d05     org.watchthelight.XeGfxIntelFB (0.8) UUID-2 <127 125 19 6 3 1>
  161    0 0xffffff7f96ec9000 0x35c4     0x35c4     org.watchthelight.XeGfxAGDC (0.1.1) UUID-3 <136 7 6 3 1>
  162    0 0xffffff7f96f20000 0x6e0f     0x6e0f     org.xenative.XeNative (0.14) UUID-4 <9 7 6 3 1>
  179    0 0xffffff7f96903000 0x31d03e   0x31d03e   org.watchthelight.XeGfx (0.0.46) UUID-5 <20 19 9 7 6 3 1>
  180    0 0xffffff7f96f0b000 0xd844     0xd844     org.watchthelight.XeGfxDisplay (0.5) UUID-6 <125 19 7 6 3 1>
  182    0 0xffffff7f96ecf000 0x1c65d    0x1c65d    org.watchthelight.XeGfxAccel (0.1) UUID-7 <181 125 108 19 7 6 3 1>
```

**Bring-up log (three boots: two failed attempts, then the working one)**

```text
2026-11-01 19:18:09 accel already loaded
2026-11-01 19:18:14 bootArm is driving (debug.xegfxaccel_boot=1) -- not touching accelfb/iop, waiting for its steps
2026-11-01 19:20:21 boot_steps=0 (bit32 iop, bit64 flip)
2026-11-01 19:20:43 bootArm armed but no frames yet -- restarting WindowServer once
2026-11-01 19:21:37 === bringup start (boot=2026-11-01 19:21:30) ===
2026-11-01 19:22:05 console user=change48 (uid 501) after 25s
2026-11-01 19:22:07 GUI session + WindowServer up after 2s
2026-11-01 19:22:35 core loaded (debug.xegfx_stage present, attempt 1)
2026-11-01 19:22:36 stages ok: 0 1 1 1 1 1 1 1 (after 1s)
2026-11-01 19:22:37 accel already loaded
2026-11-01 19:22:41 bootArm is driving (debug.xegfxaccel_boot=1) -- not touching accelfb/iop, waiting for its steps
2026-11-01 19:24:48 boot_steps=0 (bit32 iop, bit64 flip)
2026-11-01 19:25:10 bootArm armed but no frames yet -- restarting WindowServer once
2026-11-01 19:25:27 WindowServer back (pid 2086)
2026-11-01 19:27:34 iop=1  iop_ok=0 (fail=0 empty=0)
2026-11-01 19:27:34 WARN: iop_ok did not advance (panel keeps the firmware frame)
2026-11-01 19:30:45 === bringup start (boot=2026-11-01 19:30:39) ===
2026-11-01 19:31:10 console user=change48 (uid 501) after 22s
2026-11-01 19:31:13 GUI session + WindowServer up after 2s
2026-11-01 19:31:54 core loaded (debug.xegfx_stage present, attempt 1)
2026-11-01 19:31:55 stages ok: 0 1 1 1 1 1 1 1 (after 1s)
2026-11-01 19:31:57 accel already loaded
2026-11-01 19:32:01 already live (debug.xegfxaccel_iop=1) -- skipping pipe + hooks, not re-delivering the mode change
2026-11-01 19:32:01 bootArm is driving (debug.xegfxaccel_boot=2) -- not touching accelfb/iop, waiting for its steps
2026-11-01 19:32:01 boot_steps=123 (bit32 iop, bit64 flip)
2026-11-01 19:32:23 bootArm armed but no frames yet -- restarting WindowServer once
2026-11-01 19:32:38 WindowServer back (pid 1425)
2026-11-01 19:32:42   sample 1: iop_ok 3 -> 10
2026-11-01 19:32:42 iop=1  iop_ok=10 (fail=0 empty=0)
2026-11-01 19:32:42 OK: frames are reaching the panel
```

**Accelerator log, last 5 minutes**

```text
Timestamp                       Thread     Type        Activity             PID    TTL  
2026-11-01 19:40:29.810131+0800 0x26d4     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: iop: trace surf 0x5 frame 72410 copy 3959 vblank 7277 txn 10830
2026-11-01 19:40:29.810141+0800 0x26d4     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: iop: trace surf 0x5 frame 72410 copy 3959 vblank 7277 txn 10830
2026-11-01 19:42:20.750904+0800 0x59f5     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: getStampMemory(<private>) -> descriptor <private> (4096 stamps, 16 KB)
2026-11-01 19:42:20.750915+0800 0x59f5     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: getStampMemory(<private>) -> descriptor <private> (4096 stamps, 16 KB)
2026-11-01 19:42:20.751162+0800 0x59f5     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: createUserGPUTask -> <private> init=1
2026-11-01 19:42:20.751166+0800 0x59f5     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: createUserGPUTask -> <private> init=1
2026-11-01 19:42:33.007393+0800 0x5aa7     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: getStampMemory(<private>) -> descriptor <private> (4096 stamps, 16 KB)
2026-11-01 19:42:33.007403+0800 0x5aa7     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: getStampMemory(<private>) -> descriptor <private> (4096 stamps, 16 KB)
2026-11-01 19:42:33.007750+0800 0x5aa7     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: createUserGPUTask -> <private> init=1
2026-11-01 19:42:33.007754+0800 0x5aa7     Default     0x0                  0      0    kernel: (XeGfxAccel) NMXAccel: createUserGPUTask -> <private> init=1
```

**Hardware**

```text
Hardware:

    Hardware Overview:

      Model Name: MacBook Pro
      Model Identifier: MacBookPro16,2
      Processor Name: Intel(R) Core(TM) Ultra 7 255H
      Processor Speed: 3.68 GHz
      Number of Processors: 1
      Total Number of Cores: 16
      L2 Cache (per Core): 3 MB
      L3 Cache: 27.4 MB
      Memory: 32 GB
      System Firmware Version: 2094.80.5.0.0
      OS Loader Version: 583~5707
      Serial Number (system): REDACTED
      Hardware UUID: REDACTED
      Provisioning UDID: REDACTED

Graphics/Displays:

    Intel Arrow Lake Xe-LPG+:

      Chipset Model: Intel Arrow Lake Xe-LPG+
      Type: GPU
      Bus: Built-In
      VRAM (Dynamic, Max): 24576 MB
      Vendor: Intel
      Device ID: 0x7d51
      Revision ID: 0x0003
      Metal Support: Metal 3
      Displays:
        EDO14.55:
          Resolution: 3120 x 2080
          UI Looks like: 1560 x 1040 @ 120.00Hz
          Framebuffer Depth: 24-Bit Color (ARGB8888)
          Main Display: Yes
          Mirror: Off
          Online: Yes
```

**macOS and OpenCore version**

```text
ProductName:		macOS
ProductVersion:		26.5.2
BuildVersion:		25F84
4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version	DBG-108-2026-09-27
```

**Panic report on this machine (2026-10-31 22:51, the frames that matter)**

```text
panic(cpu 4 caller 0xffffff80100d459a): Kernel trap at 0xffffff7fa6ae015e, type = 14=page fault, registers:
Panicked task 0xffffff952b6469a0: 275 threads: pid 0: kernel_task
Backtrace (CPU 4), panicked thread: 0xffffff905ecc94f8, Frame : Return Address
0xffffffe5706cccd0 : 0xffffff7fa6ae015e org.watchthelight.XeGfxAccel : __ZL19nvAccelResidentCallPKcPvS1_S1_S1_ + 0x7e
0xffffffe5706cce00 : 0xffffff7fa6adfebe org.watchthelight.XeGfxAccel : __ZL16nvAccelXeFlipArmP9IOSurfacejjjb + 0x2ae
0xffffffe5706cce60 : 0xffffff7fa6adfac4 org.watchthelight.XeGfxAccel : __ZL21nvAccelXeFlipDeferRunPvS_ + 0xc4
      Kernel Extensions in backtrace:
         org.watchthelight.XeGfxAccel(0.1)[UUID-9]@0xffffff7fa6acf000->0xffffff7fa6aebfff
            dependency: com.apple.iokit.IOAcceleratorFamily2(487.4.3)[UUID-10]@0xffffff7fa5023000->0xffffff7fa508cfff
            dependency: com.apple.iokit.IOGraphicsFamily(600)[UUID-11]@0xffffff7fa51a2000->0xffffff7fa51d0fff
            dependency: com.apple.iokit.IOPCIFamily(2.9)[UUID-12]@0xffffff80125d8000->0xffffff801262bfff
            dependency: com.apple.iokit.IOSurface(393.5.7)[UUID-13]@0xffffff80127b0000->0xffffff80127dcfff
Boot args: -v keepsyms=1 -vi2c-force-polling -amfipassbeta amfi_get_out_of_my_way=0x1 amfi=0x80 ipc_control_port_options=0 itlwm_cc=CN gfxrst=4 msgbuf=1048576 xegfxaccelboot=1
Mac OS version:
System model name: MacBookPro16,2 (Mac-5F9802EFE386AA28)
last started kext at 68237311319: org.watchthelight.XeGfxAccel	0.1 (addr 0xffffff7fa6acf000, size 118784)
org.watchthelight.XeGfxAccel	0.1
```


### Publishing

- [X] I agree that these logs are published in this repository, redacted, under CC BY 4.0
