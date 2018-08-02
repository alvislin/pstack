#include <features.h>

#define REGMAP(a,b)
#include "libpstack/dwarf/archreg.h"
#include "libpstack/dwarf.h"
#include "libpstack/proc.h"
#include "libpstack/ps_callback.h"

#include <link.h>
#include <unistd.h>

#include <cassert>
#include <climits>

#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sys/ucontext.h>

static size_t gMaxFrames = 1024; /* max number of frames to read */

void
PstackOptions::operator += (PstackOption opt)
{
    values.set(opt);
}

void
PstackOptions::operator -= (PstackOption opt)
{
    values.reset(opt);
}

bool
PstackOptions::operator () (PstackOption opt) const
{
    return values[opt];
}

Process::Process(Elf::Object::sptr exec, Reader::csptr memory,
                  const PathReplacementList &prl, Dwarf::ImageCache &cache)
    : entry(0)
    , interpBase(0)
    , isStatic(false)
    , vdsoBase(0)
    , agent(nullptr)
    , execImage(std::move(exec))
    , pathReplacements(prl)
    , sysent(0)
    , imageCache(cache)
    , io(std::move(memory))
{
    if (exec)
        entry = exec->getHeader().e_entry;
}

void
Process::load(const PstackOptions &options)
{
    /*
     * Attach the executable and any shared libs.
     * The process is still running here, but unless its actively loading or
     * unload a shared library, this relatively safe, and saves us a lot of
     * work while the process is stopped.
     */

    if (!execImage)
        throw (Exception() << "no executable image located for process");

    Elf::Addr r_debug_addr = findRDebugAddr();
    isStatic = r_debug_addr == 0 || r_debug_addr == Elf::Addr(-1);
    if (isStatic)
        addElfObject(execImage, 0);
    else
        loadSharedObjects(r_debug_addr);

    if (!options(PstackOptions::nothreaddb)) {
        td_err_e the;
        the = td_ta_new(this, &agent);
        if (the != TD_OK) {
            agent = nullptr;
            if (verbose > 0 && the != TD_NOLIBTHREAD)
                *debug << "failed to load thread agent: " << the << std::endl;
        }
    }

}

Dwarf::Info::sptr
Process::getDwarf(Elf::Object::sptr elf)
{
    return imageCache.getDwarf(elf);
}

void
Process::processAUXV(const Reader &auxio)
{
    for (size_t i = 0;; i++) {
        Elf::auxv_t aux;
        try {
            auxio.readObj(i * sizeof aux, &aux);
        }
        catch (const Exception &ex) {
            break;
        }

        Elf::Addr hdr = aux.a_un.a_val;
        switch (aux.a_type) {
            case AT_ENTRY: {
                // this provides a reference for relocating the executable when
                // compared to the entrypoint there.
                entry = hdr;
                break;
            }
            case AT_SYSINFO: {
                sysent = aux.a_un.a_val;
                break;
            }
            case AT_SYSINFO_EHDR: {
                try {
                    auto elf = std::make_shared<Elf::Object>(imageCache, std::make_shared<OffsetReader>(io, hdr, 65536));
                    vdsoBase = hdr;
                    addElfObject(elf, hdr);
                    if (verbose >= 2)
                        *debug << "VDSO " << *elf->io << " loaded at " << std::hex << hdr << "\n";

                }
                catch (const std::exception &ex) {
                    std::clog << "warning: failed to load DSO: " << ex.what() << "\n";
                }
                break;
            }
            case AT_BASE:
                interpBase = hdr;
                break;
#ifdef AT_EXECFN
            case AT_EXECFN:
                auto exeName = io->readString(hdr);
                if (verbose >= 2)
                    *debug << "filename from auxv: " << exeName << "\n";
                if (!execImage) {
                    execImage = imageCache.getImageForName(exeName);
                    if (entry == 0)
                       entry = execImage->getHeader().e_entry;
                }

                break;
#endif
        }
    }
}

template <typename ctx>
std::ostream &
operator << (std::ostream &os, const JSON<td_thr_type_e, ctx> &jt)
{
    switch (jt.object) {
        case TD_THR_ANY_TYPE: return os << json("TD_THR_ANY_TYPE");
        case TD_THR_USER: return os << json("TD_THR_USER");
        case TD_THR_SYSTEM: return os << json("TD_THR_SYSTEM");
        default: return os << json("unknown type");
    }
}

