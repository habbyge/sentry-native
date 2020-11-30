#include "sentry_modulefinder_linux.h"

#include "sentry_core.h"
#include "sentry_path.h"
#include "sentry_string.h"
#include "sentry_sync.h"
#include "sentry_value.h"

#include <arpa/inet.h>
#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define MAX_MAPPINGS 5

#define ENSURE(Ptr)                                                            \
    if (!Ptr)                                                                  \
    goto fail

static bool g_initialized = false;
static sentry_mutex_t g_mutex = SENTRY__MUTEX_INIT;
static sentry_value_t g_modules = { 0 };

static sentry_slice_t LINUX_GATE = { "linux-gate.so", 13 };

/**
 * Checks that `start_offset` + `size` is a valid contiguous mapping in the
 * mapped regions, and returns the translated pointer corresponding to
 * `start_offset`.
 */
void *
sentry__module_get_addr(
    const sentry_module_t *module, uint64_t start_offset, uint64_t size)
{
    uint64_t addr = 0;
    uint64_t addr_end = UINT64_MAX;
    for (size_t i = 0; i < module->num_mappings; i++) {
        const sentry_mapped_region_t *mapping = &module->mappings[i];
        // we have a gap and can’t fit a contiguous range
        if (addr && addr_end < mapping->addr) {
            return NULL;
        }
        addr_end = mapping->addr + mapping->size;
        // if the start_offset is inside this mapping, create our addr
        if (start_offset >= mapping->offset
            && start_offset < mapping->offset + mapping->size) {
            addr = start_offset - mapping->offset + mapping->addr;
        }
        if (addr && addr + size <= addr_end) {
            return (void *)(uintptr_t)(addr);
        }
    }
    return NULL;
}

static void
sentry__module_mapping_push(
    sentry_module_t *module, const sentry_parsed_module_t *parsed)
{
    size_t size = parsed->end - parsed->start;
    if (module->num_mappings) {
        sentry_mapped_region_t *last_mapping
            = &module->mappings[module->num_mappings - 1];
        if (last_mapping->addr + last_mapping->size == parsed->start
            && last_mapping->offset + last_mapping->size == parsed->offset) {
            last_mapping->size += size;
            return;
        }
    }
    if (module->num_mappings < MAX_MAPPINGS) {
        sentry_mapped_region_t *mapping
            = &module->mappings[module->num_mappings++];
        mapping->offset = parsed->offset;
        mapping->size = size;
        mapping->addr = parsed->start;
    }
}

int
sentry__procmaps_parse_module_line(
    const char *line, sentry_parsed_module_t *module)
{
    uint8_t major_device;
    uint8_t minor_device;
    uint64_t inode;
    int consumed = 0;

    // this has been copied from the breakpad source:
    // https://github.com/google/breakpad/blob/13c1568702e8804bc3ebcfbb435a2786a3e335cf/src/processor/proc_maps_linux.cc#L66
    if (sscanf(line,
            "%" SCNx64 "-%" SCNx64 " %4c %" SCNx64 " %hhx:%hhx %" SCNu64 " %n",
            &module->start, &module->end, &module->permissions[0],
            &module->offset, &major_device, &minor_device, &inode, &consumed)
        < 7) {
        return 0;
    }

    // copy the filename up to a newline
    line += consumed;
    module->file.ptr = line;
    module->file.len = 0;
    char *nl = strchr(line, '\n');
    // `consumed` skips over whitespace (the trailing newline), so we have to
    // check for that explicitly
    if (consumed && (line - 1)[0] == '\n') {
        module->file.ptr = NULL;
    } else if (nl) {
        module->file.len = nl - line;
        consumed += nl - line + 1;
    } else {
        module->file.len = strlen(line);
        consumed += module->file.len;
    }

    // and return the consumed chars…
    return consumed;
}

void
align(size_t alignment, void **offset)
{
    size_t diff = (size_t)*offset % alignment;
    if (diff != 0) {
        *(size_t *)offset += alignment - diff;
    }
}

