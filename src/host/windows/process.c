/*
 * The process group: five callbacks over CreateProcess, and the child-side
 * bootstrap that turns a fresh image into a call of the caller's entry point.
 *
 * The shape of this file rests on one fact about what these callbacks are for.
 * "fork" names three unrelated things in this tree, and spawn_cloned /
 * spawn_prepared are the *launch* one: a fresh engine that cold-loads a guest.
 * At the moment they are called there is no guest, no translated code and no
 * worker thread, so nothing warm has to survive. What the child actually needs
 * is a process of its own, a fixed set of handles, and a waitable identity --
 * and CreateProcess plus explicit handle inheritance supplies all three without
 * an address-space clone. (An address-space clone is a real requirement for
 * guest fork(2), which is a different problem and is not implemented here.)
 *
 * Four decisions follow from that, and each has a cost stated where it is paid:
 *
 *   1. The child is a re-execution of *our own image*, discovered with
 *      GetModuleFileNameW. The entry point is carried as an offset from the
 *      module base rather than as an absolute address, so the child recomputes
 *      it against its own base and nothing depends on the two processes being
 *      loaded at the same address.
 *   2. The launch record travels in a pagefile-backed section whose handle is
 *      inherited, and the *numeric handle value* is what the environment
 *      variable carries. Inherited handles occupy the same numeric slot in the
 *      child, which makes a handle value a usable transfer channel; the
 *      environment is used only for that one small integer, because it is the
 *      one channel that exists before the child has run a single instruction.
 *   3. Inheritance is by PROC_THREAD_ATTRIBUTE_HANDLE_LIST, never by "every
 *      inheritable handle". The list form is exact: a handle the parent forgot
 *      to think about cannot leak into the child, and a handle on the list that
 *      is not marked inheritable fails the spawn loudly instead of silently
 *      arriving absent.
 *   4. The child is created suspended so that the parent can stamp the child's
 *      own process id into the record before it runs. The child refuses a
 *      record not addressed to it, which is what keeps a stale environment
 *      variable inherited by some grandchild from hijacking an unrelated
 *      process.
 *
 * The one thing CreateProcess cannot do that fork does: entry_context is a bare
 * pointer, and a fresh process does not have the parent's heap. A context that
 * points at committed parent memory is therefore refused at spawn time with a
 * typed HL_STATUS_NOT_SUPPORTED -- in the parent, before a child exists --
 * rather than handed to a child that would fault on the first dereference. A
 * context that is not an address at all (the scalar-in-a-pointer form) crosses
 * intact, and so does NULL.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../process.h"

/*
 * The child looks for this one variable, consumes it, and deletes it from its
 * own environment before running anything, so no descendant ever sees it.
 */
#define HL_WINDOWS_SPAWN_VARIABLE L"HL_HOST_WINDOWS_SPAWN"
#define HL_WINDOWS_SPAWN_VARIABLE_LENGTH 21u
#define HL_WINDOWS_SPAWN_MAGIC UINT64_C(0x484c53504157314e)

/*
 * Process exit codes this backend mints itself, encoding a Linux signal number
 * in the low byte. 0xE0000000 is the customer-defined NTSTATUS space (severity
 * "error" plus the customer bit), which the system is guaranteed never to use,
 * so these can never be confused with a real exception status or with a value a
 * program returned from main.
 */
#define HL_WINDOWS_EXIT_SIGNAL_BASE 0xE0484C00u
/* A child whose launch record was present but unusable. Deliberately not in the
 * signal space above: it is a spawn failure, not a death. */
#define HL_WINDOWS_EXIT_BOOTSTRAP 255u

/*
 * The launch record. Laid out with explicit widths and no pointers because it is
 * read by a different process; nothing in it is an address in the parent's
 * address space except entry_context, which the spawn path has already proved is
 * not one.
 */
typedef struct hl_windows_spawn_record {
    uint64_t magic;
    uint64_t nonce; /* must equal the value the environment variable carries */
    uint64_t entry_offset;
    uint64_t entry_context;
    uint64_t image_size; /* the parent's SizeOfImage, checked against the child's */
    uint32_t record_size;
    uint32_t child_id; /* stamped after CreateProcess, before ResumeThread */
    volatile LONG claimed;
    uint32_t reserved;
} hl_windows_spawn_record;

/* --- small string helpers ---------------------------------------------------
 * Hand-rolled rather than taken from a library. wsprintfW would do the
 * formatting, but it lives in USER32, which pulls in IMM32, whose thread-detach
 * handler is not safe in every process shape this engine creates -- so the whole
 * backend stays clear of that import and pays ten lines for it here. */

static size_t hl_windows_wide_length(const WCHAR *text) {
    size_t length = 0;
    while (text[length] != L'\0')
        length++;
    return length;
}

