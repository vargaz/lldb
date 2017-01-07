//===-- ObjectFileMono.h -----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_ObjectFileMono_h_
#define liblldb_ObjectFileMono_h_

// C Includes
#include <stdint.h>

// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/lldb-private.h"
#include "lldb/Host/FileSpec.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Core/UUID.h"
#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/Address.h"
#include "lldb/Core/AddressRange.h"
#include "lldb/Core/RangeMap.h"

namespace lldb_private {

class MonoLineEntry
{
public:
	MonoLineEntry (int native_offset, int il_offset, int file_idx, int line, int column, int end_line, int end_column) :
		m_native_offset (native_offset), m_il_offset (il_offset), m_file_idx (file_idx), m_line (line), m_column (column),
		m_end_line (end_line), m_end_column (end_column) {
	}

	int m_native_offset, m_il_offset, m_file_idx, m_line, m_column, m_end_line, m_end_column;
};

class MonoMethodInfo
{
public:
	MonoMethodInfo (int id, char *name, AddressRange range, Symbol *symbol, int nsrcfiles, std::vector<MonoLineEntry> lines) :
		m_id (id), m_name(name), m_range (range), m_symbol (symbol), m_srcfiles (nsrcfiles), m_lines (lines), m_cu_added (false) {
	}

	int m_id;
	std::string m_name;
	AddressRange m_range;
	Symbol *m_symbol;
	std::vector<std::string> m_srcfiles;
	std::vector<MonoLineEntry> m_lines;
	// Whenever its added to the cu
	bool m_cu_added;
};

class ObjectFileMono :
    public lldb_private::ObjectFile
{
public:
	ObjectFileMono (const lldb::ModuleSP &module_sp,
					lldb::DataBufferSP& header_data_sp,
					const lldb::ProcessSP &process_sp,
					lldb::addr_t header_addr);

    ~ObjectFileMono() override;

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

    static lldb_private::ObjectFile *
    CreateInstance (const lldb::ModuleSP &module_sp,
                    lldb::DataBufferSP& data_sp,
                    lldb::offset_t data_offset,
                    const lldb_private::FileSpec* file,
                    lldb::offset_t file_offset,
                    lldb::offset_t length);

    static lldb_private::ObjectFile *
    CreateMemoryInstance (const lldb::ModuleSP &module_sp, 
                          lldb::DataBufferSP& data_sp, 
                          const lldb::ProcessSP &process_sp, 
                          lldb::addr_t header_addr);

    static size_t
    GetModuleSpecifications (const lldb_private::FileSpec& file,
                             lldb::DataBufferSP& data_sp,
                             lldb::offset_t data_offset,
                             lldb::offset_t file_offset,
                             lldb::offset_t length,
                             lldb_private::ModuleSpecList &specs);

	static int
	GetMethodEntryRegion(void *buf, int size);

	static int
	GetTrampolineEntryRegion(void *buf, int size);

    //------------------------------------------------------------------
    // Member Functions
    //------------------------------------------------------------------
    bool
    ParseHeader() override;

    bool
    SetLoadAddress(lldb_private::Target &target,
                   lldb::addr_t value,
                   bool value_is_offset) override;
    
    lldb::ByteOrder
    GetByteOrder() const override;
    
    bool
    IsExecutable() const override;

    uint32_t
    GetAddressByteSize() const override;

    lldb_private::Symtab *
    GetSymtab() override;

    bool
    IsStripped() override;
    
    void
    CreateSections(lldb_private::SectionList &unified_section_list) override;

    void
    Dump(lldb_private::Stream *s) override;

    bool
    GetArchitecture(lldb_private::ArchSpec &arch) override;

    bool
    GetUUID(lldb_private::UUID* uuid) override;

    uint32_t
    GetDependentModules(lldb_private::FileSpecList& files) override;
    
    size_t
    ReadSectionData(const lldb_private::Section *section,
                    lldb::offset_t section_offset,
                    void *dst,
                    size_t dst_len) const override;

    size_t
    ReadSectionData(const lldb_private::Section *section,
                    lldb_private::DataExtractor& section_data) const override;
    
    lldb_private::Address
    GetEntryPointAddress() override;
    
    lldb_private::Address
    GetHeaderAddress() override;
    
    ObjectFile::Type
    CalculateType() override;
    
    ObjectFile::Strata
    CalculateStrata() override;

	lldb::UnwindPlanSP
	GetUnwindPlan(lldb_private::AddressRange range, lldb::offset_t offset) override;

	void AddMethod (void *buf, int size);

	void AddTrampoline (void *buf, int size);

	MonoMethodInfo* FindMethodByAddr (lldb::addr_t addr);

	int	GetId (void);

	lldb::ModuleSP GetModule (void);

    //------------------------------------------------------------------
    // PluginInterface protocol
    //------------------------------------------------------------------
    lldb_private::ConstString
    GetPluginName() override;

    uint32_t
    GetPluginVersion() override;

private:
	lldb::ModuleSP m_module;
	std::map<lldb::addr_t, lldb::UnwindPlanSP> m_unwinders;

    typedef lldb_private::RangeDataArray<lldb::addr_t, uint32_t, MonoMethodInfo*, 1> RangeToMethod;
	RangeToMethod m_ranges;
	int m_id;
};

}

#endif // liblldb_ObjectFileMono_h_