static const uint8_t *
get_code_id_from_notes(
    size_t alignment, void *start, void *end, size_t *size_out)
{
    *size_out = 0;
    if (alignment < 4) {
        alignment = 4;
    } else if (alignment != 4 && alignment != 8) {
        return NULL;
    }

    const uint8_t *offset = start;
    while (offset < (const uint8_t *)end) {
        // The note header size is independent of the architecture, so we just
        // use the `Elf64_Nhdr` variant.
        const Elf64_Nhdr *note = (const Elf64_Nhdr *)offset;
        // the headers are consecutive, and the optional `name` and `desc` are
        // saved inline after the header.

        offset += sizeof(Elf64_Nhdr);
        offset += note->n_namesz;
        align(alignment, (void **)&offset);
        if (note->n_type == NT_GNU_BUILD_ID) {
            *size_out = note->n_descsz;
            return offset;
        }
        offset += note->n_descsz;
        align(alignment, (void **)&offset);
    }
    return NULL;
}

static bool
is_elf_module(const sentry_module_t *module)
{
    // we try to interpret `addr` as an ELF file, which should start with a
    // magic number...
    const unsigned char *e_ident
        = sentry__module_get_addr(module, 0, EI_NIDENT);
    if (!e_ident) {
        return false;
    }
    return e_ident[EI_MAG0] == ELFMAG0 && e_ident[EI_MAG1] == ELFMAG1
        && e_ident[EI_MAG2] == ELFMAG2 && e_ident[EI_MAG3] == ELFMAG3;
}

static const uint8_t *
get_code_id_from_elf(const sentry_module_t *module, size_t *size_out)
{
    *size_out = 0;

    // iterate over all the program headers, for 32/64 bit separately
    const unsigned char *e_ident
        = sentry__module_get_addr(module, 0, EI_NIDENT);
    ENSURE(e_ident);
    if (e_ident[EI_CLASS] == ELFCLASS64) {
        const Elf64_Ehdr *elf
            = sentry__module_get_addr(module, 0, sizeof(Elf64_Ehdr));
        ENSURE(elf);
        for (int i = 0; i < elf->e_phnum; i++) {
            const Elf64_Phdr *header = sentry__module_get_addr(
                module, elf->e_phoff + elf->e_phentsize * i, elf->e_phentsize);
            ENSURE(header);

            // we are only interested in notes
            if (header->p_type != PT_NOTE) {
                continue;
            }
            void *segment_addr = sentry__module_get_addr(
                module, header->p_offset, header->p_filesz);
            ENSURE(segment_addr);
            const uint8_t *code_id = get_code_id_from_notes(header->p_align,
                segment_addr,
                (void *)((uintptr_t)segment_addr + header->p_filesz), size_out);
            if (code_id) {
                return code_id;
            }
        }
    } else {
        const Elf32_Ehdr *elf
            = sentry__module_get_addr(module, 0, sizeof(Elf32_Ehdr));
        ENSURE(elf);

        for (int i = 0; i < elf->e_phnum; i++) {
            const Elf32_Phdr *header = sentry__module_get_addr(
                module, elf->e_phoff + elf->e_phentsize * i, elf->e_phentsize);
            ENSURE(header);
            // we are only interested in notes
            if (header->p_type != PT_NOTE) {
                continue;
            }
            void *segment_addr = sentry__module_get_addr(
                module, header->p_offset, header->p_filesz);
            ENSURE(segment_addr);
            const uint8_t *code_id = get_code_id_from_notes(header->p_align,
                segment_addr,
                (void *)((uintptr_t)segment_addr + header->p_filesz), size_out);
            if (code_id) {
                return code_id;
            }
        }
    }
fail:
    return NULL;
}

