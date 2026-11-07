import sys
p=sys.argv[1]; s=open(p).read()
if "static VkComponentMapping nvmtl_a8_mapping" in s:
    print("already present"); raise SystemExit
i=s.find("nvmtl_a8_mapping(")
if i<0: raise SystemExit("no use found")
# 回退到上一个顶层 } 之后插入
j=s.rfind("\n}\n", 0, i)
if j<0: raise SystemExit("no anchor")
ins=("\n// The a8 flag of a pixel format selects a fixed swizzle of its view: 1 = A8Unorm (alpha in the red lane),\n"
     "// 2 = Apple's private ARGB8 format 84 (bytes A,R,G,B stored as RGBA8, so R<-G, G<-B, B<-A, A<-R).\n"
     "// 0 = identity.  (Ported from the v2.6.6 plugin: our tree lost the definition with the host-pages hunks.)\n"
     "static VkComponentMapping nvmtl_a8_mapping(int a8)\n{\n"
     "    if (a8 == 1) return (VkComponentMapping){ VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };\n"
     "    if (a8 == 2) return (VkComponentMapping){ VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R };\n"
     "    return (VkComponentMapping){ 0, 0, 0, 0 };\n}\n")
s=s[:j+3]+ins+s[j+3:]
open(p,"w").write(s); print("inserted")
