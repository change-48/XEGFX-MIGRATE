import sys,re
ph=sys.argv[1]
def edit(path, pairs):
    s=open(path).read()
    for old,new,label in pairs:
        if old not in s: raise SystemExit("MISSING %s in %s"%(label,path))
        s=s.replace(old,new,1)
    open(path,"w").write(s)

# 1) header: the field
edit(ph+"/NVMTLObjects.h", [(
 "  nvk_buffer _hostBuf; int8_t _hostState;",
 "  nvk_buffer _hostPages;   // the surface's pages imported once for the host copies and reused while they hold\n"
 "  nvk_buffer _hostBuf; int8_t _hostState;", "field")])

# 2) prototype + drop
edit(ph+"/nvmtl_vk.h", [(
 "int  nvmtl_vk_image_write_host(nvk_queue *q, nvk_image *img, void *base, size_t row_bytes, size_t bytes);",
 "int  nvmtl_vk_image_write_host(nvk_queue *q, nvk_image *img, void *base, size_t row_bytes, size_t bytes, nvk_buffer *cache);\n"
 "void nvmtl_vk_host_pages_drop(nvk_buffer *cache);", "proto")])

# 3) call site
edit(ph+"/NVMTLObjects.m", [(
 "nvmtl_vk_image_write_host(r->_q, &r->_i, base, row, IOSurfaceGetAllocSize(r->_surf) - off);",
 "nvmtl_vk_image_write_host(r->_q, &r->_i, base, row, IOSurfaceGetAllocSize(r->_surf) - off, &r->_hostPages);", "call")])
print("edits ok")
