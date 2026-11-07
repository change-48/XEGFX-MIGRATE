import sys
p = sys.argv[1]
s = open(p).read()
old = "- (id<MTLFunction>)newFunctionWithName:(NSString *)name {"
new = """// A-64: CoreAnimation asks for the same function over and over, and every answer rebuilt it: the SPIR-V text is
// cached per library, but the lowering still ran per pipeline and the profile is dominated by it.  A function is
// immutable, so hand back the one already built for this name.
static char kFnCacheKey;
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    if (!name) return [self nvmtlBuildFunctionNamed:name];
    NSMutableDictionary *c = objc_getAssociatedObject(self, &kFnCacheKey);
    if (!c) { c = [NSMutableDictionary dictionary]; objc_setAssociatedObject(self, &kFnCacheKey, c, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    @synchronized (c) {
        id<MTLFunction> hit = c[name];
        if (hit) return hit;
        id<MTLFunction> made = [self nvmtlBuildFunctionNamed:name];
        if (made && c.count < 512) c[name] = made;
        return made;
    }
}
- (id<MTLFunction>)nvmtlBuildFunctionNamed:(NSString *)name {"""
if old not in s: raise SystemExit("ANCHOR MISSING")
open(p,"w").write(s.replace(old,new,1)); print("patched")
