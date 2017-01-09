//===-- ObjectFileJIT.cpp ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringRef.h"

#include "ObjectFileMono.h"

#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/DataBuffer.h"
#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/FileSpecList.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/RangeMap.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/Timer.h"
#include "lldb/Core/UUID.h"
#include "lldb/Core/dwarf.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/FileSpec.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Target/Platform.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/Target.h"

using namespace lldb;
using namespace lldb_private;

//
// Design:
// - One ObjectFileMono instance for each codegen region in the runtime (address range).
// - Dynamically extended when methods are registered by the runtime
// - SymbolVendorMono instances handle symbol info without an underlying SymbolFile.
// FIXME: Locking
//

/*
 * Functions to decode protocol data
 */

static inline int
decode_byte (uint8_t *buf, uint8_t **endbuf, uint8_t *limit)
{
	*endbuf = buf + 1;
	assert (*endbuf <= limit);
	return buf [0];
}

static inline int
decode_int (uint8_t *buf, uint8_t **endbuf, uint8_t *limit)
{
	*endbuf = buf + 4;
	assert (*endbuf <= limit);

	return (((int)buf [0]) << 24) | (((int)buf [1]) << 16) | (((int)buf [2]) << 8) | (((int)buf [3]) << 0);
}

static inline char*
decode_string (uint8_t *buf, uint8_t **endbuf, uint8_t *limit)
{
	int len = decode_int (buf, &buf, limit);
	char *s;

	if (len < 0) {
		*endbuf = buf;
		return NULL;
	}

	s = new char [len + 1];

	memcpy (s, buf, len);
	s [len] = '\0';
	buf += len;
	*endbuf = buf;

	return s;
}

void
ObjectFileMono::Initialize()
{
    PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                   GetPluginDescriptionStatic(),
                                   CreateInstance,
                                   CreateMemoryInstance,
                                   GetModuleSpecifications);
}

void
ObjectFileMono::Terminate()
{
    PluginManager::UnregisterPlugin (CreateInstance);
}


lldb_private::ConstString
ObjectFileMono::GetPluginNameStatic()
{
    static ConstString g_name("mono-jit");
    return g_name;
}

const char *
ObjectFileMono::GetPluginDescriptionStatic()
{
    return "Mono JIT code object file";
}

ObjectFile *
ObjectFileMono::CreateInstance (const lldb::ModuleSP &module_sp,
                                 DataBufferSP& data_sp,
                                 lldb::offset_t data_offset,
                                 const FileSpec* file,
                                 lldb::offset_t file_offset,
                                 lldb::offset_t length)
{
    return NULL;
}

static const char* MAGIC = { "MONO_JIT_OBJECT_FILE" };

ObjectFile *
ObjectFileMono::CreateMemoryInstance (const lldb::ModuleSP &module_sp,
									  DataBufferSP& data_sp,
									  const ProcessSP &process_sp,
									  lldb::addr_t header_addr)
{
    if (data_sp && data_sp->GetByteSize() > sizeof (MAGIC))
    {
        const uint8_t *magic = data_sp->GetBytes();

		if (memcmp (magic, MAGIC, strlen (MAGIC)) == 0) {
			std::auto_ptr<ObjectFileMono> objfile_ap(new ObjectFileMono(module_sp, data_sp, process_sp, header_addr));

			// Set module architecture to match the target
			ProcessInstanceInfo proc_info;
			bool res = process_sp->GetProcessInfo (proc_info);
			assert (res);
			objfile_ap->SetModulesArchitecture (proc_info.GetArchitecture ());

			return objfile_ap.release();
        }
    }
    return NULL;
}

size_t
ObjectFileMono::GetModuleSpecifications (const lldb_private::FileSpec& file,
                                        lldb::DataBufferSP& data_sp,
                                        lldb::offset_t data_offset,
                                        lldb::offset_t file_offset,
                                        lldb::offset_t length,
                                        lldb_private::ModuleSpecList &specs)
{
    return 0;
}

