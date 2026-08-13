// [stol:infra-crash-diag-module-map]
// Emulator (darwin) implementation of the crash-diagnostics module enumerator.
//
// Same role as the linux dl_iterate_phdr walk (arch/linux/hal/dynload.cpp), but
// macOS has no dl_iterate_phdr: enumerate dyld's image list instead and derive
// the extents from Mach-O segment load commands. The main executable is image 0
// and is labeled "kernel" so the report format is identical across arches.

// The macOS 26 SDK's mach/message.h (reached via <mach-o/loader.h>) asserts
// struct sizes with _Static_assert, a C keyword clang also honors in C++ but
// gcc does not — map it to C++'s static_assert before any mach header lands.
#if defined(__cplusplus) && !defined(__clang__)
#define _Static_assert static_assert
#endif

#include <hal/modulemap.h>

// <mach-o/dyld.h> drags in even more of mach/*.h than loader.h does; declare
// the four dyld enumeration entry points ourselves instead (stable ABI since
// macOS 10.1) and take only the Mach-O structs from loader.h.
#include <mach-o/loader.h>
#include <stdint.h>
#include <string.h>

extern "C"
{
  uint32_t _dyld_image_count(void);
  const struct mach_header *_dyld_get_image_header(uint32_t image_index);
  intptr_t _dyld_get_image_vmaddr_slide(uint32_t image_index);
  const char *_dyld_get_image_name(uint32_t image_index);
}

namespace
{
  // Mirrors the linux convention: the symbolication base is the LOAD BIAS
  // (dyld's slide), not slide + segment vmaddr, so the offline symbolizer's
  //     offset = capturedAddr - textBase
  // recovers the link-time vaddr. The containment extent therefore runs from
  // the slide to the END of the highest exec segment measured from the slide,
  // i.e. textSize = vmaddr + vmsize.
  void describeImage(const struct mach_header *hdr, intptr_t slide,
                     const char *name, std::vector<od::ModuleInfo> &out,
                     bool isMain)
  {
    od::ModuleInfo m;
    m.path = (isMain || name == nullptr || name[0] == '\0') ? "kernel" : name;
    m.textBase = 0;
    m.textSize = 0;
    m.dataBase = 0;
    m.dataSize = 0;

    if (hdr == nullptr || hdr->magic != MH_MAGIC_64)
    {
      out.push_back(m);
      return;
    }

    const struct mach_header_64 *hdr64 = (const struct mach_header_64 *)hdr;
    const uint8_t *cmd = (const uint8_t *)(hdr64 + 1);
    bool haveText = false;
    bool haveData = false;
    uintptr_t textEnd = 0;   // max (vmaddr + vmsize) over exec segments
    uintptr_t dataVaddr = 0; // lowest writable vmaddr
    size_t dataMemsz = 0;

    for (uint32_t i = 0; i < hdr64->ncmds; i++)
    {
      const struct load_command *lc = (const struct load_command *)cmd;
      if (lc->cmd == LC_SEGMENT_64)
      {
        const struct segment_command_64 *seg =
            (const struct segment_command_64 *)lc;
        const uintptr_t vaddr = (uintptr_t)seg->vmaddr;
        const uintptr_t end = vaddr + (uintptr_t)seg->vmsize;
        if (seg->initprot & VM_PROT_EXECUTE)
        {
          if (!haveText || end > textEnd)
          {
            textEnd = end;
          }
          haveText = true;
        }
        else if ((seg->initprot & VM_PROT_WRITE) &&
                 strcmp(seg->segname, "__PAGEZERO") != 0)
        {
          if (!haveData || vaddr < dataVaddr)
          {
            dataVaddr = vaddr;
            dataMemsz = (size_t)seg->vmsize;
          }
          haveData = true;
        }
      }
      cmd += lc->cmdsize;
    }

    const uintptr_t loadBias = (uintptr_t)slide;
    if (haveText)
    {
      m.textBase = loadBias;        // == symbolizer base (the slide)
      m.textSize = (size_t)textEnd; // range [slide .. slide+vmaddr+vmsize)
    }
    if (haveData)
    {
      m.dataBase = loadBias + dataVaddr;
      m.dataSize = dataMemsz;
    }

    out.push_back(m);
  }
}

namespace od
{
  void enumerateModules(std::vector<ModuleInfo> &out)
  {
    out.clear();
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++)
    {
      describeImage(_dyld_get_image_header(i),
                    _dyld_get_image_vmaddr_slide(i),
                    _dyld_get_image_name(i), out, i == 0);
    }

    // dyld lists the main program first already, but guarantee a "kernel"
    // entry exists even in the degenerate case.
    bool haveKernel = false;
    for (auto &m : out)
    {
      if (m.path == "kernel")
      {
        haveKernel = true;
        break;
      }
    }
    if (!haveKernel)
    {
      ModuleInfo kernel;
      kernel.path = "kernel";
      kernel.textBase = 0;
      kernel.textSize = 0;
      kernel.dataBase = 0;
      kernel.dataSize = 0;
      out.insert(out.begin(), kernel);
    }
  }
}
