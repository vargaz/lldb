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
    SymbolVendor (module_sp), m_cu (nullptr)
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

size_t
SymbolVendorMono::GetNumCompileUnits()
{
	return 0;
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

struct Entry {
    Entry()
        : file_addr(LLDB_INVALID_ADDRESS), line(0), column(0), file_idx(0),
          is_start_of_statement(false), is_start_of_basic_block(false),
          is_prologue_end(false), is_epilogue_begin(false),
          is_terminal_entry(false) {}

    Entry(lldb::addr_t _file_addr, uint32_t _line, uint16_t _column,
          uint16_t _file_idx, bool _is_start_of_statement,
          bool _is_start_of_basic_block, bool _is_prologue_end,
          bool _is_epilogue_begin, bool _is_terminal_entry)
        : file_addr(_file_addr), line(_line), column(_column),
          file_idx(_file_idx), is_start_of_statement(_is_start_of_statement),
          is_start_of_basic_block(_is_start_of_basic_block),
          is_prologue_end(_is_prologue_end),
          is_epilogue_begin(_is_epilogue_begin),
          is_terminal_entry(_is_terminal_entry) {}

    int bsearch_compare(const void *key, const void *arrmem);

    void Clear() {
      file_addr = LLDB_INVALID_ADDRESS;
      line = 0;
      column = 0;
      file_idx = 0;
      is_start_of_statement = false;
      is_start_of_basic_block = false;
      is_prologue_end = false;
      is_epilogue_begin = false;
      is_terminal_entry = false;
    }

    static int Compare(const Entry &lhs, const Entry &rhs) {
// Compare the sections before calling
#define SCALAR_COMPARE(a, b)                                                   \
  if (a < b)                                                                   \
    return -1;                                                                 \
  if (a > b)                                                                   \
  return +1
      SCALAR_COMPARE(lhs.file_addr, rhs.file_addr);
      SCALAR_COMPARE(lhs.line, rhs.line);
      SCALAR_COMPARE(lhs.column, rhs.column);
      SCALAR_COMPARE(lhs.is_start_of_statement, rhs.is_start_of_statement);
      SCALAR_COMPARE(lhs.is_start_of_basic_block, rhs.is_start_of_basic_block);
      // rhs and lhs reversed on purpose below.
      SCALAR_COMPARE(rhs.is_prologue_end, lhs.is_prologue_end);
      SCALAR_COMPARE(lhs.is_epilogue_begin, rhs.is_epilogue_begin);
      // rhs and lhs reversed on purpose below.
      SCALAR_COMPARE(rhs.is_terminal_entry, lhs.is_terminal_entry);
      SCALAR_COMPARE(lhs.file_idx, rhs.file_idx);
#undef SCALAR_COMPARE
      return 0;
    }

	  bool operator<(const Entry &b) {
		  return Entry::Compare(*this, b) < 0;
}
    //------------------------------------------------------------------
    // Member variables.
    //------------------------------------------------------------------
    lldb::addr_t file_addr; ///< The file address for this line entry
    uint32_t line;   ///< The source line number, or zero if there is no line
                     ///number information.
    uint16_t column; ///< The column number of the source line, or zero if there
                     ///is no column information.
    uint16_t file_idx : 11, ///< The file index into CompileUnit's file table,
                            ///or zero if there is no file information.
        is_start_of_statement : 1, ///< Indicates this entry is the beginning of
                                   ///a statement.
        is_start_of_basic_block : 1, ///< Indicates this entry is the beginning
                                     ///of a basic block.
        is_prologue_end : 1, ///< Indicates this entry is one (of possibly many)
                             ///where execution should be suspended for an entry
                             ///breakpoint of a function.
        is_epilogue_begin : 1, ///< Indicates this entry is one (of possibly
                               ///many) where execution should be suspended for
                               ///an exit breakpoint of a function.
        is_terminal_entry : 1; ///< Indicates this entry is that of the first
                               ///byte after the end of a sequence of target
                               ///machine instructions.
};

uint32_t
SymbolVendorMono::ResolveSymbolContext (const Address& so_addr,
										uint32_t resolve_scope,
										SymbolContext& sc)
{
    ObjectFileMono *obj_file = (ObjectFileMono*)GetModule()->GetObjectFile();

	MonoMethodInfo *method = obj_file->FindMethodByAddr (so_addr.GetFileAddress ());
	if (!method)
		return 0;

	if (!m_cu) {
		m_cu = new CompileUnit (GetModule (), NULL, "", 1, LanguageType::eLanguageTypeC, eLazyBoolNo);
		m_cu->GetSupportFiles ();
		m_cu->SetLineTable (new LineTable (m_cu));
	}
	//fprintf (stderr, "ResolveSymbolContext: %p\n", so_addr.GetFileAddress ());

	if (!method->m_cu_added) {
		//llvm::outs () << "A: " << method->m_name << "\n";
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

    uint32_t resolved = 0;
	sc.comp_unit = m_cu;
	resolved |= eSymbolContextCompUnit;
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