static uint32_t hl_windows_format_hex(uint64_t value, WCHAR *out) {
    WCHAR digits[16];
    uint32_t length = 0;
    uint32_t index;
    do {
        const uint32_t nibble = (uint32_t)(value & UINT64_C(0xF));
        digits[length++] = (WCHAR)(nibble < 10u ? (uint32_t)L'0' + nibble : ((uint32_t)L'a' + nibble) - 10u);
        value >>= 4;
    } while (value != 0);
    for (index = 0; index < length; ++index)
        out[index] = digits[length - 1u - index];
    return length;
}

/* Returns 0 on a malformed or empty run of digits; *cursor is left on the first
 * character that is not a hex digit. */
static int hl_windows_parse_hex(const WCHAR *text, uint32_t *cursor, uint64_t *out) {
    uint64_t value = 0;
    uint32_t digits = 0;
    for (;;) {
        const WCHAR character = text[*cursor];
        uint32_t nibble;
        if (character >= L'0' && character <= L'9')
            nibble = (uint32_t)(character - L'0');
        else if (character >= L'a' && character <= L'f')
            nibble = (uint32_t)(character - L'a') + 10u;
        else
            break;
        if (digits >= 16u) return 0;
        value = (value << 4) | (uint64_t)nibble;
        digits++;
        (*cursor)++;
    }
    if (digits == 0) return 0;
    *out = value;
    return 1;
}

/* --- the image ---------------------------------------------------------------
 * SizeOfImage is read out of the loaded PE headers rather than queried, because
 * the only thing it is used for is bounding an offset that must land inside this
 * same module. Both sides read it the same way, so a mismatch means the child is
 * not the image the parent thought it launched. */
static uint64_t hl_windows_image_size(const void *base) {
    const IMAGE_DOS_HEADER *dos = base;
    const IMAGE_NT_HEADERS *headers;
    if (base == NULL || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
    headers = (const IMAGE_NT_HEADERS *)(const void *)((const char *)base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) return 0;
    return (uint64_t)headers->OptionalHeader.SizeOfImage;
}

/* --- the child side ----------------------------------------------------------
 *
 * A constructor rather than an exported function the runner has to call: the
 * child is our own image re-executed, and its main() knows nothing about having
 * been spawned. Running before main and never returning to it is what makes the
 * mechanism invisible to every consumer of this archive that does not spawn.
 *
 * Everything here is a refusal by default. The variable is deleted first, so any
 * process this one goes on to create is unaffected no matter which branch is
 * taken; a record not addressed to this process id is left alone and startup
 * continues normally; a record addressed to this process id that does not check
 * out exits immediately, because that state cannot be recovered from and a
 * silent fall-through to main would be reported to the parent as a plausible
 * exit code.
 */
static void hl_windows_process_bootstrap(void) __attribute__((constructor));

static void hl_windows_process_bootstrap(void) {
    WCHAR text[64];
    hl_windows_spawn_record *record;
    hl_host_process_entry entry;
    HANDLE section;
    uint64_t handle_value = 0;
    uint64_t nonce = 0;
    uint64_t entry_offset;
    uint64_t entry_context;
    uint32_t cursor = 0;
    int32_t code;
    const DWORD length =
        GetEnvironmentVariableW(HL_WINDOWS_SPAWN_VARIABLE, text, (DWORD)(sizeof(text) / sizeof(*text)));
    if (length == 0 || length >= sizeof(text) / sizeof(*text)) return;
    (void)SetEnvironmentVariableW(HL_WINDOWS_SPAWN_VARIABLE, NULL);
    if (!hl_windows_parse_hex(text, &cursor, &handle_value) || text[cursor] != L'.') return;
    cursor++;
    if (!hl_windows_parse_hex(text, &cursor, &nonce) || text[cursor] != L'\0') return;

    /* The handle value is the parent's, and it is meaningful here only because
     * an inherited handle keeps its number. Every field below is checked before
     * anything is called, so a number that is not our section fails a compare
     * instead of being trusted. */
    section = (HANDLE)(uintptr_t)handle_value;
    record = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*record));
    if (record == NULL) ExitProcess(HL_WINDOWS_EXIT_BOOTSTRAP);
    if (record->magic != HL_WINDOWS_SPAWN_MAGIC || record->record_size != (uint32_t)sizeof(*record) ||
        record->nonce != nonce) {
        (void)UnmapViewOfFile(record);
        ExitProcess(HL_WINDOWS_EXIT_BOOTSTRAP);
    }
    /* Addressed to a different process: this is the stale-inheritance case, and
     * the right answer is to leave the process alone. */
    if (record->child_id != GetCurrentProcessId()) {
        (void)UnmapViewOfFile(record);
        return;
    }
    if (InterlockedCompareExchange(&record->claimed, 1, 0) != 0 ||
        record->image_size != hl_windows_image_size(GetModuleHandleW(NULL)) ||
        record->entry_offset >= record->image_size) {
        (void)UnmapViewOfFile(record);
        ExitProcess(HL_WINDOWS_EXIT_BOOTSTRAP);
    }
    entry_offset = record->entry_offset;
    entry_context = record->entry_context;
    (void)UnmapViewOfFile(record);
    (void)CloseHandle(section);

    /* The parent created this process in its own group so that a later
     * terminate() can direct a console control event at it alone. A new group
     * starts with Ctrl+C *ignored*, which would make that event arrive and do
     * nothing, so the default disposition is restored here. */
    (void)SetConsoleCtrlHandler(NULL, FALSE);

    entry = (hl_host_process_entry)(uintptr_t)((uintptr_t)GetModuleHandleW(NULL) + (uintptr_t)entry_offset);
    code = entry((void *)(uintptr_t)entry_context);
    /* Masked to eight bits because that is what _exit() gives the POSIX backends,
     * and a guest exit status is a byte on every host. */
    ExitProcess((UINT)((uint32_t)code & 0xFFu));
}

