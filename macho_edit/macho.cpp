#include <iomanip>
#include <iostream>
#include <sstream>

#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cpuinfo.h"
#include "fileutils.h"
#include "macho.h"
#include "macros.h"
#include "magicnames.h"

MachO::MachO() {
}

MachO::MachO(const char *filename) {
	file = fopen(filename, "r+");
	if(!file) {
		throw "Couldn't open file!";
	}

	fd = fileno(file);

	fseeko(file, 0, SEEK_END);
	off_t fsize = ftello(file);
	rewind(file);

	if(fsize > UINT32_MAX) {
		throw "File size larger than 2^32 bytes!";
	}

	file_size = (uint32_t)fsize;

	uint32_t magic;
	PEEK(magic, file);

	if(!IS_MAGIC(magic)) {
		std::ostringstream o;
		o << "Unknown magic: 0x" << std::hex << magic;
		throw o.str();
	}

	is_fat = IS_FAT(magic);

	if(is_fat) {
		fat_magic = magic;

		fat_header fat_header;
		READ(fat_header, file);
		n_archs = SWAP32(fat_header.nfat_arch, magic);

		for(uint32_t i = 0; i < n_archs; i++) {
			fat_arch arch;
			READ(arch, file);
			swap_arch(&arch);
			archs.push_back(MachOArch(&arch, file));
		}
	} else {
		fat_magic = FAT_CIGAM;

		n_archs = 1;

		mach_header mh;
		READ(mh, file);

		fat_arch arch = arch_from_mach_header(mh, file_size);
		swap_arch(&arch);
		archs = {MachOArch(&arch, file)};
	}
}

void MachO::swap_arch(fat_arch *arch) const {
	uint32_t *fields = (uint32_t *)arch;
	for(size_t i = 0; i < sizeof(*arch) / sizeof(uint32_t); i++) {
		fields[i] = SWAP32(fields[i], fat_magic);
	}
}

void MachO::write_fat_header() const {
	if(!is_fat) {
		return;
	}

	rewind(file);
	fat_header fat_header;

	fat_header.magic = fat_magic;
	fat_header.nfat_arch = SWAP32(n_archs, fat_magic);

	WRITE(fat_header, file);
}

void MachO::write_fat_archs() {
	if(!is_fat) {
		const MachOArch &arch = archs[0];
		uint32_t arch_size = arch.fat_arch.size;
		if(file_size != arch_size) {
			fflush(file);
			ftruncate(fd, arch_size);
			file_size = arch_size;
		}
		return;
	}

	fseeko(file, sizeof(fat_header), SEEK_SET);
	for(auto &arch : archs) {
		fat_arch fat_arch = arch.fat_arch;
		swap_arch(&fat_arch);
		WRITE(fat_arch, file);
	}

	if(n_archs > 0) {
		fat_arch &fat_arch = archs.back().fat_arch;
		uint32_t new_size = fat_arch.offset + fat_arch.size;
		if(new_size != file_size) {
			fflush(file);
			ftruncate(fd, new_size);

			file_size = new_size;
		}
	}
}

void MachO::write_mach_header(MachOArch &arch) const {
	fseeko(file, arch.fat_arch.offset, SEEK_SET);
	WRITE(arch.mach_header, file);
}

void MachO::write_load_command(LoadCommand &lc) const {
	fseeko(file, lc.file_offset, SEEK_SET);
	fwrite(lc.raw_lc, lc.cmdsize, 1, file);
}

void MachO::print_description() const {
	if(is_fat) {
		std::cout << "Fat mach-o binary with " << n_archs << " archs:\n";
	} else {
		std::cout << "Thin mach-o binary:\n";
	}

	for(auto &arch : archs) {
		std::cout << "\t" << arch.description() << "\n";
	}
}

fat_arch MachO::arch_from_mach_header(mach_header &mh, uint32_t size) const {
	fat_arch arch;

	arch.offset = SWAP32(0, fat_magic);
	arch.size = SWAP32(size, fat_magic);

	cpu_type_t cputype = SWAP32(mh.cputype, mh.magic);
	arch.cputype = SWAP32(cputype, fat_magic);
	arch.cpusubtype = SWAP32(SWAP32(mh.cpusubtype, mh.magic), fat_magic);

	uint32_t align = cpu_pagesize(cputype);
	arch.align = SWAP32(align, fat_magic);

	return arch;
}

