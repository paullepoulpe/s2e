/**
 *  This plugin provides the means of manually specifying the location
 *  of modules in memory.
 *
 *  This allows things like defining poritions of the BIOS.
 */

extern "C" {
#include "config.h"
#include "qemu-common.h"
}


#include <s2e/S2E.h>
#include <s2e/Utils.h>
#include <s2e/ConfigFile.h>
#include "RawMonitor.h"

#include <sstream>

using namespace std;

using namespace s2e;
using namespace s2e::plugins;

S2E_DEFINE_PLUGIN(RawMonitor, "Plugin for monitoring raw module events", "Interceptor");

RawMonitor::~RawMonitor()
{

}

bool RawMonitor::initSection(const std::string &cfgKey, const std::string &svcId)
{
    Cfg cfg;

    bool ok;
    cfg.name = s2e()->getConfig()->getString(cfgKey + ".name", "", &ok);
    if (!ok) {
        s2e()->getWarningsStream() << "You must specify " << cfgKey <<  ".name" << std::endl;
        return false;
    }

    cfg.size = s2e()->getConfig()->getInt(cfgKey + ".size", 0, &ok);
    if (!ok) {
        s2e()->getWarningsStream() << "You must specify " << cfgKey <<  ".size" << std::endl;
        return false;
    }

    cfg.start = s2e()->getConfig()->getInt(cfgKey + ".start", 0, &ok);
    if (!ok) {
        s2e()->getWarningsStream() << "You must specify " << cfgKey <<  ".start" << std::endl;
        return false;
    }

    cfg.nativebase = s2e()->getConfig()->getInt(cfgKey + ".nativebase", 0, &ok);
    if (!ok) {
        s2e()->getWarningsStream() << "You must specify " << cfgKey <<  ".nativebase" << std::endl;
        return false;
    }

    cfg.delayLoad = s2e()->getConfig()->getBool(cfgKey + ".delay");
    cfg.kernelMode = s2e()->getConfig()->getBool(cfgKey + ".kernelmode");

    m_cfg.push_back(cfg);
    return true;
}

void RawMonitor::initialize()
{
    std::vector<std::string> Sections;
    Sections = s2e()->getConfig()->getListKeys(getConfigKey());
    bool noErrors = true;

    m_kernelStart = s2e()->getConfig()->getInt(getConfigKey() + ".kernelStart");

    foreach2(it, Sections.begin(), Sections.end()) {
        if (*it == "kernelStart") {
            continue;
        }

        s2e()->getMessagesStream() << "Scanning section " << getConfigKey() << "." << *it << std::endl;
        std::stringstream sk;
        sk << getConfigKey() << "." << *it;
        if (!initSection(sk.str(), *it)) {
            noErrors = false;
        }
    }

    if (!noErrors) {
        s2e()->getWarningsStream() << "Errors while scanning the WindowsService sections"
            <<std::endl;
        exit(-1);
    }

    m_onTranslateInstruction = s2e()->getCorePlugin()->onTranslateInstructionStart.connect(
        sigc::mem_fun(*this, &RawMonitor::onTranslateInstructionStart));

    s2e()->getCorePlugin()->onCustomInstruction.connect(
            sigc::mem_fun(*this, &RawMonitor::onCustomInstruction));

}