/* --- the parent side ---------------------------------------------------------
 *
 * Everything one spawn allocates, so that the twelve failure exits below have
 * one place to unwind through instead of twelve copies of the same six frees.
 */
typedef struct hl_windows_spawn_state {
    HANDLE section;
    hl_windows_spawn_record *record;
    WCHAR *image;
    WCHAR *command;
    WCHAR *environment;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes;
    HANDLE inherited[4]; /* the section, then the duplicated standard streams */
    uint32_t inherited_count;
    uint32_t stream_count; /* how many of inherited[] are streams, i.e. owned dups */
} hl_windows_spawn_state;

static void hl_windows_spawn_release(hl_windows_spawn_state *state) {
    uint32_t index;
    for (index = 0; index < state->stream_count; ++index)
        (void)CloseHandle(state->inherited[state->inherited_count - state->stream_count + index]);
    if (state->attributes != NULL) {
        DeleteProcThreadAttributeList(state->attributes);
        free(state->attributes);
    }
    if (state->record != NULL) (void)UnmapViewOfFile(state->record);
    if (state->section != NULL) (void)CloseHandle(state->section);
    free(state->environment);
    free(state->command);
    free(state->image);
    memset(state, 0, sizeof(*state));
}

/* GetModuleFileNameW has no "how long is it" form: it truncates and reports the
 * buffer size it filled, so the only way to know it fit is to see a shorter
 * answer than the buffer. */
static WCHAR *hl_windows_module_path(void) {
    uint32_t capacity = MAX_PATH;
    for (;;) {
        WCHAR *buffer = calloc(capacity, sizeof(*buffer));
        DWORD length;
        if (buffer == NULL) return NULL;
        length = GetModuleFileNameW(NULL, buffer, capacity);
        if (length != 0 && length < capacity) return buffer;
        free(buffer);
        if (capacity >= (1u << 16)) return NULL;
        capacity *= 2u;
    }
}

static int hl_windows_is_spawn_variable(const WCHAR *text) {
    static const WCHAR name[] = HL_WINDOWS_SPAWN_VARIABLE;
    uint32_t index;
    for (index = 0; index < HL_WINDOWS_SPAWN_VARIABLE_LENGTH; ++index)
        if (text[index] != name[index]) return 0;
    return text[HL_WINDOWS_SPAWN_VARIABLE_LENGTH] == L'=';
}

/*
 * The child's environment is built explicitly rather than by setting the
 * variable on ourselves and passing NULL. Two reasons, both real: a process-wide
 * SetEnvironmentVariableW would be visible to every other thread and to any
 * concurrent spawn, and it would leave the marker behind on the parent if the
 * spawn failed between the set and the clear.
 *
 * Any pre-existing marker is dropped on the way through, so a nested engine
 * never carries its parent's record forward.
 */
static WCHAR *hl_windows_child_environment(const WCHAR *addition) {
    const size_t addition_length = hl_windows_wide_length(addition);
    WCHAR *source = GetEnvironmentStringsW();
    WCHAR *block;
    const WCHAR *cursor;
    size_t total = addition_length + 2u; /* the addition's NUL, and the block's */
    size_t offset = 0;
    if (source == NULL) return NULL;
    for (cursor = source; *cursor != L'\0'; cursor += hl_windows_wide_length(cursor) + 1u)
        if (!hl_windows_is_spawn_variable(cursor)) total += hl_windows_wide_length(cursor) + 1u;
    block = calloc(total, sizeof(*block));
    if (block != NULL) {
        for (cursor = source; *cursor != L'\0'; cursor += hl_windows_wide_length(cursor) + 1u) {
            const size_t length = hl_windows_wide_length(cursor) + 1u;
            if (hl_windows_is_spawn_variable(cursor)) continue;
            memcpy(&block[offset], cursor, length * sizeof(*block));
            offset += length;
        }
        memcpy(&block[offset], addition, (addition_length + 1u) * sizeof(*block));
    }
    (void)FreeEnvironmentStringsW(source);
    return block;
}