ObjectFileMono::ObjectFileMono (const lldb::ModuleSP &module_sp,
								lldb::DataBufferSP& header_data_sp,
								const lldb::ProcessSP &process_sp,
								addr_t header_addr) :
    ObjectFile(module_sp, process_sp, header_addr, header_data_sp),
	m_module (module_sp),
	m_unwinders(),
    m_ranges(),
	m_id(0),
	m_methods()
{
}

ObjectFileMono::~ObjectFileMono()
{
}

bool
ObjectFileMono::ParseHeader ()
{
    return false;
}

ByteOrder
ObjectFileMono::GetByteOrder () const
{
    return m_data.GetByteOrder();
}

bool
ObjectFileMono::IsExecutable() const
{
    return false;
}

uint32_t
ObjectFileMono::GetAddressByteSize () const
{
    return m_data.GetAddressByteSize();
}

typedef struct {
	char magic [32];
	uint64_t start;
	int32_t size;
	int id;
} CodeRegionEntry;

typedef struct {
	int op;
	int when;
	int reg;
	int val;
} UnwindOp;

typedef struct {
	uint64_t code;
	int id;
	int region_id;
	int code_size;
	int dummy;
} MethodEntry;

typedef struct {
	uint64_t code;
	int id;
	int region_id;
	int code_size;
	int dummy;
} TrampolineEntry;

int
ObjectFileMono::GetMethodEntryRegion(void *buf, int size)
{
	MethodEntry *entry = (MethodEntry*)buf;
	return entry->region_id;
}

int
ObjectFileMono::GetTrampolineEntryRegion(void *buf, int size)
{
	TrampolineEntry *entry = (TrampolineEntry*)buf;

	return entry->region_id;
}

int
ObjectFileMono::GetId(void)
{
	return m_id;
}

lldb::ModuleSP
ObjectFileMono::GetModule(void)
{
	return m_module;
}

typedef struct
{
	int ret_reg;
	int n_ops;
	UnwindOp *ops;
}  UnwindInfo;

static void
AddUnwindPlan (std::map<lldb::addr_t, lldb::UnwindPlanSP> &m_unwinders, Symbol *symbol, UnwindInfo &info)
{
	UnwindPlanSP plan (new UnwindPlan (lldb::eRegisterKindDWARF));
	plan->SetSourceName ("Mono JIT");
	//plan->SetSourcedFromCompiler (LazyBool (true));
	//plan->SetUnwindPlanValidAtAllInstructions (LazyBool (true));
	plan->SetReturnAddressRegister (info.ret_reg);

	UnwindPlan::Row *row = new UnwindPlan::Row ();

	int cfa_reg = -1;
	int cfa_offset = -1;
	int last_when = -1;
	bool skip_rest = false;
	for (int oindex = 0; oindex < info.n_ops; ++oindex) {
		UnwindOp *op = &info.ops [oindex];

		if (op->when > last_when) {
			UnwindPlan::RowSP row_sp (row);
			plan->AppendRow (row_sp);
			row = new UnwindPlan::Row (*row);
		}

		row->SetOffset (op->when);
		switch (op->op) {
		case DW_CFA_def_cfa:
			row->GetCFAValue ().SetIsRegisterPlusOffset (op->reg, op->val);
			cfa_reg = op->reg;
			break;
		case DW_CFA_offset:
			row->SetRegisterLocationToAtCFAPlusOffset (op->reg, op->val, true);
			break;
		case DW_CFA_def_cfa_offset:
			row->GetCFAValue ().SetIsRegisterPlusOffset (cfa_reg, op->val);
			cfa_offset = op->val;
			break;
		case DW_CFA_def_cfa_register:
			row->GetCFAValue ().SetIsRegisterPlusOffset (op->reg, cfa_offset);
			cfa_reg = op->reg;
			break;
		case DW_CFA_same_value:
			row->SetRegisterLocationToSame (op->reg, false);
			break;
		case DW_CFA_lo_user:
		case DW_CFA_remember_state:
		case DW_CFA_restore_state:
			// FIXME:
			skip_rest = true;
			break;
		default:
			fprintf (stderr, "MISS: %x, reg=0x%x, val=0x%x, when=0x%x\n", op->op, op->reg, op->val, op->when);
			assert (0);
			break;
		}
		if (skip_rest)
			break;
		last_when = op->when;
	}

	UnwindPlan::RowSP row_sp (row);
	plan->AppendRow (row_sp);

	/*
	StreamFile s(stderr, false);
	plan->Dump (s, nullptr, 0);
	*/

	if (info.n_ops > 0)
		m_unwinders [symbol->GetAddressRef().GetFileAddress()] = plan;
}

