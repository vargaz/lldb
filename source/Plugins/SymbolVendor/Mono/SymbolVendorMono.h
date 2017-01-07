//===-- SymbolVendorMono.h ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_SymbolVendorMono_h_
#define liblldb_SymbolVendorMono_h_

#include "lldb/lldb-private.h"
#include "lldb/Symbol/SymbolVendor.h"

namespace lldb_private {

class SymbolVendorMono : public lldb_private::SymbolVendor
{
public:
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

    static lldb_private::SymbolVendor*
    CreateInstance (const lldb::ModuleSP &module_sp, lldb_private::Stream *feedback_strm);

    //------------------------------------------------------------------
    // Constructors and Destructors
    //------------------------------------------------------------------
    SymbolVendorMono (const lldb::ModuleSP &module_sp);

    virtual
    ~SymbolVendorMono();

    virtual size_t
    GetNumCompileUnits();

    virtual size_t
    FindFunctions (const ConstString &name,
                   const CompilerDeclContext *parent_decl_ctx,
                   uint32_t name_type_mask,
                   bool include_inlines,
                   bool append,
                   SymbolContextList& sc_list);

    virtual size_t
    FindFunctions (const RegularExpression& regex,
                   bool include_inlines,
                   bool append,
                   SymbolContextList& sc_list);

    virtual uint32_t
    ResolveSymbolContext (const Address& so_addr,
                          uint32_t resolve_scope,
                          SymbolContext& sc);

    virtual uint32_t
    ResolveSymbolContext (const FileSpec& file_spec,
                          uint32_t line,
                          bool check_inlines,
                          uint32_t resolve_scope,
                          SymbolContextList& sc_list);

    //------------------------------------------------------------------
    // PluginInterface protocol
    //------------------------------------------------------------------
    virtual lldb_private::ConstString
    GetPluginName();

    virtual uint32_t
    GetPluginVersion();

private:
	CompileUnit *m_cu;

    DISALLOW_COPY_AND_ASSIGN (SymbolVendorMono);
};

}

#endif  // liblldb_SymbolVendorMono_h_