/*
 * The standard streams, duplicated inheritable. A handle already marked
 * inheritable would work as it stands, but duplicating unconditionally keeps the
 * parent's own handle flags untouched -- flipping HANDLE_FLAG_INHERIT on
 * GetStdHandle(STD_ERROR_HANDLE) would change how every *other* CreateProcess in
 * this process behaves, which is not this function's decision to make.
 *
 * All three or none: STARTF_USESTDHANDLES is all-or-nothing, and a child given
 * two of three streams is a worse outcome than one that inherits the parent's
 * defaults.
 */
static int hl_windows_duplicate_streams(hl_windows_spawn_state *state, STARTUPINFOW *startup) {
    static const DWORD identifiers[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE copies[3] = {NULL, NULL, NULL};
    const HANDLE self = GetCurrentProcess();
    uint32_t index;
    for (index = 0; index < 3u; ++index) {
        const HANDLE source = GetStdHandle(identifiers[index]);
        if (source == NULL || source == INVALID_HANDLE_VALUE ||
            !DuplicateHandle(self, source, self, &copies[index], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            uint32_t undo;
            for (undo = 0; undo < index; ++undo)
                (void)CloseHandle(copies[undo]);
            return 0;
        }
    }
    for (index = 0; index < 3u; ++index)
        state->inherited[state->inherited_count++] = copies[index];
    state->stream_count = 3u;
    startup->dwFlags |= STARTF_USESTDHANDLES;
    startup->hStdInput = copies[0];
    startup->hStdOutput = copies[1];
    startup->hStdError = copies[2];
    return 1;
}

/*
 * Is entry_context something the child can dereference?
 *
 * The child has none of the parent's private memory, so the answer is yes only
 * when the value is not an address at all. VirtualQuery is the exact test: a
 * committed page is one the parent can read and the child cannot, and anything
 * else -- free, reserved-but-uncommitted, or outside the address space entirely
 * -- is a scalar that a caller stuffed into a pointer, which crosses by value.
 *
 * Refusing here, in the parent, is the whole point. The alternative is a child
 * that faults on its first dereference and a wait() that reports a segmentation
 * fault the caller has no way to attribute.
 */
static int hl_windows_context_crosses(void *entry_context) {
    MEMORY_BASIC_INFORMATION information;
    if (entry_context == NULL) return 1;
    if (VirtualQuery(entry_context, &information, sizeof(information)) == 0) return 1;
    return information.State != MEM_COMMIT;
}

static uint64_t hl_windows_spawn_nonce(void) {
    static volatile LONG64 counter;
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) now.QuadPart = 0;
    return ((uint64_t)GetCurrentProcessId() << 32) ^ (uint64_t)now.QuadPart ^
           (uint64_t)InterlockedIncrement64(&counter) * UINT64_C(0x9E3779B97F4A7C15);
}

static hl_host_result hl_windows_process_launch(hl_host_windows *host, hl_host_process_entry entry,
                                                void *entry_context) {
    hl_windows_spawn_state state;
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES security;
    WCHAR variable[HL_WINDOWS_SPAWN_VARIABLE_LENGTH + 36u];
    hl_host_result result;
    hl_windows_handle_entry *slot;
    SIZE_T attribute_size = 0;
    const void *base = GetModuleHandleW(NULL);
    const uint64_t image_size = hl_windows_image_size(base);
    const uint64_t entry_offset = (uint64_t)((uintptr_t)entry - (uintptr_t)base);
    uint32_t length;
    size_t command_size;

    if (entry == NULL) return hl_windows_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    /* An entry point outside this image cannot be named by an offset from its
     * base, and this backend has no other way to name code to a fresh process. */
    if (image_size == 0 || entry_offset >= image_size) return hl_windows_result(HL_STATUS_NOT_SUPPORTED, 0, 0);
    if (!hl_windows_context_crosses(entry_context)) return hl_windows_result(HL_STATUS_NOT_SUPPORTED, 0, 0);

    memset(&state, 0, sizeof(state));
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.StartupInfo.cb = (DWORD)sizeof(startup);

    security.nLength = (DWORD)sizeof(security);
    security.lpSecurityDescriptor = NULL;
    security.bInheritHandle = TRUE;
    state.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(*state.record), NULL);
    if (state.section == NULL) {
        result = hl_windows_last_error_result();
        hl_windows_spawn_release(&state);
        return result;
    }
    state.record = MapViewOfFile(state.section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*state.record));
    state.image = hl_windows_module_path();
    if (state.record == NULL || state.image == NULL) {
        result =
            state.record == NULL ? hl_windows_last_error_result() : hl_windows_result(HL_STATUS_OUT_OF_MEMORY, 0, 0);
        hl_windows_spawn_release(&state);
        return result;
    }
    memset(state.record, 0, sizeof(*state.record));
    state.record->magic = HL_WINDOWS_SPAWN_MAGIC;
    state.record->nonce = hl_windows_spawn_nonce();
    state.record->entry_offset = entry_offset;
    state.record->entry_context = (uint64_t)(uintptr_t)entry_context;
    state.record->image_size = image_size;
    state.record->record_size = (uint32_t)sizeof(*state.record);

    /* CreateProcessW may write to lpCommandLine, so it gets its own copy even
     * though the two strings are identical here. argv[0] is the image path,
     * which is what a re-executed engine would see anyway. */
    command_size = hl_windows_wide_length(state.image) + 1u;
    state.command = calloc(command_size, sizeof(*state.command));
    memcpy(&variable[0], HL_WINDOWS_SPAWN_VARIABLE, HL_WINDOWS_SPAWN_VARIABLE_LENGTH * sizeof(*variable));
    length = HL_WINDOWS_SPAWN_VARIABLE_LENGTH;
    variable[length++] = L'=';
    length += hl_windows_format_hex((uint64_t)(uintptr_t)state.section, &variable[length]);
    variable[length++] = L'.';
    length += hl_windows_format_hex(state.record->nonce, &variable[length]);
    variable[length] = L'\0';
    if (state.command != NULL) {
        memcpy(state.command, state.image, command_size * sizeof(*state.command));
        state.environment = hl_windows_child_environment(variable);
    }
    if (state.command == NULL || state.environment == NULL) {
        hl_windows_spawn_release(&state);
        return hl_windows_result(HL_STATUS_OUT_OF_MEMORY, 0, 0);
    }

    state.inherited[state.inherited_count++] = state.section;
    (void)hl_windows_duplicate_streams(&state, &startup.StartupInfo);

    /* The documented two-call form: the first call always fails, and its purpose
     * is to report the size. */
    if (InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        result = hl_windows_last_error_result();
        hl_windows_spawn_release(&state);
        return result;
    }
    state.attributes = malloc(attribute_size);
    if (state.attributes == NULL) {
        hl_windows_spawn_release(&state);
        return hl_windows_result(HL_STATUS_OUT_OF_MEMORY, 0, 0);
    }
    if (!InitializeProcThreadAttributeList(state.attributes, 1, 0, &attribute_size)) {
        result = hl_windows_last_error_result();
        free(state.attributes);
        state.attributes = NULL;
        hl_windows_spawn_release(&state);
        return result;
    }
    if (!UpdateProcThreadAttribute(state.attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, state.inherited,
                                   (SIZE_T)state.inherited_count * sizeof(state.inherited[0]), NULL, NULL)) {
        result = hl_windows_last_error_result();
        hl_windows_spawn_release(&state);
        return result;
    }
    startup.lpAttributeList = state.attributes;

    /*
     * bInheritHandles is TRUE *and* the list is present: the flag alone inherits
     * every inheritable handle, and the list alone does nothing without the flag.
     * Together they mean "these and only these".
     *
     * CREATE_NEW_PROCESS_GROUP is what makes terminate(INTERRUPT) addressable --
     * a console control event names a process group, and without a group of its
     * own the child could only be reached by an event that also hit this process.
     * The cost is that the child starts with Ctrl+C ignored, which the child-side
     * bootstrap undoes.
     */
    if (!CreateProcessW(state.image, state.command, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP |
                            EXTENDED_STARTUPINFO_PRESENT,
                        state.environment, NULL, &startup.StartupInfo, &process)) {
        result = hl_windows_last_error_result();
        hl_windows_spawn_release(&state);
        return result;
    }
    /* Stamped while the child is still suspended, so it is visible to the first
     * instruction the child runs and to nothing before it. */
    state.record->child_id = process.dwProcessId;

    result = hl_windows_allocate_handle(host, HL_WINDOWS_HANDLE_PROCESS);
    if (result.status == HL_STATUS_OK && ResumeThread(process.hThread) == (DWORD)-1)
        result = hl_windows_last_error_result();
    (void)CloseHandle(process.hThread);
    if (result.status != HL_STATUS_OK) {
        (void)TerminateProcess(process.hProcess, HL_WINDOWS_EXIT_SIGNAL_BASE + 9u);
        (void)CloseHandle(process.hProcess);
        hl_windows_spawn_release(&state);
        return result;
    }
    hl_windows_lock(host);
    slot = hl_windows_lookup_locked(host, result.value, HL_WINDOWS_HANDLE_PROCESS);
    if (slot != NULL) {
        slot->object = process.hProcess;
        slot->process_id = process.dwProcessId;
    }
    hl_windows_unlock(host);
    if (slot == NULL) {
        (void)TerminateProcess(process.hProcess, HL_WINDOWS_EXIT_SIGNAL_BASE + 9u);
        (void)CloseHandle(process.hProcess);
        hl_windows_spawn_release(&state);
        return hl_windows_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    /* The parent's section handle and view go here rather than at close: the
     * child holds an inherited reference of its own, so the object outlives this
     * release, and holding a second handle per live child for no reason would be
     * a leak with a long half-life. */
    hl_windows_spawn_release(&state);
    return result;
}