void RawMonitor::onCustomInstruction(S2EExecutionState* state, uint64_t opcode)
{
    //XXX: find a better way of allocating custom opcodes
    if (!(((opcode>>8) & 0xFF) == 0xAA)) {
        return;
    }

    opcode >>= 16;
    uint8_t op = opcode & 0xFF;
    opcode >>= 8;

    switch(op) {
    case 0:
        {
            //Module load
            //eax = pointer to module name
            //ebx = runtime load base
            //ecx = entry point
            uint32_t rtloadbase, name, size;
            bool ok = true;

            ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                                 &name, 4);
            ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                                 &rtloadbase, 4);
            ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                                 &size, 4);

            if(!ok) {
                s2e()->getWarningsStream(state)
                    << "ERROR: symbolic argument was passed to s2e_op "
                       "rawmonitor loadmodule" << std::endl;
                break;
            }

            std::string nameStr;
            if(!state->readString(name, nameStr)) {
                s2e()->getWarningsStream(state)
                        << "Error reading module name string from the guest" << std::endl;
                return;
            }

            //Look for the module in the config section and update its load address
            CfgList::iterator it;

            for (it = m_cfg.begin(); it != m_cfg.end(); ++it) {
                Cfg &c = *it;
                if (c.name == nameStr) {
                    s2e()->getMessagesStream() << "RawMonitor: Registering " << nameStr << " "
                            " @0x" << std::hex << rtloadbase << " size=0x" << size  << std::endl;
                    c.start = rtloadbase;
                    c.size = size;
                    loadModule(state, c, false);
                }
            }
        }
        break;

    case 1:
        {
            //Specifying a new import descriptor
            uint32_t dllname, funcname, funcptr;
            bool ok = true;
            ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                                 &dllname, 4);
            ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                                 &funcname, 4);
            ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                                 &funcptr, 4);

            if (!ok) {
                s2e()->getWarningsStream(state)
                    << "ERROR: symbolic argument was passed to s2e_op "
                       "rawmonitor loadimportdescriptor" << std::endl;
                break;
            }

            std::string dllnameStr;
            if(!state->readString(dllname, dllnameStr)) {
                s2e()->getWarningsStream(state)
                        << "Error reading dll name string from the guest" << std::endl;
                return;
            }

            std::string funcnameStr;
            if(!state->readString(funcname, funcnameStr)) {
                s2e()->getWarningsStream(state)
                        << "Error reading function name string from the guest" << std::endl;
                return;
            }

            s2e()->getMessagesStream() << "RawMonitor: Registering " << dllnameStr << " "
                    << funcnameStr << " @0x" << std::hex << funcptr << std::endl;

            m_imports[dllnameStr][funcnameStr] = funcptr;
            break;
        }

    default:
        s2e()->getWarningsStream() << "Invalid RawMonitor opcode 0x" << std::hex << opcode << std::endl;
        break;
    }
}

void RawMonitor::loadModule(S2EExecutionState *state, const Cfg &c, bool skipIfDelay)
{
    ModuleDescriptor md;
    if (c.delayLoad && skipIfDelay) {
        return;
    }
    md.Name = c.name;
    md.NativeBase = c.nativebase;
    md.LoadBase = c.start;
    md.Size = c.size;
    md.Pid = c.kernelMode ? 0 : state->getPid();
    md.EntryPoint = c.entrypoint;

    s2e()->getDebugStream() << "RawMonitor loaded " << c.name << " " <<
            std::hex << "0x" << c.start << " 0x" << c.size << std::dec << std::endl;
    onModuleLoad.emit(state, md);
}

void RawMonitor::onTranslateInstructionStart(ExecutionSignal *signal,
                                                   S2EExecutionState *state,
                                                   TranslationBlock *tb,
                                                   uint64_t pc)
{
    CfgList::const_iterator it;

    for (it = m_cfg.begin(); it != m_cfg.end(); ++it) {
        const Cfg &c = *it;
        loadModule(state, c, true);
    }

    m_onTranslateInstruction.disconnect();
}


bool RawMonitor::getImports(S2EExecutionState *s, const ModuleDescriptor &desc, Imports &I)
{
    I = m_imports;
    return true;
}

bool RawMonitor::getExports(S2EExecutionState *s, const ModuleDescriptor &desc, Exports &E)
{
    return false;
}

bool RawMonitor::isKernelAddress(uint64_t pc) const
{
    return false;
}

uint64_t RawMonitor::getPid(S2EExecutionState *s, uint64_t pc)
{
    if (pc >= m_kernelStart) {
        return 0;
    }
    return s->getPid();
}