static void
read_unwind_info (UnwindInfo *info, uint8_t *p, uint8_t **endp, uint8_t *limit)
{
	int i;

	info->ret_reg = decode_byte (p, &p, limit);
	info->n_ops = decode_int (p, &p, limit);
	info->ops = new UnwindOp [info->n_ops];
	for (i = 0; i < info->n_ops; ++i) {
		info->ops [i].op = decode_int (p, &p, limit);
		info->ops [i].when = decode_int (p, &p, limit);
		info->ops [i].reg = decode_int (p, &p, limit);
		info->ops [i].val = decode_int (p, &p, limit);
	}

	*endp = p;
}

void
ObjectFileMono::AddMethod(void *buf, int size)
{
	uint8_t *p = (uint8_t*)buf;
	uint8_t *end = (uint8_t*)buf + size;
	MethodEntry *entry;
	char *name;
	UnwindInfo info;

    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));

	// Read entry
	entry = (MethodEntry*)p;
	p += sizeof (MethodEntry);
	read_unwind_info (&info, p, &p, end);
	name = decode_string (p, &p, end);
	int nsrcfiles = decode_int (p, &p, end);
	std::vector<std::string> srcfiles;
	for (int i = 0; i < nsrcfiles; ++i) {
		char *s = decode_string (p, &p, end);
		srcfiles.push_back (std::string (s));
		delete s;
		// Skip guid
		p += 16;
	}
	std::vector<MonoLineEntry> lines;
	int nlines = decode_int (p, &p, end);
	for (int i = 0; i < nlines; ++i) {
		int native_offset = decode_int (p, &p, end);
		int il_offset = decode_int (p, &p, end);
		int line = decode_int (p, &p, end);
		int file_idx = decode_int (p, &p, end);
		int column = decode_int (p, &p, end);
		int end_line = decode_int (p, &p, end);
		int end_column = decode_int (p, &p, end);

		if (native_offset != -1)
			lines.push_back (MonoLineEntry (native_offset, il_offset, file_idx, line, column, end_line, end_column));
	}
	assert (p <= end);

	if (log)
		log->Printf("ObjectFileMono::%s %s [%p-%p]", __FUNCTION__, name, (char*)entry->code, (char*)entry->code + entry->code_size);

	static int symbol_id = m_symtab_ap->GetNumSymbols ();

	auto section = GetSectionList (true)->GetSectionAtIndex (0);
	int offset = (addr_t)entry->code - (addr_t)section->GetFileAddress ();

	Symbol *symbol = new Symbol (
            symbol_id,    // Symbol table index
            strdup (name),     // symbol name.
            false,      // is the symbol name mangled?
            eSymbolTypeCode, // Type of this symbol
            false,           // Is this globally visible?
            false,           // Is this symbol debug info?
            true,            // Is this symbol a trampoline?
            true,            // Is this symbol artificial?
			section, // Section in which this symbol is defined or null.
			offset,       // Offset in section or symbol value.
            entry->code_size,     // Size in bytes of this symbol.
            true,            // Size is valid
            false,           // Contains linker annotations?
            0);              // Symbol flags.
	int symbol_idx = m_symtab_ap->AddSymbol(*symbol);
	m_symtab_ap->SectionFileAddressesChanged ();

	AddUnwindPlan (m_unwinders, symbol, info);

	MonoMethodInfo *method = new MonoMethodInfo (entry->id, name, AddressRange (section, offset, entry->code_size), m_symtab_ap->SymbolAtIndex (symbol_idx), nsrcfiles, lines);
	method->m_srcfiles = srcfiles;
	delete name;

	m_ranges.Append(RangeToMethod::Entry ((addr_t)entry->code, entry->code_size, method));
	m_methods.push_back (method);
}