/* --- exit status -------------------------------------------------------------
 *
 * Windows reports an unhandled exception as the process exit code, so the exit
 * code is the only channel a signal-shaped death can arrive on. Only codes that
 * are unambiguously an unhandled exception status are read that way; anything
 * else is reported as an exit code, because a program is free to return any
 * 32-bit value from main and guessing would corrupt the ordinary case.
 */
static int hl_windows_exit_signal(DWORD code, uint64_t *signal_number) {
    if (code > HL_WINDOWS_EXIT_SIGNAL_BASE && code <= HL_WINDOWS_EXIT_SIGNAL_BASE + 64u) {
        *signal_number = code - HL_WINDOWS_EXIT_SIGNAL_BASE;
        return 1;
    }
    switch (code) {
    case 0xC0000005u: /* ACCESS_VIOLATION */
    case 0xC000008Cu: /* ARRAY_BOUNDS_EXCEEDED */
    case 0xC00000FDu: /* STACK_OVERFLOW */ *signal_number = 11u; return 1;
    case 0x80000002u: /* DATATYPE_MISALIGNMENT */
    case 0xC0000006u: /* IN_PAGE_ERROR */ *signal_number = 7u; return 1;
    case 0x80000003u: /* BREAKPOINT */
    case 0x80000004u: /* SINGLE_STEP */ *signal_number = 5u; return 1;
    case 0xC000001Du: /* ILLEGAL_INSTRUCTION */
    case 0xC0000025u: /* NONCONTINUABLE_EXCEPTION */
    case 0xC0000026u: /* INVALID_DISPOSITION */
    case 0xC0000096u: /* PRIVILEGED_INSTRUCTION */ *signal_number = 4u; return 1;
    case 0xC000008Du: /* FLOAT_DENORMAL_OPERAND */
    case 0xC000008Eu: /* FLOAT_DIVIDE_BY_ZERO */
    case 0xC000008Fu: /* FLOAT_INEXACT_RESULT */
    case 0xC0000090u: /* FLOAT_INVALID_OPERATION */
    case 0xC0000091u: /* FLOAT_OVERFLOW */
    case 0xC0000092u: /* FLOAT_STACK_CHECK */
    case 0xC0000093u: /* FLOAT_UNDERFLOW */
    case 0xC0000094u: /* INTEGER_DIVIDE_BY_ZERO */
    case 0xC0000095u: /* INTEGER_OVERFLOW */ *signal_number = 8u; return 1;
    case 0x40010005u: /* DBG_CONTROL_C */
    case 0xC000013Au: /* CONTROL_C_EXIT */ *signal_number = 2u; return 1;
    case 0xC0000409u: /* STACK_BUFFER_OVERRUN */
    case 0xC0000602u: /* FAIL_FAST_EXCEPTION */ *signal_number = 6u; return 1;
    default: return 0;
    }
}

