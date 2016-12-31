//===-- JITLoaderMono.h ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_JITLoaderMono_h_
#define liblldb_JITLoaderMono_h_

// C Includes
// C++ Includes
#include <map>

// Other libraries and framework includes
// Project includes
#include "lldb/Target/JITLoader.h"
#include "lldb/Target/Process.h"
#include "lldb/Symbol/ObjectFile.h"

class JITLoaderMono : public lldb_private::JITLoader
{
public:
    JITLoaderMono(lldb_private::Process *process);

    ~JITLoaderMono() override;

    //------------------------------------------------------------------
    // Static Functions
    //------------------------------------------------------------------
    static void
    Initialize();

    static void
    Terminate();

    static lldb_private::ConstString
    GetPluginNameStatic();

    static const char *
    GetPluginDescriptionStatic();

    static lldb::JITLoaderSP
    CreateInstance (lldb_private::Process *process, bool force);

    static void
    DebuggerInitialize(lldb_private::Debugger &debugger);

    //------------------------------------------------------------------
    // PluginInterface protocol
    //------------------------------------------------------------------
    lldb_private::ConstString
    GetPluginName() override;

    uint32_t
    GetPluginVersion() override;

    //------------------------------------------------------------------
    // JITLoader interface
    //------------------------------------------------------------------
    void
    DidAttach() override;

    void
    DidLaunch() override;

    void
    ModulesDidLoad(lldb_private::ModuleList &module_list) override;

private:
    lldb::addr_t
    GetSymbolAddress(lldb_private::ModuleList &module_list,
                     const lldb_private::ConstString &name,
                     lldb::SymbolType symbol_type) const;

    void
    SetJITBreakpoint(lldb_private::ModuleList &module_list);

    bool
    DidSetJITBreakpoint() const;

	void
    ReadJITDescriptor(bool all_entries);

    template <typename ptr_t>
    bool
    ReadJITDescriptorImpl(bool all_entries);

    static bool
    JITDebugBreakpointHit(void *baton,
                          lldb_private::StoppointCallbackContext *context,
                          lldb::user_id_t break_id,
                          lldb::user_id_t break_loc_id);

	void
	ProcessEntry (uint32_t type, const lldb::addr_t addr, int64_t size);

    lldb::addr_t m_jit_descriptor_addr;
    lldb::user_id_t m_jit_break_id;
	std::map<int,lldb_private::ObjectFile*> m_regions;
};

#endif // liblldb_JITLoaderMono_h_