void
ObjectFileMono::AddTrampoline(void *buf, int size)
{
	uint8_t *p = (uint8_t*)buf;
	uint8_t *end = (uint8_t*)buf + size;
	TrampolineEntry *entry;
	char *name;
	UnwindInfo info;

    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));

	entry = (TrampolineEntry*)p;
	p += sizeof (TrampolineEntry);
	read_unwind_info (&info, p, &p, end);
	name = decode_string (p, &p, end);
	assert (p <= end);

	if (log)
		log->Printf("ObjectFileMono::%s %s [%p-%p]", __FUNCTION__, name, (char*)entry->code, (char*)entry->code + entry->code_size);

	static int symbol_id = m_symtab_ap->GetNumSymbols ();

	auto section = GetSectionList (true)->GetSectionAtIndex (0);
	int offset = (addr_t)entry->code - (addr_t)section->GetFileAddress ();

	Symbol *symbol = new Symbol (
            symbol_id,     // Symbol table index
            strdup (name), // symbol name.
            false,         // is the symbol name mangled?
            eSymbolTypeCode, // Type of this symbol
            false,           // Is this globally visible?
            false,           // Is this symbol debug info?
            true,            // Is this symbol a trampoline?
            true,            // Is this symbol artificial?
			section, // Section in which this symbol is defined or null.
			offset,       // Offset in section or symbol value.
            entry->code_size,     // Size in bytes of this symbol.
            true,            // Size is valid
            false,           // Contains linker annotations?
            0);              // Symbol flags.

	m_symtab_ap->AddSymbol(*symbol);
	m_symtab_ap->SectionFileAddressesChanged ();
	delete name;

	AddUnwindPlan (m_unwinders, symbol, info);
}

MonoMethodInfo*
ObjectFileMono::FindMethodByAddr (lldb::addr_t addr)
{
    const RangeToMethod::Entry *entry = m_ranges.FindEntryThatContains (addr);

	if (entry)
		return entry->data;
	else
		return NULL;
}

std::vector<MonoMethodInfo*> *
ObjectFileMono::GetMethods (void)
{
	return &m_methods;
}

Symtab *
ObjectFileMono::GetSymtab()
{
	if (m_symtab_ap.get() != NULL)
		return m_symtab_ap.get ();

	DataExtractor reader(m_data);
	CodeRegionEntry entry;
	reader.ExtractBytes (0, sizeof (CodeRegionEntry), eByteOrderLittle, &entry);

	m_id = entry.id;

	Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));

	if (log)
		log->Printf("ObjectFileMono::%s added JIT object file %d for range [%p-%p]", __FUNCTION__, m_id, (uint8_t*)entry.start, (uint8_t*)entry.start + entry.size);

	m_sections_ap.reset(new SectionList());
	m_symtab_ap.reset(new Symtab(this));

	SectionSP section_sp (new Section(GetModule(),        // Module to which this section belongs.
									  this,               // ObjectFile to which this section belongs and should read section data from.
									  0,    // Section ID.
									  ConstString("jitted_code"),               // Section name.
									  eSectionTypeCode,          // Section type.
									  (offset_t)entry.start,     // VM address.
									  entry.size,            // VM size in bytes of this section.
									  0,   // Offset of this section in the file.
									  entry.size,          // Size of the section as found in the file.
									  1,          // Alignment of the section
									  0,    // Flags for this section.
									  0));// Number of host bytes per target byte
	m_sections_ap->AddSection (section_sp);

	return m_symtab_ap.get ();
}