static hl_host_result hl_windows_process_retained(const hl_windows_handle_entry *entry) {
    return hl_windows_result(HL_STATUS_OK, entry->process_exit_value, entry->process_exit_kind);
}

static uint64_t hl_windows_monotonic_now(hl_host_windows *host) {
    const hl_host_result now = hl_windows_clock_services.monotonic_ns(host);
    return now.status == HL_STATUS_OK ? now.value : 0;
}

/*
 * deadline_ns is absolute on the host monotonic clock, so it is converted to a
 * relative millisecond timeout on each pass and the loop re-checks the clock
 * after every WAIT_TIMEOUT. Rounding up and re-checking is what keeps the call
 * from returning before the deadline: a bare (deadline - now) / 1e6 would
 * truncate and could report WOULD_BLOCK up to a millisecond early.
 */
static DWORD hl_windows_process_wait_for(hl_host_windows *host, HANDLE object, uint64_t deadline_ns) {
    for (;;) {
        DWORD timeout = INFINITE;
        DWORD waited;
        if (deadline_ns != HL_HOST_DEADLINE_INFINITE) {
            const uint64_t now = hl_windows_monotonic_now(host);
            const uint64_t remaining = now >= deadline_ns ? 0u : (deadline_ns - now + 999999u) / 1000000u;
            timeout = remaining >= (uint64_t)INFINITE ? INFINITE - 1u : (DWORD)remaining;
        }
        waited = WaitForSingleObject(object, timeout);
        if (waited != WAIT_TIMEOUT || deadline_ns == HL_HOST_DEADLINE_INFINITE) return waited;
        if (deadline_ns == 0 || hl_windows_monotonic_now(host) >= deadline_ns) return WAIT_TIMEOUT;
    }
}

