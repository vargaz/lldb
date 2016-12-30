//===-- JITLoaderMono.cpp ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes

#include "lldb/Breakpoint/Breakpoint.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Interpreter/OptionValueProperties.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/Target.h"
#include "Plugins/ObjectFile/Mono/ObjectFileMono.h"
#include <cstdio>

#include "JITLoaderMono.h"

using namespace lldb;
using namespace lldb_private;

//
// Mono plugin for lldb
//

JITLoaderMono::JITLoaderMono (lldb_private::Process *process) :
    JITLoader(process),
    m_jit_descriptor_addr(LLDB_INVALID_ADDRESS),
    m_jit_break_id(LLDB_INVALID_BREAK_ID)
{
}

JITLoaderMono::~JITLoaderMono ()
{
    if (LLDB_BREAK_ID_IS_VALID(m_jit_break_id))
        m_process->GetTarget().RemoveBreakpointByID (m_jit_break_id);
}

void
JITLoaderMono::DebuggerInitialize(Debugger &debugger)
{
}

void JITLoaderMono::DidAttach()
{
    Target &target = m_process->GetTarget();
    ModuleList &module_list = target.GetImages();
    SetJITBreakpoint(module_list);
}

void JITLoaderMono::DidLaunch()
{
    Target &target = m_process->GetTarget();
    ModuleList &module_list = target.GetImages();
    SetJITBreakpoint(module_list);
}

void
JITLoaderMono::ModulesDidLoad(ModuleList &module_list)
{
    if (!DidSetJITBreakpoint() && m_process->IsAlive())
        SetJITBreakpoint(module_list);
}

bool
JITLoaderMono::DidSetJITBreakpoint() const
{
    return LLDB_BREAK_ID_IS_VALID(m_jit_break_id);
}

addr_t
JITLoaderMono::GetSymbolAddress(ModuleList &module_list, const ConstString &name,
                               SymbolType symbol_type) const
{
    SymbolContextList target_symbols;
    Target &target = m_process->GetTarget();

    if (!module_list.FindSymbolsWithNameAndType(name, symbol_type,
                                                target_symbols))
        return LLDB_INVALID_ADDRESS;

    SymbolContext sym_ctx;
    target_symbols.GetContextAtIndex(0, sym_ctx);

    const Address jit_descriptor_addr = sym_ctx.symbol->GetAddress();
    if (!jit_descriptor_addr.IsValid())
        return LLDB_INVALID_ADDRESS;

    const addr_t jit_addr = jit_descriptor_addr.GetLoadAddress(&target);
    return jit_addr;
}

//------------------------------------------------------------------
// Setup the JIT Breakpoint
//------------------------------------------------------------------
void
JITLoaderMono::SetJITBreakpoint(lldb_private::ModuleList &module_list)
{
    if (DidSetJITBreakpoint())
        return;

    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));
    if (log)
        log->Printf("JITLoaderMono::%s looking for JIT register hook",
                    __FUNCTION__);

    addr_t jit_addr = GetSymbolAddress(module_list,
                                       ConstString("__mono_jit_debug_register_code"),
                                       eSymbolTypeAny);
    if (jit_addr == LLDB_INVALID_ADDRESS)
        return;

    m_jit_descriptor_addr = GetSymbolAddress(module_list,
                                             ConstString("__mono_jit_debug_descriptor"),
                                             eSymbolTypeData);
    if (m_jit_descriptor_addr == LLDB_INVALID_ADDRESS)
    {
        if (log)
            log->Printf(
                "JITLoaderMono::%s failed to find JIT descriptor address",
                __FUNCTION__);
        return;
    }

    if (log)
        log->Printf("JITLoaderMono::%s setting JIT breakpoint",
                    __FUNCTION__);

    Breakpoint *bp =
        m_process->GetTarget().CreateBreakpoint(jit_addr, true, false).get();
    bp->SetCallback(JITDebugBreakpointHit, this, true);
    bp->SetBreakpointKind("jit-debug-register");
    m_jit_break_id = bp->GetID();

    ReadJITDescriptor(true);
}

bool
JITLoaderMono::JITDebugBreakpointHit(void *baton,
                                    StoppointCallbackContext *context,
                                    user_id_t break_id, user_id_t break_loc_id)
{
    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));
    if (log)
        log->Printf("JITLoaderMono::%s hit JIT breakpoint",
                    __FUNCTION__);
    JITLoaderMono *instance = static_cast<JITLoaderMono *>(baton);
    instance->ReadJITDescriptor(false);
	// Continue running
	return false;
}

bool
JITLoaderMono::ReadJITDescriptor(bool all_entries)
{
    Target &target = m_process->GetTarget();
    if (target.GetArchitecture().GetAddressByteSize() == 8)
        return ReadJITDescriptorImpl<uint64_t>(all_entries);
    else
        return ReadJITDescriptorImpl<uint32_t>(all_entries);
}

template <typename ptr_t>
struct mono_debug_entry
{
	uint32_t type;
    uint64_t size;
    ptr_t    addr;
};

template <typename ptr_t>
struct mono_jit_descriptor
{
    uint32_t version;
    ptr_t    entry;
};

typedef enum {
	ENTRY_CODE_REGION = 1,
	ENTRY_METHOD = 2,
	ENTRY_TRAMPOLINE = 3,
} EntryType;

#define MAJOR_VERSION 1
#define MINOR_VERSION 0

static const char*
entry_type_to_str (EntryType type)
{
	switch (type) {
	case ENTRY_CODE_REGION:
		return "code-region";
	case ENTRY_METHOD:
		return "method";
	case ENTRY_TRAMPOLINE:
		return "trampoline";
	default:
		return "unknown";
	}
}