void MachO::make_fat() {
	assert(!is_fat);

	MachOArch &arch = archs[0];

	uint32_t offset = ROUND_UP(sizeof(fat_header), 1 << arch.fat_arch.align);

	ftruncate(fd, file_size + offset);

	fmove(file, offset, 0, file_size);
	fzero(file, offset, 0);

	// dyld doesn't like FAT_MAGIC
	fat_magic = FAT_CIGAM;
	write_fat_header();

	arch.fat_arch.offset = offset;
	write_fat_archs();

	fflush(file);

	is_fat = true;
	file_size += offset;
}

void MachO::make_thin(uint32_t arch_index) {
	assert(is_fat);

	MachOArch &arch = archs[arch_index];

	archs = {arch};

	uint32_t size = arch.fat_arch.size;
	fmove(file, 0, arch.fat_arch.offset, size);

	fflush(file);
	ftruncate(fd, size);

	file_size = size;
	n_archs = 1;
	is_fat = false;

	//swap_arch ????
}

bool MachO::save_arch_to_file(uint32_t arch_index, const char *filename) const {
	const MachOArch &arch = archs[arch_index];

	FILE *f = fopen(filename, "w");
	if(!f) {
		return false;
	}

	fcpy(f, 0, file, arch.fat_arch.offset, arch.fat_arch.size);

	fclose(f);

	chmod(filename, S_IRWXU);

	return true;
}

void MachO::remove_arch(uint32_t arch_index) {
	MachOArch &arch = archs[arch_index];

	fzero(file, arch.fat_arch.offset, arch.fat_arch.size);

	uint32_t new_offset;
	if(arch_index == 0) {
		new_offset = sizeof(fat_header);
	} else {
		fat_arch &prev_raw = archs[arch_index - 1].fat_arch;
		new_offset = prev_raw.offset + prev_raw.size;
	}

	archs.erase(archs.begin() + arch_index);
	n_archs--;

	for(uint32_t i = arch_index; i < n_archs; i++) {
		MachOArch &arch = archs[i];

		uint32_t offset = arch.fat_arch.offset;
		uint32_t size =  arch.fat_arch.size;

		new_offset = ROUND_UP(new_offset, 1 << arch.fat_arch.align);
		arch.fat_arch.offset = new_offset;

		fmove(file, new_offset, offset, size);
		fzero(file, new_offset + size, offset - new_offset);

		new_offset += size;
	}

	write_fat_header();
	write_fat_archs();

	fflush(file);
	ftruncate(fd, new_offset);

	file_size = new_offset;
}

void MachO::insert_arch_from_macho(MachO &macho, uint32_t arch_index) {
	n_archs++;

	MachOArch arch = macho.archs[arch_index];
	fat_arch &fat_arch = arch.fat_arch;

	macho.swap_arch(&fat_arch);
	swap_arch(&fat_arch);

	//arch.swap_mach_header(); ????

	uint32_t offset = ROUND_UP(file_size, 1 << fat_arch.align);

	fat_arch.offset = offset;

	archs.push_back(arch);

	uint32_t new_size = file_size + offset;

	ftruncate(fd, new_size);
	fzero(file, file_size, offset - file_size);

	fcpy(file, offset, macho.file, 0, fat_arch.size);

	file_size = new_size;

	write_fat_header();
	write_fat_archs();
}

void MachO::remove_load_command(uint32_t arch_index, uint32_t lc_index) {
	MachOArch &arch = archs[arch_index];
	auto &load_commands = arch.load_commands;

	if(load_commands.size() > 1) {
		move_load_command(arch_index, lc_index, (uint32_t)load_commands.size() - 1);
	}

	LoadCommand &lc = load_commands.back();

	arch.mach_header.ncmds--;
	arch.mach_header.sizeofcmds -= lc.cmdsize;

	write_mach_header(arch);

	fzero(file, lc.file_offset, lc.cmdsize);

	load_commands.pop_back();
}

void MachO::move_load_command(uint32_t arch_index, uint32_t lc_index, uint32_t new_index) {
	if(lc_index == new_index) {
		return;
	}
	if(lc_index > new_index) {
		move_load_command(arch_index, new_index, lc_index);
		return;
	}

	MachOArch &arch = archs[arch_index];
	auto &load_commands = arch.load_commands;
	LoadCommand lc_to_move = load_commands[lc_index];

	off_t new_offset = lc_to_move.file_offset;
	fseeko(file, new_offset, SEEK_SET);

	for(uint32_t i = lc_index + 1; i <= new_index; i++) {
		LoadCommand &lc = load_commands[i];
		lc.file_offset = new_offset;

		fwrite(lc.raw_lc, lc.cmdsize, 1, file);

		new_offset += lc.cmdsize;
	}

	lc_to_move.file_offset = new_offset;
	fseeko(file, new_offset, SEEK_SET);
	fwrite(lc_to_move.raw_lc, lc_to_move.cmdsize, 1, file);

	load_commands.erase(load_commands.begin() + lc_index);
	load_commands.push_back(lc_to_move);
}