bool
ObjectFileMono::IsStripped ()
{
    return false;
}

void
ObjectFileMono::CreateSections (SectionList &unified_section_list)
{
	unified_section_list = *m_sections_ap;
}

void
ObjectFileMono::Dump (Stream *s)
{
    ModuleSP module_sp(GetModule());
    if (module_sp)
    {
        s->Printf("%p: ", static_cast<void*>(this));
        s->Indent();
        s->PutCString("ObjectFileMono");

        ArchSpec arch;
        if (GetArchitecture(arch))
            *s << ", arch = " << arch.GetArchitectureName();

        s->EOL();
    }
}

bool
ObjectFileMono::GetUUID (lldb_private::UUID* uuid)
{
    return false;
}

uint32_t
ObjectFileMono::GetDependentModules (FileSpecList& files)
{
    files.Clear();
    return 0;
}

lldb_private::Address
ObjectFileMono::GetEntryPointAddress ()
{
    return Address();
}

lldb_private::Address
ObjectFileMono::GetHeaderAddress ()
{
    return Address();
}

ObjectFile::Type
ObjectFileMono::CalculateType()
{
    return eTypeJIT;
}

ObjectFile::Strata
ObjectFileMono::CalculateStrata()
{
    return eStrataJIT;
}

bool
ObjectFileMono::GetArchitecture (ArchSpec &arch)
{
    return false;
}

bool
ObjectFileMono::SetLoadAddress (Target &target,
                               lldb::addr_t value,
                               bool value_is_offset)
{
    size_t num_loaded_sections = 0;
    SectionList *section_list = GetSectionList ();
    if (section_list)
    {
        const size_t num_sections = section_list->GetSize();
        // "value" is an offset to apply to each top level segment
        for (size_t sect_idx = 0; sect_idx < num_sections; ++sect_idx)
        {
            SectionSP section_sp (section_list->GetSectionAtIndex (sect_idx));
            if (section_sp &&
                section_sp->GetFileSize() > 0 &&
                section_sp->IsThreadSpecific() == false)
            {
                if (target.GetSectionLoadList().SetSectionLoadAddress (section_sp, section_sp->GetFileAddress() + value), true)
                    ++num_loaded_sections;
            }
        }
    }
    return num_loaded_sections > 0;
}

    
size_t
ObjectFileMono::ReadSectionData (const lldb_private::Section *section,
                                lldb::offset_t section_offset,
                                void *dst,
                                size_t dst_len) const
{
    return 0;
}

size_t
ObjectFileMono::ReadSectionData (const lldb_private::Section *section,
                                lldb_private::DataExtractor& section_data) const
{
    return 0;
}

lldb::UnwindPlanSP
ObjectFileMono::GetUnwindPlan(lldb_private::AddressRange range, lldb::offset_t offset)
{
	Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_JIT_LOADER));

	//fprintf (stderr, "GetUnwindPlan: %p\n", (uint8_t*)range.GetBaseAddress ().GetFileAddress ());
	auto iter = m_unwinders.find (range.GetBaseAddress ().GetFileAddress ());
	if (iter != m_unwinders.end ()) {
		lldb::UnwindPlanSP plan (iter->second);

		if (log) {
			log->Printf("ObjectFileMono::%s found unwind plan for: %p", __FUNCTION__, (uint8_t*)range.GetBaseAddress ().GetFileAddress ());

			StreamFile s(stderr, false);
			plan->Dump (s, nullptr, 0);
		}

		return plan;
	} else {
		return NULL;
	}
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
lldb_private::ConstString
ObjectFileMono::GetPluginName()
{
    return GetPluginNameStatic();
}

uint32_t
ObjectFileMono::GetPluginVersion()
{
    return 1;
}
