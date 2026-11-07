import sys
p = sys.argv[1]
s = open(p).read()

def rep(old, new, label):
    global s
    if old not in s:
        raise SystemExit("ANCHOR MISSING: " + label)
    s = s.replace(old, new, 1)

# 1) off by default: a normal boot must stay exactly as it is.  Testing uses -xeintelfb1x=2.
rep("    int  fScale1x = 2;                  // 0 off, 1 quarter desktop, 2 quarter desktop + pipe scaler",
    "    int  fScale1x = 0;                  // 0 off (normal boot), 1 quarter desktop, 2 quarter desktop + scaler",
    "default level")

# 2) rewrite the mode list CoreDisplay actually reads
rep("    void xifbPublishDetailedTimings() {",
    '''    // A-60: CoreDisplay builds its mode list from IOFBConfig, not from the query overrides.  Its DM word
    // carries hact=3120, so the compositor sized its backing at 3120 no matter what getPixelInformation
    // answered -- and the plane then read a 1560-wide picture out of a 3120-wide buffer, which is exactly the
    // torn, half-black screen A-58 and A-59 produced.  Rewrite that one entry instead: DM takes the smaller
    // active size, TM keeps the panel's totals with the picture scaled up to its 3120x2080.
    void xifbPublishConfig1560() {
        if (!fTValid) { XIFBLOG("1x: no measured timing, IOFBConfig left alone"); return; }
        OSDictionary *cfg = OSDynamicCast(OSDictionary, getProperty("IOFBConfig"));
        if (!cfg) { XIFBLOG("1x: no IOFBConfig to rewrite"); return; }
        OSArray *modes = OSDynamicCast(OSArray, cfg->getObject("IOFBModes"));
        if (!modes || modes->getCount() < 1) { XIFBLOG("1x: IOFBConfig carries no modes"); return; }
        OSDictionary *m0 = OSDynamicCast(OSDictionary, modes->getObject(0));
        if (!m0) { XIFBLOG("1x: IOFBConfig mode 0 is not a dictionary"); return; }
        const OSData *dm = OSDynamicCast(OSData, m0->getObject("DM"));
        if (!dm || dm->getLength() < 8) { XIFBLOG("1x: no DM blob to patch"); return; }

        unsigned char dmBytes[64];
        unsigned dn = dm->getLength(); if (dn > sizeof dmBytes) dn = sizeof dmBytes;
        memcpy(dmBytes, dm->getBytesNoCopy(), dn);
        *(UInt32 *)(void *)(dmBytes + 0) = XIFB_1X_W;      /* hact */
        *(UInt32 *)(void *)(dmBytes + 4) = XIFB_1X_H;      /* vact */

        IODetailedTimingInformationV2 t;
        xifbFillDetailedTiming(t);
        t.horizontalActive   = XIFB_1X_W;
        t.horizontalBlanking = fT[T_HTOT] - XIFB_1X_W;
        t.horizontalScaled   = fT[T_HVIS];
        t.verticalActive     = XIFB_1X_H;
        t.verticalBlanking   = fT[T_VTOT] - XIFB_1X_H;
        t.verticalScaled     = fT[T_VVIS];

        OSData *dmNew = OSData::withBytes(dmBytes, dn);
        OSData *tmNew = OSData::withBytes(&t, sizeof t);
        OSDictionary *entry = dmNew && tmNew ? OSDictionary::withDictionary(m0) : nullptr;
        OSArray *list = entry ? OSArray::withCapacity(1) : nullptr;
        OSDictionary *cfgNew = list ? OSDictionary::withDictionary(cfg) : nullptr;
        if (!cfgNew) {
            XIFBLOG("1x: out of memory building the config");
            if (cfgNew) cfgNew->release();
            if (list) list->release();
            if (entry) entry->release();
            if (dmNew) dmNew->release();
            if (tmNew) tmNew->release();
            return;
        }
        entry->setObject("DM", dmNew);
        entry->setObject("TM", tmNew);
        list->setObject(entry);
        cfgNew->setObject("IOFBModes", list);
        const bool ok = setProperty("IOFBConfig", cfgNew);
        XIFBLOG("1x: IOFBConfig rewritten: active %ux%u, scaled %ux%u, %u kHz (ok %d)", XIFB_1X_W, XIFB_1X_H,
                (unsigned)fT[T_HVIS], (unsigned)fT[T_VVIS], (unsigned)(fT[T_PCLK] / 1000), (int)ok);
        cfgNew->release(); list->release(); entry->release(); dmNew->release(); tmNew->release();
        setProperty("IOFBNeedsRefresh", kOSBooleanTrue);
    }
    void xifbPublishDetailedTimings() {''',
    "config rewrite")

# 3) call it, and keep the detailed timings in step
rep('''        if (started && xifbScale1xOffered()) {
            XIFBLOG("1x: base mode %#x answers %ux%u, taking the plane and the scaler", (unsigned)fBaseMode, XIFB_1X_W, XIFB_1X_H);
            xifbApplyScale1x(fScale1x);
        }''',
    '''        if (started && xifbScale1xOffered()) {
            XIFBLOG("1x: base mode %#x answers %ux%u, rewriting the mode list and taking the plane", (unsigned)fBaseMode, XIFB_1X_W, XIFB_1X_H);
            if (fScale1x >= 2) { xifbPublishConfig1560(); xifbPublishDetailedTimings(); }
            xifbApplyScale1x(fScale1x);
        }''',
    "call site")

open(p, "w").write(s)
print("patched")