static hl_host_result hl_windows_process_wait(void *context, hl_host_handle handle, uint64_t deadline_ns) {
    hl_host_windows *host = context;
    hl_windows_handle_entry *entry;
    HANDLE object;
    DWORD waited;
    DWORD code = 0;
    hl_host_result result;

    hl_windows_lock(host);
    entry = hl_windows_lookup_locked(host, handle, HL_WINDOWS_HANDLE_PROCESS);
    if (entry == NULL || host->destroying) {
        hl_windows_unlock(host);
        return hl_windows_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    if ((entry->process_state & HL_WINDOWS_PROCESS_REAPED) != 0) {
        result = hl_windows_process_retained(entry);
        hl_windows_unlock(host);
        return result;
    }
    /* The waiter count is what close() refuses on, so the native handle can be
     * used outside the lock without a duplicate: no close can retire it while
     * this count is non-zero. */
    entry->process_waiters++;
    object = entry->object;
    hl_windows_unlock(host);

    waited = hl_windows_process_wait_for(host, object, deadline_ns);
    if (waited == WAIT_OBJECT_0 && !GetExitCodeProcess(object, &code)) waited = WAIT_FAILED;

    hl_windows_lock(host);
    entry = hl_windows_lookup_locked(host, handle, HL_WINDOWS_HANDLE_PROCESS);
    if (entry != NULL) {
        if (entry->process_waiters != 0) entry->process_waiters--;
        /* First waiter home records the completion; every later one -- and every
         * concurrent one that woke on the same exit -- reads that record, so all
         * of them return the identical value and kind. */
        if (waited == WAIT_OBJECT_0 && (entry->process_state & HL_WINDOWS_PROCESS_REAPED) == 0) {
            uint64_t signal_number = 0;
            if (hl_windows_exit_signal(code, &signal_number)) {
                entry->process_exit_kind = HL_HOST_PROCESS_EXIT_SIGNAL;
                entry->process_exit_value = signal_number;
            } else {
                entry->process_exit_kind = HL_HOST_PROCESS_EXIT_CODE;
                entry->process_exit_value = code;
            }
            entry->process_state |= HL_WINDOWS_PROCESS_REAPED;
        }
    }
    if (waited == WAIT_OBJECT_0 && entry != NULL) result = hl_windows_process_retained(entry);
    hl_windows_unlock(host);

    if (waited == WAIT_OBJECT_0) return entry != NULL ? result : hl_windows_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (waited == WAIT_TIMEOUT) return hl_windows_result(HL_STATUS_WOULD_BLOCK, 0, 0);
    return hl_windows_last_error_result();
}

/*
 * What Windows can express, and what it cannot.
 *
 * INTERRUPT is a console control event, which is a genuine equivalent: the child
 * is asked to stop, it can install a handler and decline, and if it does not it
 * dies with CONTROL_C_EXIT -- which wait() reports as SIGINT. FORCE is
 * TerminateProcess, which is exactly SIGKILL's contract: immediate, unmaskable,
 * no handler runs.
 *
 * The signal form is accepted for those same two numbers and refused for every
 * other one, with HL_STATUS_NOT_SUPPORTED. That refusal is the honest answer.
 * TerminateProcess is not SIGTERM: SIGTERM is a request a process may catch,
 * block, or use to run its shutdown path, and reporting "delivered" for a call
 * that instead destroyed the process would be a lie the caller cannot detect.
 * Windows has no mechanism that delivers an arbitrary catchable signal to
 * another process, so there is nothing to map it onto.
 */
static hl_host_result hl_windows_process_terminate(void *context, hl_host_handle handle, uint32_t reason) {
    hl_host_windows *host = context;
    hl_windows_handle_entry *entry;
    HANDLE object;
    DWORD process_id;
    int force;
    if (reason != HL_HOST_PROCESS_TERMINATE_INTERRUPT && reason != HL_HOST_PROCESS_TERMINATE_FORCE &&
        (reason <= HL_HOST_PROCESS_TERMINATE_SIGNAL || reason > HL_HOST_PROCESS_TERMINATE_SIGNAL + 64u))
        return hl_windows_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    if (reason == HL_HOST_PROCESS_TERMINATE_FORCE || reason == HL_HOST_PROCESS_TERMINATE_SIGNAL + 9u)
        force = 1;
    else if (reason == HL_HOST_PROCESS_TERMINATE_INTERRUPT || reason == HL_HOST_PROCESS_TERMINATE_SIGNAL + 2u)
        force = 0;
    else
        return hl_windows_result(HL_STATUS_NOT_SUPPORTED, 0, reason - HL_HOST_PROCESS_TERMINATE_SIGNAL);

    hl_windows_lock(host);
    entry = hl_windows_lookup_locked(host, handle, HL_WINDOWS_HANDLE_PROCESS);
    if (entry == NULL || host->destroying || (entry->process_state & HL_WINDOWS_PROCESS_REAPED) != 0) {
        hl_windows_unlock(host);
        return hl_windows_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    /* Held across the call rather than borrowed: TerminateProcess on a stale
     * handle would be a signal delivered to whatever process id got reused, and
     * close() is the only thing this lock excludes. */
    object = entry->object;
    process_id = entry->process_id;
    if (force) {
        if (!TerminateProcess(object, HL_WINDOWS_EXIT_SIGNAL_BASE + 9u)) {
            hl_windows_unlock(host);
            return hl_windows_last_error_result();
        }
    } else if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, process_id)) {
        hl_windows_unlock(host);
        return hl_windows_last_error_result();
    }
    hl_windows_unlock(host);
    return hl_windows_result(HL_STATUS_OK, 0, 0);
}