static sentry_uuid_t
get_code_id_from_text_fallback(const sentry_module_t *module)
{
    const uint8_t *text = NULL;
    size_t text_size = 0;

    // iterate over all the program headers, for 32/64 bit separately
    const unsigned char *e_ident
        = sentry__module_get_addr(module, 0, EI_NIDENT);
    ENSURE(e_ident);
    if (e_ident[EI_CLASS] == ELFCLASS64) {
        const Elf64_Ehdr *elf
            = sentry__module_get_addr(module, 0, sizeof(Elf64_Ehdr));
        ENSURE(elf);
        const Elf64_Shdr *strheader = sentry__module_get_addr(module,
            elf->e_shoff + elf->e_shentsize * elf->e_shstrndx,
            elf->e_shentsize);
        ENSURE(strheader);

        const char *names = sentry__module_get_addr(
            module, strheader->sh_offset, strheader->sh_entsize);
        ENSURE(names);
        for (int i = 0; i < elf->e_shnum; i++) {
            const Elf64_Shdr *header = sentry__module_get_addr(
                module, elf->e_shoff + elf->e_shentsize * i, elf->e_shentsize);
            ENSURE(header);
            const char *name = names + header->sh_name;
            if (header->sh_type == SHT_PROGBITS && strcmp(name, ".text") == 0) {
                text = sentry__module_get_addr(
                    module, header->sh_offset, header->sh_size);
                ENSURE(text);
                text_size = header->sh_size;
                break;
            }
        }
    } else {
        const Elf32_Ehdr *elf
            = sentry__module_get_addr(module, 0, sizeof(Elf64_Ehdr));
        ENSURE(elf);
        const Elf32_Shdr *strheader = sentry__module_get_addr(module,
            elf->e_shoff + elf->e_shentsize * elf->e_shstrndx,
            elf->e_shentsize);
        ENSURE(strheader);

        const char *names = sentry__module_get_addr(
            module, strheader->sh_offset, strheader->sh_entsize);
        ENSURE(names);
        for (int i = 0; i < elf->e_shnum; i++) {
            const Elf32_Shdr *header = sentry__module_get_addr(
                module, elf->e_shoff + elf->e_shentsize * i, elf->e_shentsize);
            ENSURE(header);
            const char *name = names + header->sh_name;
            if (header->sh_type == SHT_PROGBITS && strcmp(name, ".text") == 0) {
                text = sentry__module_get_addr(
                    module, header->sh_offset, header->sh_size);
                ENSURE(text);
                text_size = header->sh_size;
                break;
            }
        }
    }

    sentry_uuid_t uuid = sentry_uuid_nil();

    // adapted from
    // https://github.com/getsentry/symbolic/blob/8f9a01756e48dcbba2e42917a064f495d74058b7/debuginfo/src/elf.rs#L100-L110
    size_t max = MIN(text_size, 4096);
    for (size_t i = 0; i < max; i++) {
        uuid.bytes[i % 16] ^= text[i];
    }

    return uuid;
fail:
    return sentry_uuid_nil();
}

bool
sentry__procmaps_read_ids_from_elf(
    sentry_value_t value, const sentry_module_t *module)
{
    // and try to get the debug id from the elf headers of the loaded
    // modules
    size_t code_id_size;
    const uint8_t *code_id = get_code_id_from_elf(module, &code_id_size);
    sentry_uuid_t uuid = sentry_uuid_nil();
    if (code_id) {
        sentry_value_set_by_key(value, "code_id",
            sentry__value_new_hexstring(code_id, code_id_size));

        memcpy(uuid.bytes, code_id, MIN(code_id_size, 16));
    } else {
        uuid = get_code_id_from_text_fallback(module);
    }

    // the usage of these is described here:
    // https://getsentry.github.io/symbolicator/advanced/symbol-server-compatibility/#identifiers
    // in particular, the debug_id is a `little-endian GUID`, so we have to do
    // appropriate byte-flipping
    char *uuid_bytes = uuid.bytes;
    uint32_t *a = (uint32_t *)uuid_bytes;
    *a = htonl(*a);
    uint16_t *b = (uint16_t *)(uuid_bytes + 4);
    *b = htons(*b);
    uint16_t *c = (uint16_t *)(uuid_bytes + 6);
    *c = htons(*c);

    sentry_value_set_by_key(value, "debug_id", sentry__value_new_uuid(&uuid));
    return true;
}

sentry_value_t
sentry__procmaps_module_to_value(const sentry_module_t *module)
{
    if (!is_elf_module(module)) {
        return sentry_value_new_null();
    }
    sentry_value_t mod_val = sentry_value_new_object();
    sentry_value_set_by_key(mod_val, "type", sentry_value_new_string("elf"));

    sentry_value_set_by_key(mod_val, "image_addr",
        sentry__value_new_addr(module->mappings[0].addr));
    const sentry_mapped_region_t *last_mapping
        = &module->mappings[module->num_mappings - 1];
    sentry_value_set_by_key(mod_val, "image_size",
        sentry_value_new_int32(last_mapping->offset + last_mapping->size));
    sentry_value_set_by_key(mod_val, "code_file",
        sentry__value_new_string_owned(sentry__slice_to_owned(module->file)));

    sentry__procmaps_read_ids_from_elf(mod_val, module);

    return mod_val;
}

static void
try_append_module(sentry_value_t modules, const sentry_module_t *module)
{
    if (!module->file.ptr) {
        return;
    }

    sentry_value_t mod_val = sentry__procmaps_module_to_value(module);
    if (!sentry_value_is_null(mod_val)) {
        sentry_value_append(modules, mod_val);
    }
}

