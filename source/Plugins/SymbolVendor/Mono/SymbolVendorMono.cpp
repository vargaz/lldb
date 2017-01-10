//===-- SymbolVendorMono.cpp ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SymbolVendorMono.h"

#include <string.h>

#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/Timer.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/Symbols.h"
#include "lldb/Host/XML.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/LineTable.h"
#include "Plugins/ObjectFile/Mono/ObjectFileMono.h"

using namespace lldb;
using namespace lldb_private;

//----------------------------------------------------------------------
// SymbolVendorMono constructor
//----------------------------------------------------------------------
SymbolVendorMono::SymbolVendorMono(const lldb::ModuleSP &module_sp) :
    SymbolVendor (module_sp), m_cu (nullptr), m_nadded_methods (0)
{
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
SymbolVendorMono::~SymbolVendorMono()
{
}

void
SymbolVendorMono::Initialize()
{
    PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                   GetPluginDescriptionStatic(),
                                   CreateInstance);
}

void
SymbolVendorMono::Terminate()
{
    PluginManager::UnregisterPlugin (CreateInstance);
}


lldb_private::ConstString
SymbolVendorMono::GetPluginNameStatic()
{
    static ConstString g_name("mono");
    return g_name;
}

const char *
SymbolVendorMono::GetPluginDescriptionStatic()
{
    return "Symbol vendor for Mono.";
}

SymbolVendor*
SymbolVendorMono::CreateInstance (const lldb::ModuleSP &module_sp, lldb_private::Stream *feedback_strm)
{
    if (!module_sp)
        return NULL;

    ObjectFile * obj_file = module_sp->GetObjectFile();
    if (!obj_file)
        return NULL;

    static ConstString obj_file_macho("mono-jit");
    ConstString obj_name = obj_file->GetPluginName();
    if (obj_name != obj_file_macho)
        return NULL;

    SymbolVendorMono* symbol_vendor = new SymbolVendorMono(module_sp);
	symbol_vendor->AddSymbolFileRepresentation(obj_file->shared_from_this());
	symbol_vendor->m_cu = nullptr;
	return symbol_vendor;
}

void
SymbolVendorMono::CreateCU (void)
{
	if (!m_cu) {
		m_cu = CompUnitSP (new CompileUnit (GetModule (), NULL, "", 1, LanguageType::eLanguageTypeC, eLazyBoolNo));
		m_cu->GetSupportFiles ();
		m_cu->SetLineTable (new LineTable (m_cu.get ()));
	}
}

// Add newly registered methods to the CU
void
SymbolVendorMono::AddMethods (void)
{
    ObjectFileMono *obj_file = (ObjectFileMono*)GetModule()->GetObjectFile();

	std::vector<MonoMethodInfo*> *methods = obj_file->GetMethods ();
	if (methods->size () == m_nadded_methods)
		return;
	for (MonoMethodInfo *method : *methods) {
		if (method->m_cu_added)
			continue;

		m_nadded_methods ++;

		//llvm::outs () << "ADD: " << method->m_name << "\n";
		method->m_cu_added = true;

		//llvm::outs () << "B: " << method->m_range.GetBaseAddress ().GetFileAddress () << " " << method->m_range.GetByteSize () << "\n";

		int *file_idx_map = new int [method->m_srcfiles.size ()];

		FileSpecList &files = m_cu->GetSupportFiles ();
		for (int i = 0; i < method->m_srcfiles.size (); ++i) {
			file_idx_map [i] = -1;
			//llvm::outs () << "S: " << method->m_srcfiles [i] << "\n";
			FileSpec spec (llvm::StringRef (method->m_srcfiles [i].c_str ()), true);
			files.AppendIfUnique (spec);

			size_t idx = files.FindFileIndex (0, spec, true, false);
			assert (idx != UINT32_MAX);
			file_idx_map [i] = idx;
		}

		LineTable *table = m_cu->GetLineTable ();

		LineSequence *seq = table->CreateLineSequenceContainer ();
		for (int i = 0; i < method->m_lines.size (); ++i) {
			MonoLineEntry &entry = method->m_lines [i];

			int file_idx = file_idx_map [entry.m_file_idx];
			if (file_idx == -1)
				continue;
			addr_t addr = method->m_range.GetBaseAddress ().GetFileAddress ();
			table->AppendLineEntryToSequence (seq, addr + entry.m_native_offset, entry.m_line, entry.m_column, file_idx, true, false, false, false, false);
			if (i == method->m_lines.size () - 1)
				table->AppendLineEntryToSequence (seq, addr + method->m_range.GetByteSize (), entry.m_line, entry.m_column, file_idx, true, false, false, false, true);
		}

		table->InsertSequence (seq);
	}
}

size_t
SymbolVendorMono::GetNumCompileUnits(void)
{
	return 1;
}

lldb::CompUnitSP
SymbolVendorMono::GetCompileUnitAtIndex(size_t idx)
{
	CreateCU ();
	AddMethods ();

	if (idx == 0)
		return m_cu;
	else
		assert (0);
}

size_t
SymbolVendorMono::FindFunctions (const ConstString &name,
                   const CompilerDeclContext *parent_decl_ctx,
                   uint32_t name_type_mask,
                   bool include_inlines,
                   bool append,
                   SymbolContextList& sc_list)
{
	return 0;
}

size_t
SymbolVendorMono::FindFunctions (const RegularExpression& regex,
								 bool include_inlines,
								 bool append,
								 SymbolContextList& sc_list)
{
	return 0;
}

uint32_t
SymbolVendorMono::ResolveSymbolContext (const Address& so_addr,
										uint32_t resolve_scope,
										SymbolContext& sc)
{
    ObjectFileMono *obj_file = (ObjectFileMono*)GetModule()->GetObjectFile();

	MonoMethodInfo *method = obj_file->FindMethodByAddr (so_addr.GetFileAddress ());
	if (!method)
		return 0;

	CreateCU ();
	AddMethods ();

	//fprintf (stderr, "ResolveSymbolContext: %p\n", so_addr.GetFileAddress ());

    uint32_t resolved = 0;
	if (resolve_scope & eSymbolContextCompUnit) {
		sc.comp_unit = m_cu.get ();
		resolved |= eSymbolContextCompUnit;
	}
#if 0
	if (resolve_scope & eSymbolContextSymbol) {
		sc.symbol = method->m_symbol;
		resolved |= eSymbolContextSymbol;
	}
#endif

	if (resolve_scope & eSymbolContextLineEntry) {
		LineTable *table = m_cu->GetLineTable ();

		LineEntry entry;
		if (table->FindLineEntryByAddress (so_addr, entry, nullptr)) {
			sc.line_entry = entry;
			resolved |= eSymbolContextLineEntry;
		}
	}

	return resolved;
}

uint32_t
SymbolVendorMono::ResolveSymbolContext (const FileSpec& file_spec,
										uint32_t line,
										bool check_inlines,
										uint32_t resolve_scope,
										SymbolContextList& sc_list)
{
	return 0;
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
ConstString
SymbolVendorMono::GetPluginName()
{
    return GetPluginNameStatic();
}

uint32_t
SymbolVendorMono::GetPluginVersion()
{
    return 1;
}