template <typename ptr_t>
bool
JITLoaderMono::ReadJITDescriptorImpl(bool all_entries)
{
    if (m_jit_descriptor_addr == LLDB_INVALID_ADDRESS)
        return false;

    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));
    Target &target = m_process->GetTarget();
    ModuleList &module_list = target.GetImages();

    mono_jit_descriptor<ptr_t> jit_desc;
    const size_t jit_desc_size = sizeof(jit_desc);
    Error error;
    size_t bytes_read = m_process->DoReadMemory(
        m_jit_descriptor_addr, &jit_desc, jit_desc_size, error);
    if (bytes_read != jit_desc_size || !error.Success())
    {
        if (log)
            log->Printf("JITLoaderMono::%s failed to read JIT descriptor",
                        __FUNCTION__);
        return false;
    }

	int major = jit_desc.version >> 16;
	//int minor = jit_desc.version && 0xff;
	if (major != MAJOR_VERSION) {
		if (log)
            log->Printf("JITLoaderMono::%s JIT descriptor has wrong version, expected %d got %d", __FUNCTION__, MAJOR_VERSION, major);
		return false;
	}

    addr_t jit_relevant_entry = (addr_t)jit_desc.entry;

	mono_debug_entry<ptr_t> debug_entry;
	const size_t debug_entry_size = sizeof(debug_entry);
	bytes_read = m_process->DoReadMemory(jit_relevant_entry, &debug_entry, debug_entry_size, error);
	if (bytes_read != debug_entry_size || !error.Success()) {
		if (log)
			log->Printf(
						"JITLoaderGDB::%s failed to read JIT entry at 0x%" PRIx64,
						__FUNCTION__, jit_relevant_entry);
		return false;
	}

	const addr_t &addr = (addr_t)debug_entry.addr;
	const int64_t &size = (int64_t)debug_entry.size;
	ModuleSP module_sp;

	if (log)
		log->Printf(
                    "JITLoaderMono::%s registering JIT entry %s at 0x%" PRIx64
                    " (%" PRIu64 " bytes)",
                    __FUNCTION__, entry_type_to_str ((EntryType)debug_entry.type), addr, (uint64_t) size);

	switch (debug_entry.type) {
	case ENTRY_CODE_REGION:
		//
		// This entry defines a code region in the JIT.
		// We represent it using an lldb module.
		// The entry starts with a magic string so ReadModuleFromMemory ()
		// will create an ObjectFileMono object for it.
		//
		char jit_name[64];
		snprintf(jit_name, 64, "JIT");
		module_sp = m_process->ReadModuleFromMemory(
													FileSpec(jit_name, false), addr, size);

		if (module_sp && module_sp->GetObjectFile()) {
			bool changed;
			ObjectFileMono *ofile = (ObjectFileMono*)module_sp->GetObjectFile ();

			module_sp->GetObjectFile()->GetSymtab();

			module_list.AppendIfNeeded(module_sp);

			ModuleList module_list;
			module_list.Append(module_sp);
				
			module_sp->SetLoadAddress(target, 0, true, changed);

			target.ModulesDidLoad(module_list);

			m_regions [ofile->GetId ()] = ofile;
		} else {
			if (log)
				log->Printf("JITLoaderMono::%s failed to load module for "
							"JIT entry at 0x%" PRIx64,
							__FUNCTION__, addr);
		}
		break;
	case ENTRY_METHOD: {
		uint8_t *buf = new uint8_t [debug_entry.size];
		Error error;

		m_process->ReadMemory (debug_entry.addr, buf, debug_entry.size, error);
		assert (!error.Fail ());

		int region_id = ObjectFileMono::GetMethodEntryRegion(buf, debug_entry.size);

		auto iter = m_regions.find (region_id);
		assert (iter != m_regions.end ());
		ObjectFileMono *ofile = (ObjectFileMono*)iter->second;

		ofile->AddMethod (buf, debug_entry.size);
		break;
	}
	case ENTRY_TRAMPOLINE: {
		uint8_t *buf = new uint8_t [debug_entry.size];
		Error error;

		m_process->ReadMemory (debug_entry.addr, buf, debug_entry.size, error);
		assert (!error.Fail ());

		int region_id = ObjectFileMono::GetTrampolineEntryRegion(buf, debug_entry.size);

		auto iter = m_regions.find (region_id);
		assert (iter != m_regions.end ());
		ObjectFileMono *ofile = (ObjectFileMono*)iter->second;

		ofile->AddTrampoline (buf, debug_entry.size);
		break;
	}
	default:
		if (log)
			log->Printf("JITLoaderMono::%s unknown entry type %d", __FUNCTION__, debug_entry.type);
		break;
	}

    return false;
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
lldb_private::ConstString
JITLoaderMono::GetPluginNameStatic()
{
    static ConstString g_name("mono-jit");
    return g_name;
}

JITLoaderSP
JITLoaderMono::CreateInstance(Process *process, bool force)
{
    JITLoaderSP jit_loader_sp;
	jit_loader_sp.reset(new JITLoaderMono(process));

    return jit_loader_sp;
}

const char *
JITLoaderMono::GetPluginDescriptionStatic()
{
    return "JIT loader plug-in that watches for JIT events from the Mono runtime.";
}

lldb_private::ConstString
JITLoaderMono::GetPluginName()
{
    return GetPluginNameStatic();
}

uint32_t
JITLoaderMono::GetPluginVersion()
{
    return 1;
}

void
JITLoaderMono::Initialize()
{
    PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                   GetPluginDescriptionStatic(),
                                   CreateInstance,
                                   DebuggerInitialize);
}

void
JITLoaderMono::Terminate()
{
    PluginManager::UnregisterPlugin (CreateInstance);
}
