// The bot processes of the C++ ports of CodinGame referees, driven like the engine's runner drives its
// agents (com.codingame.gameengine.runner Agent / CommandLinePlayerAgent / AsynchronousWriter). All the
// process handling of the referees lives here, once: a referee never creates, reads or kills a process
// itself.
//
//   cg::Agent a;                              // the bot dies with the object at the latest
//   a.start("path/to/bot.exe", options);      // false: could not be launched
//   a.sendInput("1 2\n3\n");                  // queued, never blocks
//   std::string out;
//   bool ok = a.getOutput(2, 50, false, out); // 2 lines within 50 ms, the engine's way
//   a.readError();                            // drains the bot's stderr like the runner does
//   a.destroy();                              // kills the bot and every process it started
//
// - No threads: the input queued for the bots is written while the referee waits for an output
//   (overlapped I/O on Windows, non-blocking pipes and poll on POSIX), like the runner's writer
//   threads write it while it waits.
// - A bot never outlives the referee, however the referee ends (return, exit, crash, killed): on
//   Windows each bot runs in a job object that kills its processes when the referee's handle to it
//   closes; on POSIX each bot leads its own process group, which a small watchdog process kills once
//   the referee is gone. destroy() also kills the whole job / group, so what a bot starts (`cmd /c`,
//   `sh -c`, battle's `wrap capture`) goes with it.
// - A bot inherits its three standard handles and nothing else of the referee.
// - Windows and POSIX (Linux, macOS).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace cg {

// What happens to what a bot writes to its stderr
enum class Stderr {
    Discard, // thrown away
    // Like the jar: after each execution of the bot the runner reads at most 4096 bytes of its stderr
    // (Agent#readError), from a pipe of 4096 + 24 bytes on Windows (java.lang.ProcessImpl), through a
    // BufferedInputStream of 8192 bytes on POSIX. A bot writing more than that per turn blocks on its
    // stderr and times out, as it does with the jar.
    Jar,
    Inherit, // the referee's own stderr (debugging)
    File,    // written to AgentOptions::stderrFile (debugging)
};

struct AgentOptions {
    Stderr stderrMode = Stderr::Discard;
    std::string stderrFile; // with Stderr::File
    // The jar launches the bots, then spends 0.2-1 s initialising its referee before it sends them
    // their first input, so a slow bot startup hardly counts in its first turn. With a head start,
    // the first turn of a bot ends at the latest of `timeout` after its first input and
    // `headStart + timeout` after its launch (0: the time limit counts from the first input only).
    int firstTurnHeadStartMs = 0;
};

// AGENT_MAX_BUFFER_SIZE: the most bytes the runner reads from a bot in one turn
const int AGENT_MAX_BUFFER_SIZE = 10000;

#ifndef _WIN32
namespace detail {
// The POSIX watchdog: a process of its own, outside the referee's process group, that kills the
// process groups of the bots when the referee is gone. It reads the groups to watch (+pgid) and to
// forget (-pgid) from the lifeline pipe, whose only writer is the referee: end of file means the
// referee ended, whatever the way (even SIGKILL, or a kill of the referee's whole group).
struct Watchdog {
    int lifeline = -1; // the write end, close-on-exec
    bool started = false;

    void start() {
        if (started) return;
        started = true;
        int p[2];
        if (pipe(p) != 0) return;
        int maxFd = openMax();
        pid_t w = fork();
        if (w < 0) {
            close(p[0]);
            close(p[1]);
            return;
        }
        if (w == 0) run(p[0], p[1], maxFd);
        close(p[0]);
        fcntl(p[1], F_SETFD, FD_CLOEXEC);
        lifeline = p[1];
    }

    // async-signal-safe (called by a bot between fork and exec)
    void send(int32_t v) const {
        if (lifeline < 0) return;
        ssize_t n;
        do {
            n = write(lifeline, &v, sizeof v); // atomic: less than PIPE_BUF
        } while (n < 0 && errno == EINTR);
    }
    void watch(pid_t pgid) const { send((int32_t)pgid); }
    void forget(pid_t pgid) const { send(-(int32_t)pgid); }

    static int openMax() {
        long m = sysconf(_SC_OPEN_MAX);
        return m < 0 || m > 65536 ? 65536 : (int)m;
    }