std::ostream &
operator << (std::ostream &os, const JSON<Dwarf::StackFrame *, Process *> &jt)
{
    auto &frame =jt.object;
    auto proc = jt.context;

    JObject jo(os);

    Elf::Addr objIp = 0;
    Elf::Object::sptr obj;
    Elf::Sym sym;
    std::string fileName;
    std::string symName = "unknown";
    if (frame->ip == proc->sysent) {
        symName = "(syscall)";
    } else {
        Elf::Off loadAddr = 0;
        obj = proc->findObject(frame->ip, &loadAddr);
        if (obj) {
            fileName = stringify(*obj->io);
            objIp = frame->ip - loadAddr;
            obj->findSymbolByAddress(objIp, STT_FUNC, sym, symName);
        }
    }

    jo.field("ip", frame->ip);
    if (symName != "")
        jo.field("function", symName);

    if (obj) {
        jo.field("off", objIp - sym.st_value)
            .field("file", fileName);
        const auto &di = proc->getDwarf(obj);
        if (di) {
            auto src = di->sourceFromAddr(objIp - 1);
            jo.field("source", src);
        }
    }
    return os;
}

std::ostream &
operator << (std::ostream &os, const JSON<ThreadStack, Process *> &ts)
{
    return JObject(os)
        .field("ti_tid", ts->info.ti_tid)
        .field("ti_type", ts->info.ti_type)
        .field("ti_stack", ts->stack, ts.context);
}

struct ArgPrint {
    const Process &p;
    const struct Dwarf::StackFrame *frame;
    ArgPrint(const Process &p_, const Dwarf::StackFrame *frame_) : p(p_), frame(frame_) {}
};

struct RemoteValue {
    const Process &p;
    const Elf::Addr addr;
    const Dwarf::DIE *type;
    RemoteValue(const Process &p_, Elf::Addr addr_, const Dwarf::DIE *type_)
        : p(p_)
        , addr(addr_)
        , type(type_)
    {}
};

std::ostream &
operator << (std::ostream &os, const RemoteValue &rv)
{
    using namespace Dwarf;
    if (rv.addr == 0)
       return os << "(null)";
    auto type = rv.type;
    while (type->type->tag == DW_TAG_typedef || type->type->tag == DW_TAG_const_type)
       type = type->referencedEntry(DW_AT_type);


    uintmax_t size;
    Attribute sizeAttr;
    std::vector<char> buf;
    if (type->attrForName(DW_AT_byte_size, sizeAttr)) {
        size = uintmax_t(sizeAttr);
        buf.resize(size);
        auto rc = rv.p.io->read(rv.addr, size, &buf[0]);
        if (rc != size) {
            return os << "<error reading " << size << " bytes from " << rv.addr << ", got " << rc << ">";
        }
    } else {
       size = 0;
    }

    IOFlagSave _(os);
    switch (type->type->tag) {
        case DW_TAG_base_type: {
            if (size == 0) {
                os << "unrepresentable(1)";
            }
            Attribute encoding;
            if (!type->attrForName(DW_AT_encoding, encoding))
                throw (Exception() << "no encoding specified for base type");

            union {
               int8_t *int8;
               int16_t *int16;
               int32_t *int32;
               int64_t *int64;
               void **voidp;
               char *cp;
            } u;
            u.cp = &buf[0];

            switch (uintmax_t(encoding)) {
                case DW_ATE_address:
                    os << *u.voidp;
                    break;
                case DW_ATE_boolean:
                    for (size_t i = 0;; ++i) {
                        if (i == size) {
                            os << "false";
                            break;
                        }
                        if (buf[i] != 0) {
                            os << "true";
                            break;
                        }
                    }
                    break;

                case DW_ATE_signed:
                case DW_ATE_signed_char:
                    switch (size) {
                        case sizeof (int8_t):
                            os << *u.int8;
                            break;
                        case sizeof (int16_t):
                            os << *u.int16;
                            break;
                        case sizeof (int32_t):
                            os << *u.int32;
                            break;
                        case sizeof (int64_t):
                            os << *u.int64;
                            break;
                    }
                    break;

                case DW_ATE_unsigned:
                case DW_ATE_unsigned_char:
                    switch (size) {
                        case sizeof (uint8_t):
                            os << *u.int8;
                            break;
                        case sizeof (uint16_t):
                            os << *u.int16;
                            break;
                        case sizeof (uint32_t):
                            os << *u.int32;
                            break;
                        case sizeof (uint64_t):
                            os << *u.int64;
                            break;
                        default:
                            abort();
                    }
                    break;

                default:
                    abort();
            }
            break;
        }
        case DW_TAG_reference_type:
        case DW_TAG_pointer_type: {
            if (size == 0) {
               buf.resize(sizeof (void *));
               rv.p.io->read(rv.addr, sizeof (void **), &buf[0]);
            }
            auto remote = Elf::Addr(*(void **)&buf[0]);
            auto base = type->referencedEntry(DW_AT_type);
            if (base && base->name() == "char") {
               std::string s = rv.p.io->readString(remote);
               os << "\"" << s << "\"";
            } else {
               os << (void *)remote;
            }
            break;
        }
        default:
            os << "<unprintable type " << type->type->tag << ">";
    }
    return os;
}

