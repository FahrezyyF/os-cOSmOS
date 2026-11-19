#include "user/shell.h"

/**
 * Every command the shell knows. COMMAND_BUILTIN entries run inside the shell (they change
 * its state); COMMAND_EXTERNAL entries are separate programs in /bin (book 4.3.6), found
 * through PATH, with this built-in copy as a fallback when /bin is unavailable.
 */
#define B COMMAND_BUILTIN
#define X COMMAND_EXTERNAL

const struct Command shell_commands[] = {
    {"ls",       cmd_ls,       "[-l] [-a] [path]...",      "list a directory", X},
    {"cd",       cmd_cd,       "[path | -]",               "change the working directory (.. goes up)", B},
    {"pwd",      cmd_pwd,      "",                         "print the working directory", X},
    {"mkdir",    cmd_mkdir,    "[-p] <dir>...",            "create folders", X},
    {"touch",    cmd_touch,    "<file>...",                "create empty files", X},
    {"cat",      cmd_cat,      "[-n] [file]...",           "print files (or the pipeline input)", X},
    {"cp",       cmd_cp,       "[-r] <src>... <dst>",      "copy files, -r copies folders", X},
    {"rm",       cmd_rm,       "[-r] [-f] <path>...",      "remove files, -r removes folders", X},
    {"mv",       cmd_mv,       "<src>... <dst>",           "move / rename files and folders", X},
    {"find",     cmd_find,     "[dir] <name|pattern>",     "find by name in the whole file system", X},
    {"grep",     cmd_grep,     "[-ivncloFr] <regex> [file]...", "search lines (regular expressions)", X},
    {"tree",     cmd_tree,     "[path]",                   "show a directory tree", X},
    {"stat",     cmd_stat,     "<path>...",                "file details (inode, size, times)", X},
    {"echo",     cmd_echo,     "[-n] [text]...",           "print text", X},
    {"write",    cmd_write,    "<file> [text]...",         "write text (or the pipeline input) to a file", X},
    {"head",     cmd_head,     "[-n N] [file]...",         "first lines", X},
    {"tail",     cmd_tail,     "[-n N] [file]...",         "last lines", X},
    {"wc",       cmd_wc,       "[-lwc] [file]...",         "count lines, words and bytes", X},
    {"sort",     cmd_sort,     "[-r] [-n] [file]...",      "sort lines", X},
    {"hexdump",  cmd_hexdump,  "[file]",                   "hex + ASCII dump", X},
    {"exec",     cmd_exec,     "[-b] <program> [args]...", "run a program as a new process (& or -b: background)", B},
    {"ps",       cmd_ps,       "[-l]",                     "list processes (PID, state, memory, CPU)", X},
    {"kill",     cmd_kill,     "<pid>...",                 "terminate processes (Ctrl+C: the foreground one)", X},
    {"jobs",     cmd_jobs,     "",                         "programs started by this shell", B},
    {"wait",     cmd_wait,     "[pid]",                    "wait for background programs", B},
    {"nice",     cmd_nice,     "<pid> <low|normal|high>",  "change a process' priority (time slice)", X},
    {"pmap",     cmd_pmap,     "[-a] [pid]",               "memory regions and page list of a process", X},
    {"sched",    cmd_sched,    "",                         "scheduler statistics", X},
    {"execinfo", cmd_execinfo, "<program>",                "show a CEX / ELF executable's header", X},
    {"export",   cmd_export,   "[NAME[=value]]...",        "environment variables (saved in /etc/environment)", B},
    {"env",      cmd_env,      "",                         "environment passed to programs", B},
    {"path",     cmd_path,     "[add|remove <dir>]",       "edit PATH, dump its dynamic array", B},
    {"type",     cmd_type,     "<name>...",                "how a command is run (built-in or program)", B},
    {"sh",       cmd_sh,       "<script.sh> [args]...",    "run a shell script (also: ./x.sh, source)", B},
    {"source",   cmd_sh,       "<script.sh> [args]...",    "same as sh", B},
    {"set",      cmd_set,      "[NAME value]",             "list / set shell variables (also NAME=value)", B},
    {"unset",    cmd_unset,    "<NAME>...",                "remove shell variables", B},
    {"history",  cmd_history,  "",                         "previous commands (!! and !n repeat them)", B},
    {"clear",    cmd_clear,    "",                         "clear the screen", B},
    {"help",     cmd_help,     "[command]",                "this list", B},
    {"date",     cmd_date,     "",                         "current date and time (CMOS RTC)", X},
    {"uptime",   cmd_uptime,   "",                         "time since boot", X},
    {"sleep",    cmd_sleep,    "<ms>",                     "wait (the process blocks, others run)", X},
    {"beep",     cmd_beep,     "[Hz] [ms]",                "PC speaker", X},
    {"free",     cmd_free,     "",                         "physical memory, paging and swap", X},
    {"heap",     cmd_heap,     "[check]",                  "user and kernel heap statistics", X},
    {"memtest",  cmd_memtest,  "<MiB>",                    "allocate and verify memory (exercises swap)", X},
    {"sysinfo",  cmd_sysinfo,  "",                         "hardware and kernel summary", X},
    {"df",       cmd_df,       "",                         "file system usage", X},
    {"fsck",     cmd_fsck,     "",                         "check the file system", X},
    {"crash",    cmd_crash,    "[kernel|null|ro|gpf|div]", "fault on purpose (only that process dies)", X},
    {"true",     cmd_true,     "",                         "exit status 0", B},
    {"false",    cmd_false,    "",                         "exit status 1", B},
    {"exit",     cmd_exit,     "[code]",                   "exit the shell (init: the kernel starts a new one)", B},
    {"reboot",   cmd_reboot,   "",                         "restart the machine", X},
    {"shutdown", cmd_shutdown, "",                         "power off", X},
};

const uint32_t shell_command_count = sizeof(shell_commands) / sizeof(shell_commands[0]);