// copied from:
// https://github.com/google/breakpad/blob/216cea7bca53fa441a3ee0d0f5fd339a3a894224/src/client/linux/minidump_writer/linux_dumper.h#L61-L70
#if defined(__i386) || defined(__ARM_EABI__)                                   \
    || (defined(__mips__) && _MIPS_SIM == _ABIO32)
typedef Elf32_auxv_t elf_aux_entry;
#elif defined(__x86_64) || defined(__aarch64__)                                \
    || (defined(__mips__) && _MIPS_SIM != _ABIO32)
typedef Elf64_auxv_t elf_aux_entry;
#endif

// See http://man7.org/linux/man-pages/man7/vdso.7.html
static uint64_t
get_linux_vdso(void)
{
    // this is adapted from:
    // https://github.com/google/breakpad/blob/79ba6a494fb2097b39f76fe6a4b4b4f407e32a02/src/client/linux/minidump_writer/linux_dumper.cc#L548-L572

    int fd = open("/proc/self/auxv", O_RDONLY);
    if (fd < 0) {
        return false;
    }

    elf_aux_entry one_aux_entry;
    while (
        read(fd, &one_aux_entry, sizeof(elf_aux_entry)) == sizeof(elf_aux_entry)
        && one_aux_entry.a_type != AT_NULL) {
        if (one_aux_entry.a_type == AT_SYSINFO_EHDR) {
            close(fd);
            return (uint64_t)one_aux_entry.a_un.a_val;
        }
    }
    close(fd);
    return 0;
}

static void
load_modules(sentry_value_t modules)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        return;
    }

    // just read the whole map at once, maybe do it line-by-line as a followup…
    char buf[4096];
    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);
    while (true) {
        ssize_t n = read(fd, buf, 4096);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        } else if (n <= 0) {
            break;
        }
        if (sentry__stringbuilder_append_buf(&sb, buf, n)) {
            sentry__stringbuilder_cleanup(&sb);
            close(fd);
            return;
        }
    }
    close(fd);

    char *contents = sentry__stringbuilder_into_string(&sb);
    if (!contents) {
        return;
    }
    char *current_line = contents;

    uint64_t linux_vdso = get_linux_vdso();

    // we have multiple memory maps per file, and we need to merge their offsets
    // based on the filename. Luckily, the maps are ordered by filename, so yay
    sentry_module_t last_module = { 0 };
    while (true) {
        sentry_parsed_module_t module = { 0 };
        int read = sentry__procmaps_parse_module_line(current_line, &module);
        current_line += read;
        if (!read) {
            break;
        }

        // for the vdso, we use the special filename `linux-gate.so`,
        // otherwise we check that we have a valid pathname (with a `/` inside),
        // and skip over things that end in `)`, because entries marked as
        // `(deleted)` might crash when dereferencing, trying to check if its
        // a valid elf file.
        char *slash;
        if (module.start && module.start == linux_vdso) {
            module.file = LINUX_GATE;
        } else if (!module.start || !module.file.len
            || module.permissions[0] != 'r'
            || module.file.ptr[module.file.len - 1] == ')'
            || (slash = strchr(module.file.ptr, '/')) == NULL
            || slash > module.file.ptr + module.file.len
            || (module.file.len >= 5
                && memcmp("/dev/", module.file.ptr, 5) == 0)) {
            continue;
        }

        if (last_module.file.len
            && !sentry__slice_eq(last_module.file, module.file)) {
            try_append_module(modules, &last_module);

            memset(&last_module, 0, sizeof(sentry_module_t));
        }
        last_module.file = module.file;
        sentry__module_mapping_push(&last_module, &module);
    }
    try_append_module(modules, &last_module);
    sentry_free(contents);
}

sentry_value_t
sentry_get_modules_list(void)
{
    sentry__mutex_lock(&g_mutex);
    if (!g_initialized) {
        g_modules = sentry_value_new_list();
        SENTRY_TRACE("trying to read modules from /proc/self/maps");
        load_modules(g_modules);
        SENTRY_TRACEF("read %zu modules from /proc/self/maps",
            sentry_value_get_length(g_modules));
        sentry_value_freeze(g_modules);
        g_initialized = true;
    }
    sentry_value_t modules = g_modules;
    sentry_value_incref(modules);
    sentry__mutex_unlock(&g_mutex);
    return modules;
}

void
sentry_clear_modulecache(void)
{
    sentry__mutex_lock(&g_mutex);
    sentry_value_decref(g_modules);
    g_modules = sentry_value_new_null();
    g_initialized = false;
    sentry__mutex_unlock(&g_mutex);
}