std::ostream &
operator << (std::ostream &os, const ArgPrint &ap)
{
    using namespace Dwarf;
    const char *sep = "";
    for (auto &child : ap.frame->function->children) {
        switch (child.type->tag) {
            case DW_TAG_formal_parameter: {
                auto name = child.name();
                const Dwarf::DIE *type = child.referencedEntry(DW_AT_type);
                Elf::Addr addr = 0;
                os << sep << name;
                if (type != nullptr) {
                    Dwarf::Attribute attr;

                    if (child.attrForName(Dwarf::DW_AT_location, attr)) {
                        Dwarf::ExpressionStack fbstack;
                        addr = fbstack.eval(ap.p, attr, ap.frame, ap.frame->elfReloc);
                        os << "=";
                        if (fbstack.isReg) {
                           os << std::hex << addr << std::dec << "{r" << fbstack.inReg << "}";
                        } else {
                           os << RemoteValue(ap.p, addr, type);
                        }
                    } else if (child.attrForName(Dwarf::DW_AT_const_value, attr)) {
                        os << "=" << intmax_t(attr);
                    }
                }
                sep = ", ";
                break;
            }
            default:
                break;
        }
    }
    return os;
}

std::ostream &
Process::dumpStackText(std::ostream &os, const ThreadStack &thread, const PstackOptions &options)
{
    os << std::dec;
    os << "thread: " << (void *)thread.info.ti_tid << ", lwp: "
       << thread.info.ti_lid << ", type: " << thread.info.ti_type << "\n";
    int frameNo = 0;
    for (auto frame : thread.stack) {

        {
            IOFlagSave _(os);
            os << "#" << std::left << std::dec << std::setw(2) << std::setfill(' ') << frameNo++ << " ";
            os << std::right << std::hex << "0x" << std::setw(ELF_BITS/4) << std::setfill('0') << frame->ip;
            if (verbose > 0)
                os << "/" << std::hex << std::setw(ELF_BITS/4) << std::setfill('0') << frame->cfa;
            os << " ";
        }

        Elf::Sym sym;
        std::string fileName = "unknown file";
        std::string symName;

        Elf::Off loadAddr;
        auto obj = findObject(frame->ip, &loadAddr);
        if (obj) {
            fileName = stringify(*obj->io);
            Elf::Addr objIp = frame->ip - loadAddr;

            Dwarf::Info::sptr dwarf = getDwarf(obj);
            std::list<Dwarf::Unit::sptr> units;
            if (dwarf->hasRanges()) {
                for (const auto &rangeset : dwarf->ranges()) {
                    for (const auto range : rangeset.ranges) {
                        if (objIp >= range.start && objIp <= range.start + range.length) {
                            units.push_back(dwarf->getUnit(rangeset.debugInfoOffset));
                            break;
                        }
                    }
                }
            } else {
                // no ranges - try each dwarf unit in turn. (This seems to happen for single-unit exe's only, so it's no big loss)
                units = dwarf->getUnits();
            }

            std::string sigmsg = frame->cie != nullptr && frame->cie->isSignalHandler ?  "[signal handler called]" : "";
            Dwarf::Unit::sptr dwarfUnit;
            for (const auto &u : units) {
                // find the DIE for this function
                for (auto &it : u->entries) {
                    const Dwarf::DIE *de = Dwarf::findEntryForFunc(objIp, it);
                    if (de != nullptr) {
                        symName = de->name();
                        if (symName == "") {
                            obj->findSymbolByAddress(objIp, STT_FUNC, sym, symName);
                            if (symName != "")
                                symName += "%"; // mark the lack of a dwarf symbol.
                            else if (sigmsg == "")
                                symName = "<unknown>";
                        }
                        frame->function = de;
                        frame->dwarf = dwarf; // hold on to 'de'
                        os << "in " << symName << sigmsg;
                        Dwarf::Attribute lowpc;
                        if (de->attrForName(Dwarf::DW_AT_low_pc, lowpc))
                            os << "+" << objIp - uintmax_t(lowpc);
                        os << "(";
                        if (options(::PstackOptions::doargs)) {
                            os << ArgPrint(*this, frame);
                        }
                        os << ")";
                        dwarfUnit = u;
                        break;
                    }
                }
                if (dwarfUnit)
                    break;
            }

            if (!dwarfUnit) {
                obj->findSymbolByAddress(objIp, STT_FUNC, sym, symName);
                if (symName != "" || sigmsg != "")
                    os << "in " << symName << sigmsg << "!+" << objIp - sym.st_value << "()";
                else
                    os << "in <unknown>" << sigmsg << "()";
            }

            os << " at " << fileName;
            if (!options(PstackOptions::nosrc) && dwarf) {
                auto source = dwarf->sourceFromAddr(objIp - 1);
                for (auto ent : source)
                    os << " at " << ent.first << ":" << std::dec << ent.second;
            }
        } else {
            os << "no information for frame";
        }
        os << "\n";
    }
    return os;
}