/*
 * Retiring the slot is also what releases the retained completion, so close is
 * refused until there is one and until no waiter is still inside wait(). That is
 * the same rule the POSIX backends apply, and for the same reason: a close that
 * ran first would leave a concurrent waiter holding a handle to a reaped process
 * with nowhere to record its answer.
 */
static hl_host_result hl_windows_process_close(void *context, hl_host_handle handle) {
    hl_host_windows *host = context;
    hl_windows_handle_entry *entry;
    HANDLE object;
    hl_windows_lock(host);
    entry = hl_windows_lookup_locked(host, handle, HL_WINDOWS_HANDLE_PROCESS);
    if (entry == NULL) {
        hl_windows_unlock(host);
        return hl_windows_result(HL_STATUS_INVALID_ARGUMENT, 0, 0);
    }
    if (host->destroying || (entry->process_state & HL_WINDOWS_PROCESS_REAPED) == 0 || entry->process_waiters != 0) {
        hl_windows_unlock(host);
        return hl_windows_result(HL_STATUS_BUSY, 0, 0);
    }
    object = entry->object;
    hl_windows_clear_entry_locked(entry);
    hl_windows_unlock(host);
    if (object != NULL && !CloseHandle(object)) return hl_windows_last_error_result();
    return hl_windows_result(HL_STATUS_OK, 0, 0);
}

/*
 * spawn_cloned takes the host's fork bracket itself; spawn_prepared is called
 * with that bracket already held by the caller and is required to complete it.
 * On this host the bracket is the sync registry's lock and there is nothing to
 * quiesce -- a fresh process inherits no locks -- but it is still taken and
 * released honestly, because a caller that used it to serialise something of its
 * own is entitled to that serialisation.
 *
 * A bracket that fails to complete after the child already exists is settled by
 * retiring the child: the caller is being told the spawn failed, so it will
 * never wait on the handle, and a process nobody will reap is worse than a
 * process that never ran.
 */
static hl_host_result hl_windows_process_settle(hl_host_windows *host, hl_host_result spawned,
                                                hl_host_result completed) {
    if (spawned.status != HL_STATUS_OK) return spawned;
    if (completed.status == HL_STATUS_OK) return spawned;
    (void)hl_windows_process_terminate(host, spawned.value, HL_HOST_PROCESS_TERMINATE_FORCE);
    (void)hl_windows_process_wait(host, spawned.value, HL_HOST_DEADLINE_INFINITE);
    (void)hl_windows_process_close(host, spawned.value);
    return completed;
}

static hl_host_result hl_windows_process_spawn_cloned(void *context, hl_host_process_entry entry, void *entry_context) {
    hl_host_windows *host = context;
    const hl_host_result armed = hl_host_sync_fork_prepare(host->sync);
    hl_host_result spawned;
    if (armed.status != HL_STATUS_OK) return armed;
    spawned = hl_windows_process_launch(host, entry, entry_context);
    return hl_windows_process_settle(host, spawned, hl_host_sync_fork_complete(host->sync));
}

static hl_host_result hl_windows_process_spawn_prepared(void *context, hl_host_process_entry entry,
                                                        void *entry_context) {
    hl_host_windows *host = context;
    const hl_host_result spawned = hl_windows_process_launch(host, entry, entry_context);
    return hl_windows_process_settle(host, spawned, hl_host_sync_fork_complete(host->sync));
}

const hl_host_process_services hl_windows_process_services = {.abi = HL_HOST_PROCESS_ABI,
                                                              .size = sizeof(hl_host_process_services),
                                                              .spawn_cloned = hl_windows_process_spawn_cloned,
                                                              .wait = hl_windows_process_wait,
                                                              .terminate = hl_windows_process_terminate,
                                                              .close = hl_windows_process_close,
                                                              .spawn_prepared = hl_windows_process_spawn_prepared};

/*
 * A pidfd-shaped watch on another process.
 *
 * REFUSAL, and the reason is the readiness boundary rather than the watch. The
 * contract is a close-on-exec DESCRIPTOR that becomes persistently readable when
 * the process exits -- Linux's pidfd_open(2). Windows has the watchable object
 * (a process HANDLE is signalled on exit, permanently, which is the same
 * edge-then-level shape) but no way to present it as a descriptor that joins the
 * engine's readiness set, because that set is a mixed one and this host has no
 * single call that waits on a mixed set. The same absence host_poll.h records.
 *
 * The typed process group already exposes wait-for-exit over the handle it
 * created, so a caller that wants a child's status has a supported path; what is
 * refused here is specifically the poll-alongside-everything-else form.
 */
int hl_host_process_open(pid_t pid) {
    (void)pid;
    errno = ENOSYS;
    return -1;
}