void MachO::insert_load_command(uint32_t arch_index, load_command *raw_lc) {
	MachOArch &arch = archs[arch_index];

	uint32_t offset;
	if(arch.load_commands.size() == 0) {
		offset = arch.fat_arch.offset + arch.fat_arch.size;
	} else {
		LoadCommand &last_lc = arch.load_commands.back();
		offset = (uint32_t)(last_lc.file_offset + last_lc.cmdsize);
	}

	uint32_t magic = arch.mach_header.magic;
	uint32_t cmdsize = SWAP32(raw_lc->cmdsize, magic);

	fseeko(file, offset, SEEK_SET);
	fwrite(raw_lc, cmdsize, 1, file);

	arch.load_commands.push_back(LoadCommand(magic, offset, raw_lc));

	arch.mach_header.ncmds++;
	arch.mach_header.sizeofcmds += cmdsize;

	write_mach_header(arch);
}

void MachO::change_file_type(uint32_t arch_index, uint32_t file_type) {
    MachOArch &arch = archs[arch_index];

    arch.mach_header.filetype = file_type;

    write_mach_header(arch);
}

bool MachO::remove_codesignature(uint32_t arch_index) {
	MachOArch &arch = archs[arch_index];
	uint32_t magic = arch.mach_header.magic;

	LoadCommand *codesig_lc = NULL;
	uint32_t codesig_index = -1;
	LoadCommand *linkedit_lc = NULL;
	LoadCommand *symtab_lc = NULL;

	uint32_t i = 0;
	for(LoadCommand &lc : arch.load_commands) {
		switch(lc.cmd) {
			case LC_CODE_SIGNATURE:
				codesig_lc = &lc;
				codesig_index = i;
				break;
			case LC_SEGMENT:
			case LC_SEGMENT_64: {
				auto *c = (segment_command *)&lc;
				if(strncmp(c->segname, "__LINKEDIT", sizeof(c->segname))) {
					linkedit_lc = &lc;
				}
				break;
			}
			case LC_SYMTAB:
				symtab_lc = &lc;
				break;
		}
		i++;
	}

	if(!codesig_lc || !linkedit_lc) {
		return false;
	}

	linkedit_data_command *codesig_cmd = (linkedit_data_command *)codesig_lc->raw_lc;
	uint32_t codesig_offset = SWAP32(codesig_cmd->dataoff, magic);
	uint32_t codesig_size = SWAP32(codesig_cmd->datasize, magic);

	if(codesig_offset + codesig_size != arch.fat_arch.size) {
		return false;
	}

	uint64_t linkedit_offset;
	uint64_t linkedit_size;
	if(linkedit_lc->cmd == LC_SEGMENT) {
		auto *c = (segment_command *)linkedit_lc->raw_lc;
		linkedit_offset = SWAP32(c->fileoff, magic);
		linkedit_size = SWAP32(c->filesize, magic);
	} else {
		auto *c = (segment_command_64 *)linkedit_lc->raw_lc;
		linkedit_offset = SWAP64(c->fileoff, magic);
		linkedit_size = SWAP64(c->filesize, magic);
	}

	if(linkedit_offset + linkedit_size != arch.fat_arch.size) {
		return false;
	}

	uint32_t size_reduction = codesig_size;

	if(symtab_lc) {
		auto *symtab_cmd = (symtab_command *)symtab_lc->raw_lc;

		uint32_t strsize = SWAP32(symtab_cmd->strsize, magic);
		int64_t diff_size = ((int64_t)arch.fat_arch.size - size_reduction) - (SWAP32(symtab_cmd->stroff, magic) + strsize);

		if(0x0 <= diff_size && diff_size <= 0x10) {
			size_reduction += diff_size;
		}
	}

	arch.fat_arch.size -= size_reduction;
	linkedit_size -= size_reduction;
	uint64_t linkedit_vmsize = ROUND_UP(linkedit_size, 0x1000);

	if(linkedit_lc->cmd == LC_SEGMENT) {
		auto *c = (segment_command *)linkedit_lc->raw_lc;
		c->filesize = SWAP32(linkedit_size, magic);
		c->vmsize = SWAP32(linkedit_vmsize, magic);
	} else {
		auto *c = (segment_command_64 *)linkedit_lc->raw_lc;
		c->filesize = SWAP64(linkedit_size, magic);
		c->vmsize = SWAP64(linkedit_vmsize, magic);
	}

	write_fat_archs();

	write_load_command(*linkedit_lc);

	remove_load_command(arch_index, codesig_index);

	return true;
}