void
Process::addElfObject(Elf::Object::sptr obj, Elf::Addr load)
{
    objects.push_back(LoadedObject(load, obj));
    if (verbose >= 2) {
        IOFlagSave _(*debug);
        *debug << "object " << *obj->io << " loaded at address " << std::hex << load << std::endl;
    }
}

/*
 * Grovel through the rtld's internals to find any shared libraries.
 */
void
Process::loadSharedObjects(Elf::Addr rdebugAddr)
{

    struct r_debug rDebug;
    io->readObj(rdebugAddr, &rDebug);

    /* Iterate over the r_debug structure's entries, loading libraries */
    struct link_map map;
    for (auto mapAddr = Elf::Addr(rDebug.r_map); mapAddr != 0; mapAddr = Elf::Addr(map.l_next)) {
        io->readObj(mapAddr, &map);

        // If we've loaded the VDSO, and we see it in the link map, just skip it.
        if (map.l_addr == vdsoBase)
           continue;
        // If we see the executable, just add it in and avoid going through the path replacement work
        if (mapAddr == Elf::Addr(rDebug.r_map)) {
            assert(map.l_addr == entry - execImage->getHeader().e_entry);
            addElfObject(execImage, map.l_addr);
            continue;
        }

        // Read the path to the file
        if (map.l_name == 0)
            continue;

        std::string path = io->readString(Elf::Off(map.l_name));
        if (path == "")
            continue;

        std::string startPath = path;
        for (auto &it : pathReplacements) {
            size_t found = path.find(it.first);
            if (found != std::string::npos)
                path.replace(found, it.first.size(), it.second);
        }
        if (verbose > 0 && path != startPath)
            *debug << "replaced " << startPath << " with " << path << std::endl;

        try {
            addElfObject(imageCache.getImageForName(path), Elf::Addr(map.l_addr));
        }
        catch (const std::exception &e) {
            std::clog << "warning: can't load text for '" << path << "' at " <<
            (void *)mapAddr << "/" << (void *)map.l_addr << ": " << e.what() << "\n";
            continue;
        }
    }
}

Elf::Addr
Process::findRDebugAddr()
{
    /*
     * Calculate the address the executable was loaded at - we know the entry
     * supplied by the kernel, and also the executable's desired entrypoint -
     * the difference is the load address.
     */
    Elf::Off loadAddr = entry - execImage->getHeader().e_entry;

    // Find DT_DEBUG in the process's dynamic section.
    for (auto &segment : execImage->getSegments(PT_DYNAMIC)) {
        // Read from the process, not the executable - the linker will have updated the content.
        OffsetReader dynReader(io, segment.p_vaddr + loadAddr, segment.p_filesz);
        ReaderArray<Elf::Dyn> dynamic(dynReader);
        for (auto dyn : dynamic)
            if (dyn.d_tag == DT_DEBUG)
                return dyn.d_un.d_ptr;
    }
    /*
     * If there's no DT_DEBUG, we've probably got someone executing a shared
     * library, which doesn't have an _r_debug symbol. Use the address of
     * _r_debug in the interpreter
     */
    if (interpBase && execImage->getInterpreter() != "") {
        try {
            addElfObject(imageCache.getImageForName(execImage->getInterpreter()), interpBase);
            return findSymbolByName(execImage->getInterpreter().c_str(), "_r_debug");
        }
        catch (...) {
        }
    }
    return 0;
}