    [[noreturn]] static void run(int rd, int wr, int maxFd) {
        close(wr);
        setpgid(0, 0); // out of the referee's group: a kill of that group spares the watchdog
        // nothing of the referee stays open (battle reads the referee's stdout until its end)
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
        }
        for (int fd = 3; fd < maxFd; fd++)
            if (fd != rd) close(fd);
        signal(SIGINT, SIG_IGN); // a Ctrl+C in the referee's terminal: the referee ends, then the bots
        signal(SIGHUP, SIG_IGN);
        pid_t groups[256];
        int n = 0;
        int32_t v;
        while (true) {
            ssize_t r = read(rd, &v, sizeof v);
            if (r < 0 && errno == EINTR) continue;
            if (r != (ssize_t)sizeof v) break;
            if (v > 0) {
                if (n < 256) groups[n++] = v;
            } else {
                for (int i = 0; i < n; i++)
                    if (groups[i] == -v) {
                        groups[i] = groups[--n];
                        break;
                    }
            }
        }
        for (int i = 0; i < n; i++) kill(-groups[i], SIGKILL);
        _exit(0);
    }
};
inline Watchdog& watchdog() {
    static Watchdog w;
    return w;
}
} // namespace detail
#endif

class Agent {
  public:
    std::chrono::steady_clock::time_point startTime; // when the process was created

    Agent() = default;
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    ~Agent() { destroy(); }

    // Launches a command line. A command with shell metacharacters (&|<>^()) runs through the shell
    // (cmd /c, sh -c), '/' becomes '\' on Windows, and on POSIX the words are split like a shell
    // does (wordexp). False when the process could not be created, or (POSIX) its program could not
    // be executed.
    bool start(const std::string& cmd, const AgentOptions& options) {
        destroy();
        opt = options;
        if (!spawn(cmd)) {
            closeAll();
            return false;
        }
        startTime = std::chrono::steady_clock::now();
        running = true;
        outputOpen = true;
        live().push_back(this);
        return true;
    }

    // GameRunner queues each input for an AsynchronousWriter: the referee never blocks on a bot that
    // does not read, and a write error (the bot exited) only stops the writing.
    void sendInput(const std::string& bytes) {
        if (!running || inputClosed || bytes.empty()) return;
        pendingInput += bytes;
        pumpInput();
    }

    // Agent#getOutput + GameRunner#checkOutput: reads until `nbLine` line ends ('\n', '\r'; "\r\n"
    // counts once, even split across turns) or AGENT_MAX_BUFFER_SIZE bytes, within `timeoutMs`
    // (from now; see AgentOptions::firstTurnHeadStartMs for `firstTurn`). Bytes after the last line
    // stay buffered for the next turn. Returns false when the runner reports a timeout: not all lines
    // in time, the bot's stdout closed, or too long.
    bool getOutput(int nbLine, long long timeoutMs, bool firstTurn, std::string& out) {
        out.clear();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        if (firstTurn && opt.firstTurnHeadStartMs > 0)
            deadline = std::max(deadline, startTime + std::chrono::milliseconds(opt.firstTurnHeadStartMs + timeoutMs));
        int nbOccurences = 0;
        while ((int)out.size() < AGENT_MAX_BUFFER_SIZE && nbOccurences < nbLine) {
            if (readPos >= readBuf.size()) {
                readBuf.clear();
                readPos = 0;
                if (!fill(deadline)) break;
                continue;
            }
            char curByte = readBuf[readPos++];
            if (!(curByte == '\n' && lastCR)) {
                out += curByte;
                if (curByte == '\n' || curByte == '\r') ++nbOccurences;
            }
            lastCR = curByte == '\r';
        }
        return nbOccurences == nbLine;
    }