Elf::Object::sptr
Process::findObject(Elf::Addr addr, Elf::Off *loadAddr) const
{
    for (auto &candidate : objects) {
        for (auto &phdr : candidate.object->getSegments(PT_LOAD)) {
            Elf::Off objAddr = addr - candidate.loadAddr;
            if (objAddr >= phdr.p_vaddr && objAddr < phdr.p_vaddr + phdr.p_memsz) {
                *loadAddr = candidate.loadAddr;
                return candidate.object;
            }
        }
    }
    return 0;
}

Elf::Addr
Process::findSymbolByName(const char *objName, const char *symbolName) const
{
    if (isStatic) // static exe: ignore object name.
        objName = 0;
    for (auto &loaded : objects) {
        if (objName != 0) {
            auto objname = stringify(*loaded.object->io);
            if (objname != std::string(objName)) {
               auto p = objname.rfind('/');
               if (p != std::string::npos)
                   objname = objname.substr(p + 1, std::string::npos);
               if (objname != std::string(objName))
                   continue;
            }
        }
        Elf::Sym sym;
        if (loaded.object->findSymbolByName(symbolName, sym))
            return sym.st_value + loaded.loadAddr;
        if (objName)
            break;
    }
    Exception e;
    e << "symbol " << symbolName << " not found";
    if (objName)
        e << " in " << objName;
    throw e;
}

Process::~Process()
{
    td_ta_delete(agent);
}

void
ThreadStack::unwind(Process &p, Elf::CoreRegisters &regs)
{
    stack.clear();
    try {
        auto prevFrame = new Dwarf::StackFrame();
        auto startFrame = prevFrame;

        // Set up the first frame using the machine context registers
        prevFrame->setCoreRegs(regs);
        prevFrame->ip = prevFrame->getReg(IPREG); // use the IP address in current frame

        Dwarf::StackFrame *frame;
        for (size_t frameCount = 0; frameCount < gMaxFrames; frameCount++, prevFrame = frame) {
            stack.push_back(prevFrame);
            try {
               frame = prevFrame->unwind(p);
            }
            catch (const std::exception &ex) {
#if defined(__amd64__) || defined(__i386__) // Hail Mary stack unwinding if we can't use DWARF
               // If the first frame fails to unwind, it might be a crash calling an invalid address.
               // pop the instruction pointer off the stack, and try again.
                if (prevFrame == startFrame) {
                    frame = new Dwarf::StackFrame();
                    *frame = *prevFrame;
                    auto sp = prevFrame->getReg(SPREG);
                    auto in = p.io->read(sp, sizeof frame->ip, (char *)&frame->ip);
                    if (in == sizeof frame->ip) {
                        frame->setReg(SPREG, sp + sizeof frame->ip);
                        continue;
                    }
                }
                else
#ifdef __i386__
                {
                    Elf::Addr reloc;
                    auto obj = p.findObject(prevFrame->ip, &reloc);
                    if (obj) {
                        Elf::Sym symbol;
                        Elf::Addr sigContextAddr;
                        auto objip = prevFrame->ip - reloc;
                        if (obj->findSymbolByName("__restore", symbol) && objip == symbol.st_value)
                            sigContextAddr = prevFrame->getReg(SPREG) + 4;
                        else if (obj->findSymbolByName("__restore_rt", symbol) && objip == symbol.st_value)
                            sigContextAddr = p.io->readObj<Elf::Addr>(prevFrame->getReg(SPREG) + 8) + 20;
                        else
                            throw;
                        // This mapping is based on DWARF regnos, and ucontext.h
                        gregset_t regs;
                        static const struct {
                            int dwarf;
                            int greg;
                        }  gregmap[] = {
                            { 1, REG_EAX },
                            { 2, REG_ECX },
                            { 3, REG_EBX },
                            { 4, REG_ESP },
                            { 5, REG_EBP },
                            { 6, REG_ESI },
                            { 7, REG_EDI },
                            { 8, REG_EIP },
                            { 9, REG_EFL },
                            { 10, REG_CS },
                            { 11, REG_SS },
                            { 12, REG_DS },
                            { 13, REG_ES },
                            { 14, REG_FS }
                        };
                        p.io->readObj(sigContextAddr, &regs);
                        frame = new Dwarf::StackFrame();
                        *frame = *prevFrame;
                        for (auto &reg : gregmap)
                            frame->setReg(reg.dwarf, regs[reg.greg]);
                        frame->ip = regs[REG_EIP];
                        continue;
                    }
                }
#endif
#endif
                  throw;
            }
            if (!frame)
                break;
        }
    }
    catch (const std::exception &ex) {
        std::clog << "warning: exception unwinding stack: " << ex.what() << std::endl;
    }
}