    // For referees that read their bots line by line instead of like the engine's runner
    enum class Line {
        Ok,      // `line` is the next line, without its "\n" or "\r\n"; at the end of the bot's stdout,
                 // the bytes after the last "\n" make a last line
        Timeout, // no complete line within `timeoutMs` (what arrived stays buffered)
        Closed,  // the bot's stdout ended and nothing is left
    };
    Line readLine(std::string& line, long long timeoutMs) {
        line.clear();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            size_t nl = readBuf.find('\n', readPos);
            if (nl != std::string::npos) {
                line = readBuf.substr(readPos, nl - readPos);
                readPos = nl + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return Line::Ok;
            }
            if (std::chrono::steady_clock::now() >= deadline) return Line::Timeout;
            readBuf.erase(0, readPos);
            readPos = 0;
            if (!fill(deadline)) {
                if (outputOpen) return Line::Timeout;
                if (readBuf.empty()) return Line::Closed;
                line.swap(readBuf);
                readBuf.clear();
                if (line.back() == '\r') line.pop_back();
                return Line::Ok;
            }
        }
    }

    // Whether the bot's stdout ended
    bool closed() const { return !outputOpen; }

    // Agent#readError (with Stderr::Jar; nothing to do otherwise)
    void readError() {
        if (!running || opt.stderrMode != Stderr::Jar) return;
#ifdef _WIN32
        // a FileInputStream: one read of at most 4096 bytes when some are available
        DWORD avail = 0;
        if (!errorPipe || !PeekNamedPipe(errorPipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
        char tmp[4096];
        DWORD rd = 0;
        ReadFile(errorPipe, tmp, avail < sizeof(tmp) ? avail : (DWORD)sizeof(tmp), &rd, nullptr);
#else
        // read(tmp, 0, 4096) on a BufferedInputStream(8192) (ProcessPipeInputStream)
        if ((errBuf.size() - errPos) + (size_t)stderrAvailable() == 0) return;
        size_t want = 4096, got = 0;
        while (true) {
            if (errPos >= errBuf.size()) {
                errBuf.assign(8192, 0);
                errPos = 0;
                ssize_t n;
                do {
                    n = ::read(errorFd, &errBuf[0], errBuf.size());
                } while (n < 0 && errno == EINTR);
                errBuf.resize(n > 0 ? (size_t)n : 0);
                if (n <= 0) break;
            }
            size_t take = std::min(errBuf.size() - errPos, want - got);
            errPos += take;
            got += take;
            if (got >= want || stderrAvailable() <= 0) break;
        }
#endif
    }

    // Process.destroy(), for the whole job / process group: the bot and every process it started
    // die, then everything of the bot is released. Nothing happens for a bot that is not running.
    void destroy() {
        if (!running) return;
        running = false;
        outputOpen = false;
        auto& l = live();
        l.erase(std::remove(l.begin(), l.end(), this), l.end());
#ifdef _WIN32
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(process, 1);
        WaitForSingleObject(process, 5000);
        if (writing) {
            // the bot is gone, so the write ends; cancel it anyway before its buffer goes
            CancelIoEx(inputPipe, &writeOv);
            DWORD n;
            GetOverlappedResult(inputPipe, &writeOv, &n, TRUE);
            writing = false;
        }
#else
        kill(-pid, SIGKILL); // the group of the bot (the bot's pid): the zombie keeps it from reuse
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
        detail::watchdog().forget(pid);
        pid = -1;
#endif
        closeAll();
    }

  private:
    AgentOptions opt;
    bool running = false;    // started and not destroyed
    bool outputOpen = false; // its stdout is not at its end
    std::string readBuf;     // bytes read but not consumed yet
    size_t readPos = 0;
    bool lastCR = false;
    std::string pendingInput; // queued, not written yet
    bool inputClosed = false; // the bot's stdin is closed: the input is dropped

    // the running agents, whose input is written while any of them is waited for (never destroyed:
    // agents with static storage may be destroyed after it)
    static std::vector<Agent*>& live() {
        static std::vector<Agent*>& agents = *new std::vector<Agent*>();
        return agents;
    }
    static void pumpAll() {
        for (Agent* a : live()) a->pumpInput();
    }

#ifdef _WIN32
    HANDLE job = nullptr;        // the bot and its descendants; kills them when closed
    HANDLE process = nullptr;
    HANDLE inputPipe = nullptr;  // overlapped: written without blocking
    HANDLE outputPipe = nullptr; // overlapped: read with a precise timeout
    HANDLE errorPipe = nullptr;  // Stderr::Jar only
    HANDLE readEvent = nullptr;  // manual-reset events of the overlapped reads and writes
    HANDLE writeEvent = nullptr;
    OVERLAPPED writeOv{};
    std::string writeBuf; // the bytes of the write in progress
    bool writing = false;

    void closeAll() {
        for (HANDLE* h : {&job, &process, &inputPipe, &outputPipe, &errorPipe, &readEvent, &writeEvent}) {
            if (*h) CloseHandle(*h);
            *h = nullptr;
        }
        writing = false;
        writeBuf.clear();
    }

    // A pipe between the referee (the overlapped end it returns) and the bot (`child`, inheritable),
    // with the access rights CreatePipe gives
    static HANDLE createPipe(bool toChild, HANDLE* child) {
        char name[96];
        static LONG counter = 0;
        snprintf(name, sizeof(name), "\\\\.\\pipe\\cg_agent_%lu_%ld", (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&counter));
        HANDLE h = CreateNamedPipeA(name, (toChild ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        DWORD access = toChild ? (GENERIC_READ | FILE_WRITE_ATTRIBUTES) : (GENERIC_WRITE | FILE_READ_ATTRIBUTES);
        *child = CreateFileA(name, access, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (*child == INVALID_HANDLE_VALUE) {
            *child = nullptr;
            CloseHandle(h);
            return nullptr;
        }
        return h;
    }

    // the bot's stderr, inheritable
    HANDLE createStderr() {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE h = nullptr;
        if (opt.stderrMode == Stderr::Jar) {
            if (!CreatePipe(&errorPipe, &h, &sa, 4096 + 24)) return nullptr;
            SetHandleInformation(errorPipe, HANDLE_FLAG_INHERIT, 0);
            return h;
        }
        if (opt.stderrMode == Stderr::File) {
            h = CreateFileA(opt.stderrFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        } else if (opt.stderrMode == Stderr::Inherit) {
            HANDLE own = GetStdHandle(STD_ERROR_HANDLE);
            if (own && own != INVALID_HANDLE_VALUE && !DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &h, 0, TRUE, DUPLICATE_SAME_ACCESS))
                h = nullptr;
        }
        if (!h || h == INVALID_HANDLE_VALUE)
            h = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
        return h == INVALID_HANDLE_VALUE ? nullptr : h;
    }

    bool spawn(const std::string& cmd) {
        HANDLE childIn = nullptr, childOut = nullptr, childErr = nullptr;
        auto closeChild = [&] {
            for (HANDLE h : {childIn, childOut, childErr})
                if (h) CloseHandle(h);
        };
        inputPipe = createPipe(true, &childIn);
        outputPipe = createPipe(false, &childOut);
        childErr = createStderr();
        readEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        writeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!inputPipe || !outputPipe || !childErr || !readEvent || !writeEvent) {
            closeChild();
            return false;
        }
        // kills the bot's processes when closed: by destroy(), or by Windows when the referee ends;
        // a crash of the bot ends it at once (no error report dialog to wait for)
        job = CreateJobObjectA(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
                CloseHandle(job);
                job = nullptr;
            }
        }
        // the bot inherits its three standard handles, nothing else
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<char> attrBuf(attrSize);
        LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
        HANDLE inherited[3] = {childIn, childOut, childErr};
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) {
            closeChild();
            return false;
        }
        bool ok = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
        STARTUPINFOEXA si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = childIn;
        si.StartupInfo.hStdOutput = childOut;
        si.StartupInfo.hStdError = childErr;
        si.lpAttributeList = attrs;
        // '/' -> '\' so cmd doesn't mistake path separators for option prefixes
        std::string full = cmd;
        for (auto& ch : full)
            if (ch == '/') ch = '\\';
        if (cmd.find_first_of("&|<>^()") != std::string::npos) full = "cmd /c \"" + full + "\"";
        std::vector<char> cmdline(full.begin(), full.end());
        cmdline.push_back(0);
        PROCESS_INFORMATION pi{};
        // suspended until it is in the job, so that nothing it starts escapes the job
        ok = ok && CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                  nullptr, nullptr, &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attrs);
        closeChild();
        if (!ok) return false;
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
            // (not possible since Windows 8) destroy() then kills the bot's process only
            CloseHandle(job);
            job = nullptr;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        return true;
    }

    // Moves the queued input on: collects the write in progress when it is over, then starts the next
    void pumpInput() {
        if (writing) {
            DWORD n;
            if (!GetOverlappedResult(inputPipe, &writeOv, &n, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) return;
                inputClosed = true;
            }
            writing = false;
            writeBuf.clear();
        }
        if (inputClosed) pendingInput.clear();
        if (pendingInput.empty()) return;
        writeBuf.swap(pendingInput);
        pendingInput.clear();
        writeOv = OVERLAPPED{};
        writeOv.hEvent = writeEvent;
        ResetEvent(writeEvent);
        if (WriteFile(inputPipe, writeBuf.data(), (DWORD)writeBuf.size(), nullptr, &writeOv) || GetLastError() == ERROR_IO_PENDING) {
            writing = true; // done or not, GetOverlappedResult tells
        } else {
            inputClosed = true;
            writeBuf.clear();
        }
    }

    // Reads more bytes into readBuf, writing the queued input of every bot meanwhile. False on
    // timeout, end of file or error.
    bool fill(std::chrono::steady_clock::time_point deadline) {
        if (!outputOpen || std::chrono::steady_clock::now() >= deadline) return false;
        pumpAll();
        char tmp[4096];
        OVERLAPPED ov{};
        ov.hEvent = readEvent;
        ResetEvent(readEvent);
        DWORD rd = 0;
        if (!ReadFile(outputPipe, tmp, (DWORD)sizeof(tmp), &rd, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                outputOpen = false;
                return false;
            }
            while (true) {
                HANDLE events[MAXIMUM_WAIT_OBJECTS];
                DWORD n = 0;
                events[n++] = readEvent;
                for (Agent* a : live())
                    if (a->writing && n < MAXIMUM_WAIT_OBJECTS) events[n++] = a->writeEvent;
                auto now = std::chrono::steady_clock::now();
                long long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                DWORD w = now >= deadline ? WAIT_TIMEOUT : WaitForMultipleObjects(n, events, FALSE, (DWORD)std::max(1LL, left));
                if (w == WAIT_OBJECT_0) break;
                if (w > WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + n) {
                    pumpAll(); // a write is over: the next one starts
                    continue;
                }
                // time is up (or the wait failed): cancel the read; what it got before the
                // cancellation stays buffered, as it would stay in the jar's pipe
                CancelIoEx(outputPipe, &ov);
                DWORD got = 0;
                if (GetOverlappedResult(outputPipe, &ov, &got, TRUE) && got > 0) readBuf.append(tmp, got);
                if (w != WAIT_TIMEOUT) outputOpen = false;
                return false;
            }
            if (!GetOverlappedResult(outputPipe, &ov, &rd, FALSE)) {
                outputOpen = false;
                return false;
            }
        }
        if (rd == 0) {
            outputOpen = false;
            return false;
        }
        readBuf.append(tmp, rd);
        return true;
    }
#else
    pid_t pid = -1; // also the bot's process group
    int inputFd = -1;  // non-blocking
    int outputFd = -1;
    int errorFd = -1;   // Stderr::Jar only
    std::string errBuf; // the BufferedInputStream of the bot's stderr
    size_t errPos = 0;

    void closeAll() {
        for (int* fd : {&inputFd, &outputFd, &errorFd}) {
            if (*fd >= 0) close(*fd);
            *fd = -1;
        }
        errBuf.clear();
        errPos = 0;
    }

    static bool cloexecPipe(int p[2]) {
        if (pipe(p) != 0) return false;
        fcntl(p[0], F_SETFD, FD_CLOEXEC);
        fcntl(p[1], F_SETFD, FD_CLOEXEC);
        return true;
    }

    // closes the descriptors from `from` on, but `keep` (async-signal-safe)
    static void closeFrom(int from, int keep, int maxFd) {
#if defined(__linux__) && defined(SYS_close_range)
        if (syscall(SYS_close_range, from, keep - 1, 0) == 0 && syscall(SYS_close_range, keep + 1, ~0U, 0) == 0) return;
#endif
        for (int fd = from; fd < maxFd; fd++)
            if (fd != keep) close(fd);
    }

    bool spawn(const std::string& cmd) {
        // everything the child needs is prepared here: between fork and exec it only makes system calls
        std::vector<std::string> args;
        if (cmd.find_first_of("&|<>^()") != std::string::npos) {
            args = {"/bin/sh", "-c", cmd};
        } else {
            wordexp_t w{};
            if (wordexp(cmd.c_str(), &w, WRDE_NOCMD) != 0) return false;
            for (size_t i = 0; i < w.we_wordc; i++) args.push_back(w.we_wordv[i]);
            wordfree(&w);
            if (args.empty()) return false;
        }
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(&a[0]);
        argv.push_back(nullptr);

        signal(SIGPIPE, SIG_IGN); // a write to a bot that exited fails instead of killing the referee
        detail::Watchdog& wd = detail::watchdog();
        wd.start(); // before any pipe of a bot exists, so that it holds none
        int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, status[2] = {-1, -1};
        int childErr = -1; // -1: the referee's stderr (Stderr::Inherit)
        auto closeFds = [&] {
            for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1], status[0], status[1]})
                if (fd >= 0) close(fd);
            if (childErr >= 0 && childErr != err[1]) close(childErr);
        };
        if (!cloexecPipe(in) || !cloexecPipe(out) || !cloexecPipe(status)) {
            closeFds();
            return false;
        }
        if (opt.stderrMode == Stderr::Jar) {
            if (!cloexecPipe(err)) {
                closeFds();
                return false;
            }
            childErr = err[1];
        } else if (opt.stderrMode == Stderr::Discard) {
            childErr = open("/dev/null", O_WRONLY | O_CLOEXEC);
        } else if (opt.stderrMode == Stderr::File) {
            childErr = open(opt.stderrFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
            if (childErr < 0) childErr = open("/dev/null", O_WRONLY | O_CLOEXEC);
        }
        int maxFd = detail::Watchdog::openMax();
        pid_t c = fork();
        if (c == 0) {
            // the bot: its own process group, watched before it can start anything
            setpgid(0, 0);
            wd.watch(getpid());
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = SIG_DFL;
            sigaction(SIGPIPE, &sa, nullptr);
            dup2(in[0], STDIN_FILENO);
            dup2(out[1], STDOUT_FILENO);
            if (childErr >= 0) dup2(childErr, STDERR_FILENO);
            closeFrom(3, status[1], maxFd); // the exec status pipe closes on exec
            execvp(argv[0], argv.data());
            int e = errno;
            ssize_t n = write(status[1], &e, sizeof(e));
            (void)n;
            _exit(127);
        }
        if (c < 0) {
            closeFds();
            return false;
        }
        for (int* fd : {&in[0], &out[1], &err[1], &status[1]}) {
            if (*fd >= 0) close(*fd);
            *fd = -1;
        }
        if (childErr >= 0 && opt.stderrMode != Stderr::Jar) close(childErr);
        childErr = -1;
        // the end of the status pipe: the program runs; an errno: exec failed
        int e = 0;
        ssize_t n;
        do {
            n = ::read(status[0], &e, sizeof(e));
        } while (n < 0 && errno == EINTR);
        close(status[0]);
        status[0] = -1;
        if (n > 0) {
            int st;
            while (waitpid(c, &st, 0) < 0 && errno == EINTR) {
            }
            wd.forget(c);
            closeFds();
            return false;
        }
        fcntl(in[1], F_SETFL, fcntl(in[1], F_GETFL) | O_NONBLOCK);
        inputFd = in[1];
        outputFd = out[0];
        errorFd = err[0];
        pid = c;
        return true;
    }

    // Writes as much of the queued input as the pipe takes
    void pumpInput() {
        while (!pendingInput.empty() && !inputClosed) {
            ssize_t n = write(inputFd, pendingInput.data(), pendingInput.size());
            if (n > 0) {
                pendingInput.erase(0, (size_t)n);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            } else {
                inputClosed = true;
            }
        }
        if (inputClosed) pendingInput.clear();
    }

    // Reads more bytes into readBuf, writing the queued input of every bot meanwhile. False on
    // timeout, end of file or error.
    bool fill(std::chrono::steady_clock::time_point deadline) {
        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (!outputOpen || now >= deadline) return false;
            int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (ms < 1) ms = 1;
            pumpAll();
            std::vector<struct pollfd> fds;
            std::vector<Agent*> writers;
            fds.push_back({outputFd, POLLIN, 0});
            for (Agent* a : live())
                if (!a->pendingInput.empty()) {
                    fds.push_back({a->inputFd, POLLOUT, 0});
                    writers.push_back(a);
                }
            int pr = poll(fds.data(), (nfds_t)fds.size(), ms);
            if (pr < 0 && errno == EINTR) continue;
            if (pr < 0) {
                outputOpen = false;
                return false;
            }
            if (pr == 0) return false; // timeout
            for (size_t i = 1; i < fds.size(); i++)
                if (fds[i].revents) writers[i - 1]->pumpInput();
            if (fds[0].revents == 0) continue;
            char tmp[4096];
            ssize_t rd;
            do {
                rd = ::read(outputFd, tmp, sizeof(tmp));
            } while (rd < 0 && errno == EINTR);
            if (rd <= 0) {
                outputOpen = false;
                return false;
            }
            readBuf.append(tmp, (size_t)rd);
            return true;
        }
    }

    int stderrAvailable() const {
        int n = 0;
        if (errorFd < 0 || ioctl(errorFd, FIONREAD, &n) < 0) return 0;
        return n;
    }
#endif
};

} // namespace cg
